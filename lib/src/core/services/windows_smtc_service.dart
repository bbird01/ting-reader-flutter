import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:smtc_windows/smtc_windows.dart';

import '../models/models.dart';
import '../state/player_state.dart';
import '../utils/urls.dart';

/// 把播放状态发布到 Windows 系统媒体会话（SMTC），使系统媒体浮窗、耳机按键
/// 以及第三方媒体控制器（如 fly-island 灵动岛）能识别并控制 Ting Reader。
class WindowsSmtcService {
  WindowsSmtcService(this._player);

  final PlayerState _player;

  SMTCWindows? _smtc;
  StreamSubscription<PressedButton>? _buttonSub;
  bool _attached = false;
  bool _starting = false;
  bool _disposed = false;

  bool _sessionEnabled = false;
  String? _metadataKey;
  bool? _playing;
  int _positionMs = -1;
  int _endMs = -1;

  // SMTC 调用必须串行：FFI 侧在线程池执行，并发下发会让启用会话与元数据更新乱序。
  Future<void> _queue = Future<void>.value();

  static bool get isSupported =>
      !kIsWeb && defaultTargetPlatform == TargetPlatform.windows;

  Future<void> start() async {
    if (!isSupported || _disposed || _starting || _smtc != null) return;
    _starting = true;
    try {
      await SMTCWindows.initialize();
      if (_disposed) return;
      final smtc = SMTCWindows(
        config: const SMTCConfig(
          playEnabled: true,
          pauseEnabled: true,
          stopEnabled: false,
          nextEnabled: true,
          prevEnabled: true,
          fastForwardEnabled: false,
          rewindEnabled: false,
        ),
        enabled: false,
      );
      _smtc = smtc;
      _buttonSub = smtc.buttonPressStream.listen(_handleButton);
      _player.addListener(_sync);
      _attached = true;
      _sync();
    } catch (error) {
      debugPrint('WindowsSmtcService 初始化失败: $error');
      await _teardown();
    } finally {
      _starting = false;
    }
  }

  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    await _teardown();
  }

  Future<void> _teardown() async {
    if (_attached) {
      _player.removeListener(_sync);
      _attached = false;
    }
    await _buttonSub?.cancel();
    _buttonSub = null;
    final smtc = _smtc;
    _smtc = null;
    _sessionEnabled = false;
    if (smtc == null) return;
    try {
      await smtc.dispose();
    } catch (error) {
      debugPrint('WindowsSmtcService 释放失败: $error');
    }
  }

  void _handleButton(PressedButton button) {
    switch (button) {
      case PressedButton.play:
        if (!_player.isPlaying) unawaited(_player.togglePlay());
        break;
      case PressedButton.pause:
        if (_player.isPlaying) unawaited(_player.pause());
        break;
      case PressedButton.next:
        unawaited(_player.nextChapter());
        break;
      case PressedButton.previous:
        unawaited(_player.previousChapter());
        break;
      default:
        break;
    }
  }

  void _sync() {
    final smtc = _smtc;
    if (smtc == null || _disposed) return;

    final book = _player.currentBook;
    final chapter = _player.currentChapter;
    if (book == null || chapter == null) {
      if (_sessionEnabled) {
        _sessionEnabled = false;
        _metadataKey = null;
        _playing = null;
        _positionMs = -1;
        _endMs = -1;
        _enqueue(() => smtc.disableSmtc());
      }
      return;
    }

    if (!_sessionEnabled) {
      _sessionEnabled = true;
      _enqueue(() => smtc.enableSmtc());
    }

    final metadataKey = '${book.id}|${chapter.id}|${book.title}|${chapter.title}'
        '|${book.narrator}|${book.author}|${book.coverUrl}';
    if (metadataKey != _metadataKey) {
      _metadataKey = metadataKey;
      // 切章后进度基线失效，否则新旧位置差不足一秒时会跳过时间轴下发。
      _positionMs = -1;
      _endMs = -1;
      _enqueue(() => smtc.updateMetadata(MusicMetadata(
            title: chapter.title,
            album: book.title,
            artist: _artistFor(book),
            albumArtist: book.author,
            thumbnail: _thumbnailFor(book),
          )));
    }

    final playing = _player.isPlaying;
    if (playing != _playing) {
      _playing = playing;
      _enqueue(() => smtc.setPlaybackStatus(
            playing ? PlaybackStatus.playing : PlaybackStatus.paused,
          ));
    }

    final endMs = (_player.duration * 1000).round();
    final positionMs = (_player.currentTime * 1000).round();
    // positionStream 每帧都 notifyListeners，只在跨秒或总长变化时下发时间轴。
    if (endMs > 0 &&
        (endMs != _endMs || (positionMs - _positionMs).abs() >= 1000)) {
      _endMs = endMs;
      _positionMs = positionMs;
      _enqueue(() => smtc.updateTimeline(PlaybackTimeline(
            startTimeMs: 0,
            endTimeMs: endMs,
            positionMs: positionMs,
            minSeekTimeMs: 0,
            maxSeekTimeMs: endMs,
          )));
    }
  }

  String? _artistFor(Book book) {
    final parts = [
      if ((book.narrator ?? '').trim().isNotEmpty) book.narrator!.trim(),
      if ((book.author ?? '').trim().isNotEmpty) book.author!.trim(),
    ];
    return parts.isEmpty ? null : parts.join(' / ');
  }

  String? _thumbnailFor(Book book) {
    final raw = bookCoverUrl(_player.appState, book).trim();
    if (raw.isEmpty) return null;
    final parsed = Uri.tryParse(raw);
    final Uri? uri;
    if (parsed != null && parsed.hasScheme) {
      uri = parsed;
    } else if (RegExp(r'^[A-Za-z]:[\\/]').hasMatch(raw) ||
        raw.startsWith('/')) {
      uri = Uri.file(raw);
    } else {
      uri = null;
    }
    // Rust 端对 CreateUri/CreateFromUri 直接 unwrap，非绝对 URI 会 panic。
    if (uri == null || !uri.isAbsolute) return null;
    const schemes = {'http', 'https', 'file'};
    if (!schemes.contains(uri.scheme)) return null;
    return uri.toString();
  }

  void _enqueue(Future<void> Function() action) {
    _queue = _queue.then((_) async {
      try {
        await action();
      } catch (error) {
        debugPrint('WindowsSmtcService 调用失败: $error');
      }
    });
  }
}