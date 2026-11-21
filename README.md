# webos_native_player

Plays video on LG webOS TVs by feeding the TV's own decoders. A bundled FFmpeg reads the source
(HTTP, MKV/MP4/TS/HLS) and the plugin hands the elementary streams to LG's in-process media
pipeline (`StarfishMediaAPIs`, `BUFFERSTREAM` mode from `libplayerAPIs.so.1`), which decodes them
and draws into an exported window under the Flutter surface.

The design follows what [PlxNative](https://github.com/plxnative/plx-native) measured on real TVs
(its docs and source were read for reference; the code here is our own).

## Status

Tested on an LG C1 (webOS 6.5.3):

- Video: H.264 and HEVC, 4K HDR10 and Dolby Vision profiles 5 / 8 (`DolbyHdrInfo` in the Load
  payload; profile 7 plays its HDR10 base). Checked visually against the TV's own player.
- Audio: AC-3 and E-AC-3 as they are, Atmos flagged (`ac3PlusInfo`, `immersive`). Other audio
  plays without sound for now.
- Play, pause, seek (LG's public `Seek`, anchored on the landing keyframe, an IDR with Dolby Vision),
  starting part-way in (timeline shifted to 0: the pipeline refuses `Seek` before it plays),
  position from the frame-ready event, end of stream.

What mattered, found on the TV:

- **Own MP4/MKV to Annex B conversion** (`hevc_sei.cc`): parameter sets (VPS/SPS/PPS only) in
  front of keyframes, every unit with a 4-byte start code, as PlxNative does. ffmpeg's
  `hevc_mp4toannexb` placed the access unit's SEI ahead of the parameter sets, and the C1 then
  decoded 4K HEVC with a colour seam across the middle of the frame.
- HDR10+ SEI removed (Kodi, PlxNative and webos-native-app-kit all do; webOS mishandles it).
- Video PTS fed 1 ns early (Dolby Vision metadata lands on the right frame), about 1 s primed
  before `Play()`, `Feed` "Error" replies retried rather than dropped.
- The exported window placed with `setExportedWindow` and the video's real size.

Test knobs: key=value lines in the app's `/tmp/webos_native_player.conf` change payload and feed
details at the next open (see `Knobs` in `player.cc`); every pipeline callback is logged to the
app's `compat.log` (`[native-player]` lines).

Planned: AAC, in-file audio and subtitle tracks, PGS, DTS / TrueHD decoded and re-encoded to
AC-3 / E-AC-3, DTS fed as is where the TV decodes it (`dts_restore`), profile 7 to 8.1.

## Layout

- `lib/webos_native_player.dart` — `NativePlayerController` (shaped like `video_player`'s).
- `webos/player.cc` — demux thread, Load payload, feed loop, seek.
- `webos/plugin.cc` — method channel `webos_native_player`, events `webos_native_player/events/<id>`.
- `webos/hevc_sei.cc` — Annex B conversion, HDR10+ removal, HDR SEI reading.
- `webos/starfish.h` — LG's class (no public header exists); `starfish_stub.cc` only for linking.
- `webos/ffmpeg/` — FFmpeg 7.1.1, LGPL-2.1, shared, built by `tools/build-ffmpeg.sh` from the
  official tarball (sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`).
