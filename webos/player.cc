#include "player.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

#include "starfish.h"

#define LOG(...)                                \
  do {                                          \
    fprintf(stderr, "[native-player] ");        \
    fprintf(stderr, __VA_ARGS__);               \
    fputc('\n', stderr);                        \
    fflush(stderr);                             \
  } while (0)

namespace {

// How far ahead of the picture the feed may run. The pipeline's own buffers say BufferFull
// long before this on most streams; this keeps memory bounded on low-bitrate ones.
constexpr int64_t kMaxFeedAheadNs = 6LL * 1000 * 1000 * 1000;

int Interrupted(void* opaque) {
  return static_cast<std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

const char* VideoCodecName(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264: return "H264";
    case AV_CODEC_ID_HEVC: return "H265";
    default: return nullptr;
  }
}

// Audio the TV decodes from a fed elementary stream. Others are not fed yet (later: decoded and
// re-encoded to AC-3 / E-AC-3 in the plugin).
const char* AudioCodecName(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_AC3: return "AC3";
    case AV_CODEC_ID_EAC3: return "AC3 PLUS";
    default: return nullptr;
  }
}

}  // namespace

struct Player::Media {
  AVFormatContext* format = nullptr;
  AVBSFContext* annexb = nullptr;  // MP4/MKV length-prefixed NAL units -> Annex B start codes
  int video = -1;
  int audio = -1;
  const char* video_codec = nullptr;
  const char* audio_codec = nullptr;
  int width = 0;
  int height = 0;
  int64_t start_us = 0;  // the container's first timestamp; fed PTS start at 0
  int64_t duration_ms = 0;
  bool anchor_pending = false;  // after a seek: drop until the landing keyframe, then Seek() there

  ~Media() {
    if (annexb) av_bsf_free(&annexb);
    if (format) avformat_close_input(&format);
  }
};

Player::Player(PlayerOptions options, EventSink sink)
    : options_(std::move(options)), sink_(std::move(sink)), media_(new Media) {
  thread_ = std::thread(&Player::Run, this);
}

Player::~Player() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void Player::Emit(EventMap event) {
  if (sink_) sink_(std::move(event));
}

void Player::Play() {
  paused_ = false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (pipeline_ && loaded_) pipeline_->Play();
}

void Player::Pause() {
  paused_ = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (pipeline_ && loaded_) pipeline_->Pause();
}

void Player::SeekTo(int64_t ms) { seek_request_ms_ = std::max<int64_t>(0, ms); }

int64_t Player::PositionMs() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pipeline_ || !loaded_) return 0;
  // Nanoseconds of the fed timeline.
  return pipeline_->getCurrentPlaytime() / 1000000;
}

void Player::Run() {
  if (Open() && LoadPipeline()) FeedLoop();

  StarfishMediaAPIs* pipeline;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pipeline = pipeline_;
    loaded_ = false;
  }
  if (!pipeline) return;
  pipeline->Unload();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    unload_done_.wait_for(lock, std::chrono::seconds(3), [this] { return unloaded_.load(); });
    pipeline_ = nullptr;
  }
  if (unloaded_) {
    delete pipeline;
  } else {
    // The pipeline may still call back into it: keep it alive rather than risk a crash.
    LOG("unload not confirmed; pipeline object left allocated");
  }
  LOG("closed");
}

bool Player::Open() {
  Media& m = *media_;
  m.format = avformat_alloc_context();
  m.format->interrupt_callback.callback = Interrupted;
  m.format->interrupt_callback.opaque = &stop_;
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "rw_timeout", "15000000", 0);  // 15 s without data is an error
  int r = avformat_open_input(&m.format, options_.url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    char err[128];
    av_strerror(r, err, sizeof err);
    Emit({{"event", "error"}, {"message", std::string("Could not open the stream: ") + err}});
    return false;
  }
  if (avformat_find_stream_info(m.format, nullptr) < 0) {
    Emit({{"event", "error"}, {"message", "Could not read the stream's tracks"}});
    return false;
  }

  m.video = av_find_best_stream(m.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (m.video < 0) {
    Emit({{"event", "error"}, {"message", "The file has no video track"}});
    return false;
  }
  AVStream* vs = m.format->streams[m.video];
  m.video_codec = VideoCodecName(vs->codecpar->codec_id);
  if (!m.video_codec) {
    Emit({{"event", "error"},
          {"message", std::string("Video codec not supported yet: ") + avcodec_get_name(vs->codecpar->codec_id)}});
    return false;
  }
  m.width = vs->codecpar->width;
  m.height = vs->codecpar->height;

  int a = options_.audio_stream;
  if (a < 0 || a >= static_cast<int>(m.format->nb_streams) ||
      m.format->streams[a]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
    a = av_find_best_stream(m.format, AVMEDIA_TYPE_AUDIO, -1, m.video, nullptr, 0);
  }
  if (a >= 0) {
    m.audio_codec = AudioCodecName(m.format->streams[a]->codecpar->codec_id);
    if (m.audio_codec) {
      m.audio = a;
    } else {
      LOG("audio %s not fed yet: playing without sound",
          avcodec_get_name(m.format->streams[a]->codecpar->codec_id));
    }
  }
  for (unsigned i = 0; i < m.format->nb_streams; i++) {
    if (static_cast<int>(i) != m.video && static_cast<int>(i) != m.audio) {
      m.format->streams[i]->discard = AVDISCARD_ALL;
    }
  }

  const char* bsf_name = vs->codecpar->codec_id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb" : "h264_mp4toannexb";
  const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
  if (!bsf || av_bsf_alloc(bsf, &m.annexb) < 0 ||
      avcodec_parameters_copy(m.annexb->par_in, vs->codecpar) < 0) {
    Emit({{"event", "error"}, {"message", "Could not prepare the video stream"}});
    return false;
  }
  m.annexb->time_base_in = vs->time_base;
  if (av_bsf_init(m.annexb) < 0) {
    Emit({{"event", "error"}, {"message", "Could not prepare the video stream"}});
    return false;
  }

  m.start_us = m.format->start_time != AV_NOPTS_VALUE ? m.format->start_time : 0;
  m.duration_ms = m.format->duration != AV_NOPTS_VALUE ? m.format->duration / 1000 : 0;
  LOG("opened %s: video #%d %s %dx%d, audio #%d %s, %" PRId64 " ms, start %" PRId64 " us",
      m.format->iformat->name, m.video, m.video_codec, m.width, m.height, m.audio,
      m.audio_codec ? m.audio_codec : "-", m.duration_ms, m.start_us);

  if (options_.start_ms > 0) {
    av_seek_frame(m.format, -1, m.start_us + options_.start_ms * 1000, AVSEEK_FLAG_BACKWARD);
    m.anchor_pending = true;
  }
  return true;
}

bool Player::LoadPipeline() {
  Media& m = *media_;
  const char* app_id = getenv("FLUTTER_APP_ID");
  bool uhd = m.width > 1920 || m.height > 1080 || std::strcmp(m.video_codec, "H265") == 0;
  char payload[2048];
  snprintf(payload, sizeof payload,
           "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{"
           "\"appId\":\"%s\",\"windowId\":\"%s\","
           "\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"%s%s%s},"
           "\"esInfo\":{\"pauseAtDecodeTime\":false,\"ptsToDecode\":0,\"seperatedPTS\":true},"
           "\"format\":\"RAW\",\"provider\":\"webos_native_player\"},"
           "\"streamQualityInfo\":true,\"audioSync\":true,\"restartStreaming\":false,"
           "\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,\"preBufferByte\":0,"
           "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
           "\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":1048576},"
           "\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}},"
           "\"needAudio\":%s,\"queryPosition\":false,\"lowDelayMode\":false,"
           "\"transmission\":{\"contentsType\":\"LIVE\"},"
           "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":60}}}]}",
           JsonEscape(app_id ? app_id : "").c_str(), JsonEscape(options_.window_id).c_str(),
           m.video_codec, m.audio_codec ? ",\"audio\":\"" : "", m.audio_codec ? m.audio_codec : "",
           m.audio_codec ? "\"" : "", m.audio_codec ? "true" : "false", uhd ? 3840 : 1920, uhd ? 2160 : 1080);
  LOG("load %s", payload);

  auto* pipeline = new StarfishMediaAPIs(nullptr);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pipeline_ = pipeline;
  }
  pipeline->notifyForeground();
  if (!pipeline->Load(payload, &Player::PipelineCallback, this)) {
    Emit({{"event", "error"}, {"message", "The TV's media pipeline refused to load"}});
    return false;
  }
  loaded_ = true;
  if (!paused_) pipeline->Play();
  Emit({{"event", "initialized"},
        {"duration", std::to_string(m.duration_ms)},
        {"width", std::to_string(m.width)},
        {"height", std::to_string(m.height)},
        {"videoCodec", m.video_codec},
        {"audioCodec", m.audio_codec ? m.audio_codec : ""}});
  return true;
}

bool Player::FeedPacket(const uint8_t* data, int size, int64_t pts_ns, int es) {
  char json[160];
  snprintf(json, sizeof json, "{\"bufferAddr\":\"%p\",\"bufferSize\":%d,\"pts\":%" PRId64 ",\"esData\":%d}",
           static_cast<const void*>(data), size, std::max<int64_t>(0, pts_ns), es);
  static int logged_errors = 0;
  while (!stop_ && seek_request_ms_ < 0) {
    std::string reply = pipeline_->Feed(json);
    if (reply.find("Ok") != std::string::npos) return true;
    if (reply.find("BufferFull") != std::string::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (logged_errors++ < 20) LOG("feed es=%d refused: %s", es, reply.c_str());
    return false;
  }
  return false;
}

void Player::FeedLoop() {
  Media& m = *media_;
  AVPacket* packet = av_packet_alloc();
  AVPacket* out = av_packet_alloc();
  bool eos = false;
  bool logged_position = false;

  while (!stop_) {
    int64_t seek_ms = seek_request_ms_.exchange(-1);
    if (seek_ms >= 0) {
      av_seek_frame(m.format, -1, m.start_us + seek_ms * 1000, AVSEEK_FLAG_BACKWARD);
      av_bsf_flush(m.annexb);
      m.anchor_pending = true;
      eos = false;
    }

    if (eos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    // Keep the feed a few seconds ahead of the picture.
    int64_t played_ns;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      played_ns = pipeline_->getCurrentPlaytime();
    }
    if (!logged_position && played_ns > 0) {
      LOG("first playtime value %lld (fed video up to %" PRId64 " ns)", played_ns, last_fed_video_ns_.load());
      logged_position = true;
    }
    if (!m.anchor_pending && played_ns > 0 && last_fed_video_ns_ - played_ns > kMaxFeedAheadNs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    int r = av_read_frame(m.format, packet);
    if (r == AVERROR_EOF) {
      pipeline_->pushEOS();
      eos = true;
      LOG("end of stream fed");
      continue;
    }
    if (r < 0) {
      if (stop_) break;
      char err[128];
      av_strerror(r, err, sizeof err);
      Emit({{"event", "error"}, {"message", std::string("Reading the stream failed: ") + err}});
      break;
    }

    AVStream* st = m.format->streams[packet->stream_index];
    int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    int64_t pts_ns = pts == AV_NOPTS_VALUE ? 0
        : av_rescale_q(pts, st->time_base, AVRational{1, 1000000000}) - m.start_us * 1000;

    if (m.anchor_pending) {
      // Land on a video keyframe: anchor the pipeline's timeline there, drop what came before.
      if (packet->stream_index != m.video || !(packet->flags & AV_PKT_FLAG_KEY)) {
        av_packet_unref(packet);
        continue;
      }
      int64_t anchor_ms = std::max<int64_t>(0, pts_ns / 1000000);
      bool ok;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ok = pipeline_->Seek(std::to_string(anchor_ms).c_str());
      }
      LOG("seek anchored at %" PRId64 " ms (%s)", anchor_ms, ok ? "ok" : "refused");
      m.anchor_pending = false;
      Emit({{"event", "seeked"}, {"position", std::to_string(anchor_ms)}});
    }

    if (packet->stream_index == m.video) {
      if (av_bsf_send_packet(m.annexb, packet) == 0) {
        while (av_bsf_receive_packet(m.annexb, out) == 0) {
          int64_t opts = out->pts != AV_NOPTS_VALUE ? out->pts : out->dts;
          int64_t ns = opts == AV_NOPTS_VALUE ? pts_ns
              : av_rescale_q(opts, st->time_base, AVRational{1, 1000000000}) - m.start_us * 1000;
          if (FeedPacket(out->data, out->size, ns, 1)) last_fed_video_ns_ = ns;
          av_packet_unref(out);
        }
      }
    } else if (packet->stream_index == m.audio) {
      FeedPacket(packet->data, packet->size, pts_ns, 2);
    }
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  av_packet_free(&out);
}

void Player::PipelineCallback(int type, long long num, const char* str, void* context) {
  static_cast<Player*>(context)->OnPipelineEvent(type, num, str);
}

void Player::OnPipelineEvent(int type, long long num, const char* str) {
  std::string s = str ? str : "";
  // Frame-ready (0) and the periodic counters arrive many times a second: not logged.
  if (type != 0 && !(type >= 46 && type <= 49)) LOG("event type=%d num=%lld str=%s", type, num, s.c_str());
  if (type == 23) {
    std::lock_guard<std::mutex> lock(mutex_);
    unloaded_ = true;
    unload_done_.notify_all();
    return;
  }
  if (s.find("endOfStream") != std::string::npos) {
    Emit({{"event", "completed"}});
  } else if (type == 18) {
    Emit({{"event", "error"}, {"message", "Playback error " + std::to_string(num) + (s.empty() ? "" : ": " + s)}});
  } else if (type != 0 && type < 46) {
    Emit({{"event", "native"}, {"type", std::to_string(type)}, {"num", std::to_string(num)}, {"str", s}});
  }
}
