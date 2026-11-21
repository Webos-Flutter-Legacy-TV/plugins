// Method channel "webos_native_player": open / play / pause / seekTo / position / dispose.
// Each player gets an event channel "webos_native_player/events/<id>".
#include "include/webos_native_player/webos_native_player_plugin.h"

#include <flutter/encodable_value.h>
#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>
#include <glib.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "player.h"

namespace {

using flutter::EncodableMap;
using flutter::EncodableValue;

const EncodableValue* Arg(const EncodableMap& args, const char* key) {
  auto it = args.find(EncodableValue(key));
  return it == args.end() ? nullptr : &it->second;
}

int64_t IntArg(const EncodableMap& args, const char* key, int64_t fallback) {
  const EncodableValue* v = Arg(args, key);
  if (!v) return fallback;
  if (auto i = std::get_if<int32_t>(v)) return *i;
  if (auto l = std::get_if<int64_t>(v)) return *l;
  return fallback;
}

std::string StringArg(const EncodableMap& args, const char* key) {
  const EncodableValue* v = Arg(args, key);
  if (v) {
    if (auto s = std::get_if<std::string>(v)) return *s;
  }
  return "";
}

class NativePlayerPlugin : public flutter::Plugin {
 public:
  explicit NativePlayerPlugin(flutter::PluginRegistrar* registrar)
      : registrar_(registrar), main_context_(g_main_context_ref_thread_default()) {
    channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(
        registrar->messenger(), "webos_native_player", &flutter::StandardMethodCodec::GetInstance());
    channel_->SetMethodCallHandler([this](const auto& call, auto result) { Handle(call, std::move(result)); });
  }

  ~NativePlayerPlugin() override {
    for (auto& entry : players_) Release(std::move(entry.second.player));
    g_main_context_unref(main_context_);
  }

 private:
  struct Entry {
    std::unique_ptr<Player> player;
    std::unique_ptr<flutter::EventChannel<EncodableValue>> channel;
    std::unique_ptr<flutter::EventSink<EncodableValue>> sink;
    std::deque<EventMap> pending;  // events before Dart listens
  };

  struct Posted {
    NativePlayerPlugin* plugin;
    int64_t id;
    EventMap event;
  };

  // Unloading the pipeline can take a moment: never on the platform thread.
  static void Release(std::unique_ptr<Player> player) {
    if (player) std::thread([p = std::move(player)]() mutable { p.reset(); }).detach();
  }

  void Handle(const flutter::MethodCall<EncodableValue>& call,
              std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
    const auto* args = std::get_if<EncodableMap>(call.arguments());
    static const EncodableMap kNoArgs;
    const EncodableMap& a = args ? *args : kNoArgs;
    const std::string& method = call.method_name();

    if (method == "open") {
      PlayerOptions options;
      options.url = StringArg(a, "url");
      options.window_id = StringArg(a, "windowId");
      options.start_ms = IntArg(a, "startMs", 0);
      options.audio_stream = static_cast<int>(IntArg(a, "audioStream", -1));
      int64_t id = next_id_++;
      Entry& entry = players_[id];
      entry.channel = std::make_unique<flutter::EventChannel<EncodableValue>>(
          registrar_->messenger(), "webos_native_player/events/" + std::to_string(id),
          &flutter::StandardMethodCodec::GetInstance());
      entry.channel->SetStreamHandler(std::make_unique<flutter::StreamHandlerFunctions<EncodableValue>>(
          [this, id](const EncodableValue*, std::unique_ptr<flutter::EventSink<EncodableValue>>&& events)
              -> std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> {
            auto it = players_.find(id);
            if (it != players_.end()) {
              it->second.sink = std::move(events);
              while (!it->second.pending.empty()) {
                Send(it->second, it->second.pending.front());
                it->second.pending.pop_front();
              }
            }
            return nullptr;
          },
          [this, id](const EncodableValue*) -> std::unique_ptr<flutter::StreamHandlerError<EncodableValue>> {
            auto it = players_.find(id);
            if (it != players_.end()) it->second.sink.reset();
            return nullptr;
          }));
      entry.player = std::make_unique<Player>(options, [this, id](EventMap event) { Post(id, std::move(event)); });
      result->Success(EncodableValue(id));
      return;
    }

    int64_t id = IntArg(a, "id", -1);
    auto it = players_.find(id);
    if (it == players_.end()) {
      result->Error("no_player", "No player with id " + std::to_string(id));
      return;
    }
    Player& player = *it->second.player;
    if (method == "play") {
      player.Play();
      result->Success();
    } else if (method == "pause") {
      player.Pause();
      result->Success();
    } else if (method == "seekTo") {
      player.SeekTo(IntArg(a, "ms", 0));
      result->Success();
    } else if (method == "position") {
      result->Success(EncodableValue(player.PositionMs()));
    } else if (method == "dispose") {
      Release(std::move(it->second.player));
      players_.erase(it);
      result->Success();
    } else {
      result->NotImplemented();
    }
  }

  // From the player's threads: hand the event to the platform thread.
  void Post(int64_t id, EventMap event) {
    auto* posted = new Posted{this, id, std::move(event)};
    g_main_context_invoke_full(
        main_context_, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          auto* p = static_cast<Posted*>(data);
          p->plugin->Deliver(p->id, std::move(p->event));
          return G_SOURCE_REMOVE;
        },
        posted, [](gpointer data) { delete static_cast<Posted*>(data); });
  }

  void Deliver(int64_t id, EventMap event) {
    auto it = players_.find(id);
    if (it == players_.end()) return;  // disposed meanwhile
    if (it->second.sink) {
      Send(it->second, event);
    } else {
      it->second.pending.push_back(std::move(event));
    }
  }

  static void Send(Entry& entry, const EventMap& event) {
    EncodableMap map;
    for (const auto& kv : event) map[EncodableValue(kv.first)] = EncodableValue(kv.second);
    entry.sink->Success(EncodableValue(map));
  }

  flutter::PluginRegistrar* registrar_;
  GMainContext* main_context_;
  std::unique_ptr<flutter::MethodChannel<EncodableValue>> channel_;
  std::map<int64_t, Entry> players_;
  int64_t next_id_ = 1;
};

}  // namespace

void WebosNativePlayerPluginRegisterWithRegistrar(FlutterDesktopPluginRegistrarRef registrar) {
  auto* r = flutter::PluginRegistrarManager::GetInstance()->GetRegistrar<flutter::PluginRegistrar>(registrar);
  r->AddPlugin(std::make_unique<NativePlayerPlugin>(r));
}
