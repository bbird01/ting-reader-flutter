part of '../player_state.dart';

// EndpointFormFactor: Headphones(3), Headset(5), Handset(6).
const Set<int> _personalOutputFormFactors = {3, 5, 6};
const List<String> _personalOutputNameHints = [
  '蓝牙',
  'bluetooth',
  '耳机',
  '耳麦',
  'headphone',
  'headset',
  'airpods',
  'hands-free',
  '免提',
];

bool _isPersonalOutput(
    ({String id, String name, int formFactor, String enumerator})? info) {
  if (info == null) return false;
  if (_personalOutputFormFactors.contains(info.formFactor)) return true;
  // 蓝牙音频端点（BTHENUM/BTHHFENUM）即使上报扬声器形态也属于私有输出。
  final enumerator = info.enumerator.toLowerCase();
  if (enumerator == 'bthenum' || enumerator == 'bthhfenum') return true;
  final name = info.name.toLowerCase();
  return _personalOutputNameHints.any(name.contains);
}

/// Windows: libmpv 一直绑定启动时选中的输出端点，默认播放设备变化后会继续
/// 向失效端点推进进度而没有声音。这里轮询 Windows 默认播放端点：
/// 蓝牙耳机等私有输出断开时立即暂停（绝不切到扬声器继续播放），
/// 端点稳定后重建播放器让其重新绑定新端点；耳机重连后自动续播。
extension _PlayerOutputRoute on PlayerState {
  void _startOutputRouteWatch() {
    if (kIsWeb || defaultTargetPlatform != TargetPlatform.windows) return;
    _outputRouteTimer ??= Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => unawaited(_checkOutputRoute()),
    );
  }

  Future<void> _checkOutputRoute() async {
    if (_disposed ||
        _outputRouteChecking ||
        _outputRoutePending ||
        _outputRouteChanging) {
      return;
    }
    if (currentBook == null || currentChapter == null) {
      _lastOutputInfo = null;
      _outputRebindRetry = false;
      return;
    }
    _outputRouteChecking = true;
    ({String id, String name, int formFactor, String enumerator})? info;
    try {
      info = await DesktopWindow.instance.audioOutputInfo();
    } finally {
      _outputRouteChecking = false;
    }
    if (_disposed) return;
    final previous = _lastOutputInfo;
    if (info == null) {
      // 没有可用的默认输出（例如蓝牙断开后系统短暂无端点）：只要原先在
      // 私有输出上播放，就立即暂停，避免"进度在走但没有声音"。
      if (_isPersonalOutput(previous) && _audio.playing) {
        _resumeWhenOutputReturns = true;
        _pauseForDisconnectedOutput();
      }
      return;
    }
    if (previous == null) {
      // 首次观测只建立基线：此时播放器刚加载完章节，不能当成设备切换去重建，
      // 否则会把启动续播重开成暂停。
      _lastOutputInfo = info;
      return;
    }
    if (previous.id == info.id) {
      if (!_outputRebindRetry) return;
      // 上一轮需要重建但被推迟（例如正在起播），用当前播放状态重试。
      _outputRebindRetry = false;
      _outputRoutePending = true;
      try {
        final handled = await _rebindAudioOutput(
          resumePlaying:
              _resumeWhenOutputReturns || isPlaying || _audio.playing,
        );
        _outputRebindRetry = !handled;
        if (handled && _resumeWhenOutputReturns) {
          _resumeWhenOutputReturns = false;
        }
      } finally {
        _outputRoutePending = false;
      }
      return;
    }
    // 先记录，避免设备切换期间每个轮询周期都重复暂停/上报。
    _lastOutputInfo = info;
    final lostPersonalOutput =
        _isPersonalOutput(previous) && !_isPersonalOutput(info);
    final wasPlaying = isPlaying || _audio.playing;
    if (lostPersonalOutput && wasPlaying) {
      _resumeWhenOutputReturns = true;
      _pauseForDisconnectedOutput();
    }
    final resumeOnReturn =
        !lostPersonalOutput && _resumeWhenOutputReturns && _isPersonalOutput(info);
    // 蓝牙端点重连时会连续触发多次切换，等设备稳定后再重建。
    _outputRoutePending = true;
    try {
      await Future<void>.delayed(const Duration(milliseconds: 800));
      if (_disposed) return;
      final latest = await DesktopWindow.instance.audioOutputInfo();
      if (_disposed || latest == null) return;
      if (latest.id != info.id) {
        // 设备在等待期间又变回去了：播放器仍然绑定原端点，无需重建。
        if (latest.id == previous.id) {
          _lastOutputInfo = latest;
        }
        return;
      }
      // 起播期间 isPlaying 可能滞后于原生播放器，重建时以最新状态为准。
      final handled = await _rebindAudioOutput(
        resumePlaying: resumeOnReturn ||
            (wasPlaying && !lostPersonalOutput) ||
            (!lostPersonalOutput && (isPlaying || _audio.playing)),
      );
      _lastOutputInfo = latest;
      _outputRebindRetry = !handled;
      if (handled && resumeOnReturn) _resumeWhenOutputReturns = false;
    } finally {
      _outputRoutePending = false;
    }
  }

  Future<bool> _rebindAudioOutput({required bool resumePlaying}) async {
    if (_disposed || _outputRouteChanging || _connectingPlayback) return false;
    final book = currentBook;
    final chapter = currentChapter;
    if (book == null || chapter == null || chapters.isEmpty) return true;
    if (_audio.processingState == audio.ProcessingState.completed) return true;
    if (!resumePlaying &&
        _audio.processingState == audio.ProcessingState.idle) {
      // 尚未绑定输出端点，下次起播时会自动选择新的默认设备。
      return true;
    }
    _outputRouteChanging = true;
    final chapterList = List<Chapter>.of(chapters);
    final resume = currentTime;
    try {
      _playGeneration++;
      _positionSub?.cancel();
      _durationSub?.cancel();
      _playingSub?.cancel();
      _completeSub?.cancel();
      _indexSub?.cancel();
      _errorSub?.cancel();
      await _audio.dispose();
      if (_disposed) return true;
      // 新实例在打开音源时重新选择 Windows 默认端点。
      _createAudioPlayer();
      // 强制重开音源，避免复用旧队列继续使用失效的输出端点。
      _usingAudioQueue = false;
      _usingSegmentSource = false;
      await _playChapter(book, chapterList, chapter,
          startAt: resume, autoPlay: resumePlaying);
    } catch (error) {
      // 输出设备切换是尽力而为：失败时保留手动播放/暂停入口。
      debugPrint('rebind audio output failed: $error');
    } finally {
      _outputRouteChanging = false;
    }
    return true;
  }
}
