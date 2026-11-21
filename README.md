# webos_native_player

Plays video on LG webOS TVs by feeding the TV's own decoders. A bundled FFmpeg reads the source
(HTTP, MKV/MP4/TS/HLS) and the plugin hands the elementary streams to LG's in-process media
pipeline (`StarfishMediaAPIs`, `BUFFERSTREAM` mode from `libplayerAPIs.so.1`), which decodes them
and draws into an exported window under the Flutter surface.

The design follows what [PlxNative](https://github.com/plxnative/plx-native) measured on real TVs
(its docs and source were read for reference; the code here is our own).

## Status

First version, being tested on an LG C1 (webOS 6.5.3):

- Video: H.264 and HEVC (MP4/MKV length-prefixed NAL units converted to Annex B).
- Audio: AC-3 and E-AC-3 fed as they are. Other audio plays without sound for now.
- Play, pause, seek (LG's public `Seek`, anchored on the landing keyframe), position, end of stream.
- Every pipeline callback is logged to the app's `compat.log` (`[native-player]` lines).

Planned: Dolby Vision / HDR declaration, Atmos, AAC, in-file audio and subtitle tracks, PGS,
DTS / TrueHD decoded and re-encoded to AC-3 / E-AC-3, DTS fed as is where the TV decodes it
(`dts_restore`).

## Layout

- `lib/webos_native_player.dart` — `NativePlayerController` (shaped like `video_player`'s).
- `webos/player.cc` — demux thread, Load payload, feed loop, seek.
- `webos/plugin.cc` — method channel `webos_native_player`, events `webos_native_player/events/<id>`.
- `webos/starfish.h` — LG's class (no public header exists); `starfish_stub.cc` only for linking.
- `webos/ffmpeg/` — FFmpeg 7.1.1, LGPL-2.1, shared, built by `tools/build-ffmpeg.sh` from the
  official tarball (sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`).
