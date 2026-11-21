// Video on LG webOS TVs through the TV's own decoders: the plugin reads the source with a bundled
// FFmpeg and feeds the elementary streams to LG's media pipeline, which draws into an exported
// window under the Flutter surface. The page above the video must be transparent there.
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

@immutable
class NativePlayerValue {
  final bool initialized;
  final bool playing;
  final bool completed;
  final Duration duration;
  final Duration position;
  final Size size;
  final String videoCodec;
  final String audioCodec;
  final String? error;

  const NativePlayerValue({
    this.initialized = false,
    this.playing = false,
    this.completed = false,
    this.duration = Duration.zero,
    this.position = Duration.zero,
    this.size = Size.zero,
    this.videoCodec = '',
    this.audioCodec = '',
    this.error,
  });

  NativePlayerValue copyWith({
    bool? initialized,
    bool? playing,
    bool? completed,
    Duration? duration,
    Duration? position,
    Size? size,
    String? videoCodec,
    String? audioCodec,
    String? error,
  }) =>
      NativePlayerValue(
        initialized: initialized ?? this.initialized,
        playing: playing ?? this.playing,
        completed: completed ?? this.completed,
        duration: duration ?? this.duration,
        position: position ?? this.position,
        size: size ?? this.size,
        videoCodec: videoCodec ?? this.videoCodec,
        audioCodec: audioCodec ?? this.audioCodec,
        error: error ?? this.error,
      );
}

class NativePlayerController extends ValueNotifier<NativePlayerValue> {
  NativePlayerController(this.url, {this.startAt = Duration.zero, this.audioStream})
      : super(const NativePlayerValue());

  final String url;
  final Duration startAt;

  /// FFmpeg stream index of the audio track; null picks the file's default.
  final int? audioStream;

  static const _channel = MethodChannel('webos_native_player');
  static const _windows = MethodChannel('webos/exported_windows');

  int? _id;
  String? _windowId;
  StreamSubscription? _events;
  Timer? _positionTimer;
  bool _disposed = false;

  /// Starts loading; completes when the first picture's pipeline is ready, or throws.
  Future<void> initialize() async {
    final ready = Completer<void>();
    _windowId = await _windows.invokeMethod<String>('create', {});
    await _windows.invokeMethod<void>('resize', {
      'windowId': _windowId,
      'srcRect': [0, 0, 1920, 1080],
      'dstRect': [0, 0, 1920, 1080],
    });
    _id = await _channel.invokeMethod<int>('open', {
      'url': url,
      'windowId': _windowId,
      'startMs': startAt.inMilliseconds,
      if (audioStream != null) 'audioStream': audioStream,
    });
    _events = EventChannel('webos_native_player/events/$_id').receiveBroadcastStream().listen((raw) {
      final e = Map<String, String>.from(raw as Map);
      switch (e['event']) {
        case 'initialized':
          value = value.copyWith(
            initialized: true,
            playing: true,
            duration: Duration(milliseconds: int.tryParse(e['duration'] ?? '') ?? 0),
            position: startAt,
            size: Size(double.tryParse(e['width'] ?? '') ?? 0, double.tryParse(e['height'] ?? '') ?? 0),
            videoCodec: e['videoCodec'],
            audioCodec: e['audioCodec'],
          );
          if (!ready.isCompleted) ready.complete();
        case 'completed':
          value = value.copyWith(completed: true, playing: false);
        case 'seeked':
          value = value.copyWith(position: Duration(milliseconds: int.tryParse(e['position'] ?? '') ?? 0));
        case 'error':
          value = value.copyWith(error: e['message'] ?? 'Playback error');
          if (!ready.isCompleted) ready.completeError(StateError(value.error!));
        default:
          debugPrint('webos_native_player: $e');
      }
    });
    _positionTimer = Timer.periodic(const Duration(milliseconds: 250), (_) => _pollPosition());
    return ready.future;
  }

  Future<void> _pollPosition() async {
    final id = _id;
    if (id == null || _disposed || !value.initialized) return;
    final ms = await _channel.invokeMethod<int>('position', {'id': id});
    if (ms != null && ms > 0 && !_disposed) value = value.copyWith(position: Duration(milliseconds: ms));
  }

  Future<void> play() async {
    if (_id == null) return;
    await _channel.invokeMethod('play', {'id': _id});
    value = value.copyWith(playing: true, completed: false);
  }

  Future<void> pause() async {
    if (_id == null) return;
    await _channel.invokeMethod('pause', {'id': _id});
    value = value.copyWith(playing: false);
  }

  Future<void> seekTo(Duration to) async {
    if (_id == null) return;
    await _channel.invokeMethod('seekTo', {'id': _id, 'ms': to.inMilliseconds});
    value = value.copyWith(position: to, completed: false);
  }

  @override
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    _positionTimer?.cancel();
    await _events?.cancel();
    final id = _id, win = _windowId;
    super.dispose();
    if (id != null) await _channel.invokeMethod('dispose', {'id': id});
    // The pipeline unloads in the background; give it the window a little longer.
    if (win != null) {
      Future<void>.delayed(const Duration(seconds: 3), () => _windows.invokeMethod<void>('dispose', {'windowId': win}));
    }
  }
}

/// Where the video shows: a transparent hole in the Flutter surface.
class NativeVideoView extends StatelessWidget {
  const NativeVideoView({super.key});
  @override
  Widget build(BuildContext context) => const SizedBox.expand();
}
