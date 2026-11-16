part of '../player_state.dart';

/// Cold-start continuation of the last listening session.
extension _PlayerAutoResume on PlayerState {
  Future<void> _autoResumeLastSession() async {
    if (_autoResumeAttempted) return;
    _autoResumeAttempted = true;
    if (_disposed ||
        !appState.isAuthenticated ||
        appState.offlineMode ||
        isPlaying ||
        _audio.playing ||
        currentBook != null) {
      return;
    }
    try {
      final response = await appState.api.get('/api/progress/recent');
      final recent = asMapList(response.data)
          .map(ProgressItem.fromJson)
          .where((item) =>
              item.bookId.isNotEmpty && (item.chapterId?.isNotEmpty ?? false))
          .toList()
        ..sort((a, b) => _autoResumeTime(b).compareTo(_autoResumeTime(a)));
      if (recent.isEmpty || _disposed || currentBook != null) return;
      final progress = recent.first;
      final detail = BookDetailState(
        appState.api,
        progress.bookId,
        cache: appState.libraryCache,
      );
      try {
        await detail.load();
        final book = detail.book;
        if (book == null || _disposed || currentBook != null) return;
        final chapter = await detail.resolveResume(progress.chapterId, null);
        if (chapter == null || _disposed || currentBook != null) return;
        final chapters = book.libraryType == 'source'
            ? detail.chapters
            : (await detail.catalogue.all() ?? const <Chapter>[]);
        if (chapters.isEmpty || _disposed || currentBook != null) return;
        await playChapter(
          book,
          chapters,
          chapter.copyWith(progressPosition: progress.position),
        );
      } finally {
        detail.dispose();
      }
    } catch (error, stack) {
      // 自动续播是尽力而为：失败时保留手动「继续播放」入口。
      debugPrint('autoResume failed: $error\n$stack');
    }
  }

  DateTime _autoResumeTime(ProgressItem item) =>
      DateTime.tryParse(item.updatedAt ?? '') ?? DateTime(1970);
}
