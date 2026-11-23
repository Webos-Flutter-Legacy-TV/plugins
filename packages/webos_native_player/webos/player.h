// SPDX-License-Identifier: BSD-3-Clause
// One playback: a thread that reads the source with FFmpeg and feeds its elementary streams to
// LG's media pipeline (StarfishMediaAPIs, BUFFERSTREAM), which decodes and draws them in the
// exported window the Dart side created.
#ifndef WEBOS_NATIVE_PLAYER_PLAYER_H_
#define WEBOS_NATIVE_PLAYER_PLAYER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class StarfishMediaAPIs;

struct PlayerOptions {
  std::string url;
  std::string window_id;
  int64_t start_ms = 0;
  int audio_stream = -1;  // FFmpeg stream index; -1 picks the default
};

// Events go to Dart as maps of strings and numbers; the callback runs on the player's threads and
// the plugin moves them to the platform thread.
using EventMap = std::map<std::string, std::string>;
using EventSink = std::function<void(EventMap)>;

class Player {
 public:
  Player(PlayerOptions options, EventSink sink);
  ~Player();  // stops feeding, unloads the pipeline

  void Play();
  void Pause();
  void SeekTo(int64_t ms);
  int64_t PositionMs();

 private:
  void Run();
  bool Open();
  bool LoadPipeline();
  void FeedLoop();
  bool FeedPacket(const uint8_t* data, int size, int64_t pts_ns, int es);
  void Emit(EventMap event);
  void OnPipelineEvent(int type, long long num, const char* str);
  static void PipelineCallback(int type, long long num, const char* str, void* context);

  PlayerOptions options_;
  EventSink sink_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> seek_request_ms_{-1};
  std::atomic<int64_t> last_fed_video_ns_{0};
  std::atomic<int64_t> position_ns_{0};
  int64_t feed_error_retries_ = 0;
  // Subtracted from fed PTS: a start part-way in feeds from 0 (the pipeline's segment starts at 0 and
  // refuses Seek before it plays); after a Seek the pipeline runs on content time and this is 0.
  std::atomic<int64_t> pts_shift_ns_{0};  // from the pipeline's frame-ready event (type 0, ~5 Hz)
  std::atomic<bool> loaded_{false};
  std::atomic<bool> unloaded_{false};
  std::mutex mutex_;  // guards pipeline_ between the feed thread and Play/Pause from Dart
  std::condition_variable unload_done_;

  StarfishMediaAPIs* pipeline_ = nullptr;
  struct Media;  // FFmpeg state, kept out of this header
  std::unique_ptr<Media> media_;
};

#endif  // WEBOS_NATIVE_PLAYER_PLAYER_H_
