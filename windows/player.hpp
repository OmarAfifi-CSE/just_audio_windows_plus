#pragma comment(lib, "windowsapp")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include "source_model.hpp"
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// This must be included before many other Windows headers. NOMINMAX keeps the
// min/max macros away from std::min/std::max calls below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include "native_utils.hpp"
#include "platform_thread.hpp"
#include "uri_utils.hpp"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.System.h>

// One line per method call is useful when debugging and pure noise in production.
// Trace only in debug builds (#ifndef NDEBUG). Real errors are logged either way.
#ifndef NDEBUG
#define JAW_TRACE(expr) do { std::cerr << expr << std::endl; } while (0)
#else
#define JAW_TRACE(expr) do { } while (0)
#endif

#define JAW_ERROR(expr) do { std::cerr << "[just_audio_windows_plus] " << expr << std::endl; } while (0)

using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Uri;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using winrt::Windows::Media::Core::MediaSource;

using flutter::EncodableMap;
using flutter::EncodableValue;

inline int64_t TimeSpanToMicroseconds(winrt::Windows::Foundation::TimeSpan timespan) {
  return timespan.count() / 10;
}

inline int64_t TimeSpanToMilliseconds(winrt::Windows::Foundation::TimeSpan timespan) {
  return timespan.count() / 10000;
}

// Looks for |key| in |map|, returning the associated value if it is present, or
// a nullptr if not.
const EncodableValue* ValueOrNull(const EncodableMap& map, const char* key) {
  auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &(it->second);
}

// Safely extracts int64_t from an EncodableValue whether Dart sent int32_t or int64_t
inline bool TryGetInt64(const EncodableValue* val, int64_t& out) {
  if (!val) return false;
  if (const auto* i32 = std::get_if<int32_t>(val)) {
    out = *i32;
    return true;
  }
  if (const auto* i64 = std::get_if<int64_t>(val)) {
    out = *i64;
    return true;
  }
  return false;
}

// Converts a std::string to std::wstring
inline std::wstring TO_WIDESTRING(const std::string& string) {
  if (string.empty()) {
    return std::wstring();
  }
  int32_t target_length =
    ::MultiByteToWideChar(CP_UTF8, 0, string.data(),
      static_cast<int32_t>(string.length()), nullptr, 0);
  if (target_length == 0) {
    return std::wstring();
  }
  std::wstring utf16_string;
  utf16_string.resize(target_length);
  int32_t converted_length =
    ::MultiByteToWideChar(CP_UTF8, 0, string.data(),
      static_cast<int32_t>(string.length()),
      utf16_string.data(), target_length);
  if (converted_length == 0) {
    return std::wstring();
  }
  return utf16_string;
}

class JustAudioEventSink {
public:
  JustAudioEventSink(JustAudioEventSink const&) = delete;
  JustAudioEventSink& operator=(JustAudioEventSink const&) = delete;

  JustAudioEventSink(flutter::BinaryMessenger* messenger, const std::string& id)
      : messenger_(messenger), id_(id) {
    auto event_channel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
        messenger_, id_, &flutter::StandardMethodCodec::GetInstance());

    auto event_handler = std::make_unique<flutter::StreamHandlerFunctions<>>(
      [this](const EncodableValue* arguments, std::unique_ptr<flutter::EventSink<>>&& events) -> std::unique_ptr<flutter::StreamHandlerError<>> {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = std::move(events);
        return nullptr;
      },
      [this](const EncodableValue* arguments) -> std::unique_ptr<flutter::StreamHandlerError<>> {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink.reset();
        return nullptr;
      });

    event_channel->SetStreamHandler(std::move(event_handler));
  }

  ~JustAudioEventSink() {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink.reset();
    if (messenger_) {
      messenger_->SetMessageHandler(id_, nullptr);
    }
  }

  void Success(const EncodableValue& event) {
    bool schedule_recovery = false;
    {
      std::lock_guard<std::mutex> lock(sink_mutex_);
      if (sink) {
        try {
          sink->Success(event);
        } catch (...) {
          try {
            sink->Success(event);
          } catch (...) {
            schedule_recovery = true;
          }
        }
      }
    }
    // Invoked after releasing sink_mutex_: the recovery hook re-enters the
    // sink through a full-state Broadcast(), and calling it under the lock
    // would deadlock.
    if (schedule_recovery && deferred_recovery_) deferred_recovery_();
  }

  void Error(const std::string& error_code, const std::string& error_message) {
    bool schedule_recovery = false;
    {
      std::lock_guard<std::mutex> lock(sink_mutex_);
      if (sink) {
        try {
          sink->Error(error_code, error_message);
        } catch (...) {
          try {
            sink->Error(error_code, error_message);
          } catch (...) {
            schedule_recovery = true;
          }
        }
      }
    }
    if (schedule_recovery && deferred_recovery_) deferred_recovery_();
  }

  // Installed by the owning AudioPlayer once shared ownership exists. When a
  // state event fails to deliver after the immediate retry, this hook
  // schedules a full-state Broadcast() on the player's dispatcher so any
  // lost event is re-delivered and Dart's belief converges with the engine.
  void SetDeferredRecovery(std::function<void()> hook) {
    deferred_recovery_ = std::move(hook);
  }

private:
  flutter::BinaryMessenger* messenger_ = nullptr;
  std::string id_;
  std::mutex sink_mutex_;
  std::unique_ptr<flutter::EventSink<>> sink = nullptr;
  std::function<void()> deferred_recovery_;
};

class AudioPlayer : public std::enable_shared_from_this<AudioPlayer> {
 public:
  using Result = flutter::MethodResult<EncodableValue>;

  AudioPlayer(std::string id, flutter::BinaryMessenger* messenger,
              std::shared_ptr<PlatformThreadDispatcher> dispatcher)
      : id_(std::move(id)), dispatcher_(std::move(dispatcher)) {
    player_channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(
        messenger, "com.ryanheise.just_audio.methods." + id_,
        &flutter::StandardMethodCodec::GetInstance());
    event_sink_ = std::make_unique<JustAudioEventSink>(messenger, "com.ryanheise.just_audio.events." + id_);
    data_sink_ = std::make_unique<JustAudioEventSink>(messenger, "com.ryanheise.just_audio.data." + id_);
  }

  // Called only after shared ownership exists. Every native callback posts
  // before locking the weak player, so all player access and destruction are
  // serialized on Flutter's platform thread.
  void Initialize() {
    std::weak_ptr<AudioPlayer> weak = shared_from_this();
    player_channel_->SetMethodCallHandler([weak](const auto& call, auto result) {
      if (auto player = weak.lock()) player->HandleMethodCall(call, std::move(result));
      else result->Error("disposed", "Player has been disposed");
    });
    // Self-healing event delivery: when a state event fails to deliver even
    // after the sink's immediate retry, one deduplicated full-state
    // Broadcast() is scheduled on this player's dispatcher, re-delivering
    // every field so Dart's belief converges with the engine.
    auto recovery = [weak] {
      if (auto owner = weak.lock()) owner->PostRecoveryBroadcast();
    };
    event_sink_->SetDeferredRecovery(recovery);
    data_sink_->SetDeferredRecovery(recovery);
    ResetNative();
  }

  ~AudioPlayer() { Dispose(); }
  bool HasPlayerId(const std::string& id) const { return id_ == id; }

  void Dispose() {
    if (disposed_) return;
    disposed_ = true;
    ++generation_;
    CancelLoad("Player disposed");
    CompletePlay();
    ReleaseNative();
    if (player_channel_) player_channel_->SetMethodCallHandler(nullptr);
    event_sink_.reset();
    data_sink_.reset();
  }

 private:
  std::string id_;
  std::shared_ptr<PlatformThreadDispatcher> dispatcher_;
  std::unique_ptr<flutter::MethodChannel<EncodableValue>> player_channel_;
  std::unique_ptr<JustAudioEventSink> event_sink_;
  std::unique_ptr<JustAudioEventSink> data_sink_;
  std::atomic<bool> recovery_broadcast_pending_{false};
  Playback::MediaPlayer player_{nullptr};
  Playback::MediaPlaybackList list_{nullptr};
  std::vector<std::function<void()>> revoke_;
  jaw::SourceNode tree_;
  std::vector<EncodableMap> leaves_;
  std::unique_ptr<Result> pending_load_;
  std::vector<std::unique_ptr<Result>> pending_play_;
  uint64_t generation_ = 0;
  bool disposed_ = false;
  bool source_set_ = false;
  bool loading_ = false;
  bool failed_ = false;
  bool completed_ = false;
  bool playing_ = false;
  bool seek_pending_ = false;
  int64_t seek_position_ = 0;
  bool seek_in_progress_ = false;
  int64_t target_seek_position_ = 0;
  std::chrono::steady_clock::time_point seek_deadline_{};
  int64_t requested_index_ = 0;
  Playback::MediaPlaybackItem pending_item_{nullptr};
  int loop_mode_ = 0;
  int shuffle_mode_ = 0;
  double volume_ = 1.0;
  double speed_ = 1.0;

  auto Enqueue() {
    std::weak_ptr<AudioPlayer> weak = shared_from_this();
    std::weak_ptr<PlatformThreadDispatcher> dispatcher = dispatcher_;
    const auto generation = generation_;
    return [weak, dispatcher, generation](std::function<void(AudioPlayer&)> action) {
      if (auto queue = dispatcher.lock()) {
        queue->Post([weak, generation, action = std::move(action)] {
          auto owner = weak.lock();
          if (!owner || owner->disposed_ || owner->generation_ != generation) return;
          try { action(*owner); }
          catch (const winrt::hresult_error& error) { owner->Fail(winrt::to_string(error.message())); }
          catch (const jaw::ArgumentError& error) { owner->Fail(error.message); }
          catch (...) { owner->Fail("Native playback callback failed"); }
        });
      }
    };
  }

  void ReleaseNative() {
    for (auto& revoke : revoke_) { try { revoke(); } catch (...) {} }
    revoke_.clear();
    if (player_) { try { player_.Close(); } catch (...) {} }
    player_ = nullptr;
    list_ = nullptr;
  }

  void ResetNative() {
    ++generation_;
    ReleaseNative();
    player_ = Playback::MediaPlayer();
    list_ = Playback::MediaPlaybackList();
    player_.CommandManager().IsEnabled(false);
    player_.AutoPlay(false);
    player_.Volume(volume_);
    player_.PlaybackSession().PlaybackRate(speed_);
    list_.MaxPlayedItemsToKeepOpen(2);
    auto enqueue = Enqueue();
    auto session = player_.PlaybackSession();
    auto state = session.PlaybackStateChanged([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) { owner.Broadcast(); });
    });
    revoke_.push_back([session, state] { session.PlaybackStateChanged(state); });
    auto duration = session.NaturalDurationChanged([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) { owner.Broadcast(); });
    });
    revoke_.push_back([session, duration] { session.NaturalDurationChanged(duration); });
    auto buffer = session.BufferingProgressChanged([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) { owner.Broadcast(); });
    });
    revoke_.push_back([session, buffer] { session.BufferingProgressChanged(buffer); });
    auto download = session.DownloadProgressChanged([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) { owner.Broadcast(); });
    });
    revoke_.push_back([session, download] { session.DownloadProgressChanged(download); });
    auto seekCompleted = session.SeekCompleted([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) {
        owner.seek_in_progress_ = false;
        owner.Broadcast();
      });
    });
    revoke_.push_back([session, seekCompleted] { session.SeekCompleted(seekCompleted); });
    auto player = player_;
    auto opened = player.MediaOpened([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) { owner.Opened(); });
    });
    revoke_.push_back([player, opened] { player.MediaOpened(opened); });
    auto ended = player.MediaEnded([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) {
        if (owner.loop_mode_ != 0) return;
        owner.completed_ = true;
        owner.Broadcast();
        owner.CompletePlay();
      });
    });
    revoke_.push_back([player, ended] { player.MediaEnded(ended); });
    auto failed = player.MediaFailed([enqueue](auto, const Playback::MediaPlayerFailedEventArgs& args) {
      const auto message = winrt::to_string(args.ErrorMessage());
      enqueue([message](AudioPlayer& owner) { owner.Fail(message); });
    });
    revoke_.push_back([player, failed] { player.MediaFailed(failed); });
    auto list = list_;
    auto changed = list.CurrentItemChanged([enqueue](auto, auto) {
      enqueue([](AudioPlayer& owner) { owner.completed_ = false; owner.Broadcast(); });
    });
    revoke_.push_back([list, changed] { list.CurrentItemChanged(changed); });
    auto itemFailed = list.ItemFailed([enqueue](auto, const Playback::MediaPlaybackItemFailedEventArgs& args) {
      auto item = args.Item();
      auto message = winrt::to_string(winrt::hresult_error(args.Error().ExtendedError()).message());
      enqueue([item, message](AudioPlayer& owner) {
        // A prefetched future item failing must not abort a different load,
        // but the item the pending load is waiting on must. When the initial
        // item fails to open the list never reports it as current, so compare
        // against the item this load designated instead of CurrentItem().
        if (!owner.loading_ || item == owner.pending_item_) owner.Fail(message);
      });
    });
    revoke_.push_back([list, itemFailed] { list.ItemFailed(itemFailed); });
  }

  // Thread-safe self-healing entry point: callable from any thread (native
  // WinRT callbacks included). Reads only the construction-time dispatcher
  // and the weak self handle here; generation_ and player state are touched
  // exclusively inside the posted action, on the dispatcher thread, keeping
  // the exact threading discipline of every other native path.
  void PostRecoveryBroadcast() {
    if (recovery_broadcast_pending_.exchange(true)) return;
    std::weak_ptr<AudioPlayer> weak = weak_from_this();
    std::weak_ptr<PlatformThreadDispatcher> dispatcher = dispatcher_;
    if (auto queue = dispatcher.lock()) {
      queue->Post([weak] {
        if (auto owner = weak.lock()) {
          // Keep the dedupe flag active while Broadcast() runs so that any
          // send failure during recovery cannot trigger recursive re-entry.
          struct PendingGuard {
            std::atomic<bool>& flag;
            ~PendingGuard() { flag.store(false); }
          } guard{owner->recovery_broadcast_pending_};
          owner->Broadcast();
        }
      });
    } else {
      recovery_broadcast_pending_.store(false);
    }
  }

  void CancelLoad(const std::string& message) {
    if (pending_load_) {
      auto result = std::move(pending_load_);
      result->Error("abort", message);
    }
  }
  void CompletePlay() {
    auto results = std::move(pending_play_);
    pending_play_.clear();
    for (auto& result : results) result->Success(EncodableMap());
  }
  // Marks a seek as in flight. WinRT applies Position() asynchronously, so
  // until SeekCompleted fires the session may still report the pre-seek
  // position (or zero on a fresh item). While the flag is set, Broadcast()
  // reports the target position instead of the stale one. A deadline guards
  // against SeekCompleted never firing (item switch, pipeline abort, failure).
  void BeginSeek(int64_t position) {
    seek_in_progress_ = true;
    target_seek_position_ = position;
    seek_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  }
  void ClearSeek() {
    seek_in_progress_ = false;
    target_seek_position_ = 0;
  }
  void Opened() {
    if (failed_ || leaves_.empty()) return;
    loading_ = false;
    pending_item_ = nullptr;
    if (seek_pending_) {
      BeginSeek(seek_position_);
      player_.PlaybackSession().Position(TimeSpan(std::chrono::microseconds(seek_position_)));
      seek_pending_ = false;
    }
    if (playing_) player_.Play();
    Broadcast();
    if (pending_load_) {
      int64_t duration = 0;
      try { duration = TimeSpanToMicroseconds(player_.PlaybackSession().NaturalDuration()); } catch (...) {}
      auto result = std::move(pending_load_);
      result->Success(EncodableMap{{EncodableValue("duration"), duration > 0 ? EncodableValue(duration) : EncodableValue()}});
    }
  }
  void Fail(const std::string& message) {
    if (failed_ || disposed_) return;
    failed_ = true;
    loading_ = false;
    pending_item_ = nullptr;
    completed_ = false;
    ClearSeek();
    try { Broadcast(message.empty() ? "Native media source failed" : message); } catch (...) { JAW_ERROR("Unable to broadcast playback failure"); }
    if (pending_load_) {
      auto result = std::move(pending_load_);
      result->Error("1", message);
    }
    CompletePlay();
  }

  void ApplyModes() {
    player_.IsLoopingEnabled(loop_mode_ == 1);
    list_.AutoRepeatEnabled(loop_mode_ == 2);
    list_.ShuffleEnabled(shuffle_mode_ == 1);
    ApplyShuffle();
  }
  void ApplyShuffle() {
    if (leaves_.empty()) return;
    std::vector<size_t> order;
    tree_.Order(order);
    std::vector<Playback::MediaPlaybackItem> shuffled;
    for (auto index : order) shuffled.push_back(list_.Items().GetAt(static_cast<uint32_t>(index)));
    list_.SetShuffledItems(shuffled);
  }

  Playback::MediaPlaybackItem CreateItem(const EncodableMap& source) {
    const auto& type = jaw::Require<std::string>(source, "type");
    const auto& uriSource = type == "clipping" ? jaw::Require<EncodableMap>(source, "child") : source;
    auto media = MediaSource::CreateFromUri(Uri(TO_WIDESTRING(EncodeSpacesInUri(jaw::Require<std::string>(uriSource, "uri")))));
    if (type != "clipping") return Playback::MediaPlaybackItem(media);
    auto start = jaw::OptionalInteger(source, "start");
    auto end = jaw::OptionalInteger(source, "end", -1);
    if (end >= 0) return Playback::MediaPlaybackItem(media, TimeSpan(std::chrono::microseconds(start)), TimeSpan(std::chrono::microseconds(end - start)));
    return Playback::MediaPlaybackItem(media, TimeSpan(std::chrono::microseconds(start)));
  }

  void Load(jaw::SourceNode tree, int64_t index, int64_t position) {
    const auto size = tree.Size();
    if (index < 0 || (size && static_cast<uint64_t>(index) >= size) || (!size && index != 0) || position < 0)
      throw jaw::ArgumentError{"Invalid initial index or position"};
    std::vector<EncodableMap> leaves;
    tree.Flatten(leaves);
    std::vector<Playback::MediaPlaybackItem> items;
    for (const auto& leaf : leaves) items.push_back(CreateItem(leaf));
    ResetNative();
    ClearSeek();
    tree_ = std::move(tree);
    leaves_ = std::move(leaves);
    loading_ = !leaves_.empty();
    source_set_ = true;
    failed_ = completed_ = false;
    seek_pending_ = true;
    seek_position_ = position;
    requested_index_ = index;
    for (const auto& item : items) list_.Items().Append(item);
    pending_item_ = items.empty() ? Playback::MediaPlaybackItem{nullptr} : items[static_cast<size_t>(index)];
    if (!items.empty()) list_.StartingItem(items[static_cast<size_t>(index)]);
    ApplyModes();
    Broadcast();
    if (!items.empty()) player_.Source(list_.as<Playback::IMediaPlaybackSource>());
    else {
      auto result = std::move(pending_load_);
      if (result) result->Success(EncodableMap{{EncodableValue("duration"), EncodableValue()}});
      CompletePlay();
    }
  }

  void Mutate(const std::string& method, const EncodableMap& args) {
    auto tree = tree_;
    tree.Mutate(method, args);
    std::vector<EncodableMap> leaves;
    tree.Flatten(leaves);
    auto items = list_.Items();
    auto current = list_.CurrentItem();
    auto position = player_.PlaybackSession().Position();
    std::vector<bool> used(leaves_.size(), false);
    std::vector<Playback::MediaPlaybackItem> desired;
    for (const auto& leaf : leaves) {
      size_t match = 0;
      while (match < leaves_.size() && (used[match] || leaves_[match] != leaf)) ++match;
      if (match < leaves_.size()) {
        used[match] = true;
        desired.push_back(items.GetAt(static_cast<uint32_t>(match)));
      } else desired.push_back(CreateItem(leaf));
    }
    // Validate and construct every new item before touching the live list.
    for (uint32_t index = 0; index < desired.size(); ++index) {
      if (index < items.Size() && items.GetAt(index) == desired[index]) continue;
      uint32_t found = index;
      while (found < items.Size() && items.GetAt(found) != desired[index]) ++found;
      if (found < items.Size()) items.RemoveAt(found);
      items.InsertAt(index, desired[index]);
    }
    while (items.Size() > desired.size()) items.RemoveAtEnd();
    tree_ = std::move(tree);
    leaves_ = std::move(leaves);
    // A playlist that was loaded empty never attached the list to the player;
    // the first insertion must attach it or playback stays silent.
    if (!leaves_.empty() && player_.Source() == nullptr) {
      player_.Source(list_.as<Playback::IMediaPlaybackSource>());
    }
    ApplyModes();
    auto found = std::find(desired.begin(), desired.end(), current);
    if (current && found != desired.end() && list_.CurrentItem() != current) {
      list_.MoveTo(static_cast<uint32_t>(found - desired.begin()));
      requested_index_ = static_cast<int64_t>(found - desired.begin());
      seek_pending_ = true;
      seek_position_ = TimeSpanToMicroseconds(position);
    }
    if (leaves_.empty()) { completed_ = true; CompletePlay(); }
    Broadcast();
  }

  void HandleMethodCall(const flutter::MethodCall<EncodableValue>& call,
                        std::unique_ptr<Result> result) {
    if (disposed_) return result->Error("disposed", "Player has been disposed");
    const auto& method = call.method_name();
    const auto* args = call.arguments() ? std::get_if<EncodableMap>(call.arguments()) : nullptr;
    if (!args) return result->Error("argument_error", "Method arguments must be a map");
    try {
      if (method == "load") {
        auto tree = jaw::SourceNode::Parse(jaw::Require<EncodableMap>(*args, "audioSource"));
        auto index = jaw::OptionalInteger(*args, "initialIndex");
        auto position = jaw::OptionalInteger(*args, "initialPosition");
        // Cancel the previous load even when the new request later fails.
        CancelLoad("Replaced by a new load");
        failed_ = false;
        pending_load_ = std::move(result);
        Load(std::move(tree), index, position);
        return;
      } else if (method == "play") {
        player_.Play();
        playing_ = true;
        pending_play_.push_back(std::move(result));
        if (completed_ || failed_) CompletePlay();
        // Restarting after a natural end must re-arm the completion lifecycle:
        // without clearing the flag the plugin keeps reporting
        // ProcessingState.completed forever, so just_audio never sees `ready`
        // again and completion/repeat listeners stall on the second pass.
        completed_ = false;
        Broadcast();
        return;
      } else if (method == "pause" || method == "stop") {
        player_.Pause();
        playing_ = false;
        if (method == "stop") player_.PlaybackSession().Position(TimeSpan::zero());
        CompletePlay();
      } else if (method == "setVolume" || method == "setSpeed") {
        auto value = jaw::Require<double>(*args, method == "setVolume" ? "volume" : "speed");
        if (!std::isfinite(value) || (method == "setVolume" ? value < 0 || value > 1 : value <= 0))
          throw jaw::ArgumentError{"Invalid volume or speed"};
        if (method == "setVolume") { player_.Volume(value); volume_ = value; }
        else { player_.PlaybackSession().PlaybackRate(value); speed_ = value; }
      } else if (method == "setPitch" || method == "setSkipSilence") {
        if (method == "setPitch" && jaw::Require<double>(*args, "pitch") == 1.0)
          return result->Success(EncodableMap());
        if (method == "setSkipSilence" && !jaw::Require<bool>(*args, "enabled"))
          return result->Success(EncodableMap());
        return result->Error("unsupported", method + " is not supported on Windows");
      } else if (method == "setLoopMode" || method == "setShuffleMode") {
        auto mode = jaw::Integer(*args, method == "setLoopMode" ? "loopMode" : "shuffleMode");
        if (mode < 0 || mode > (method == "setLoopMode" ? 2 : 1)) throw jaw::ArgumentError{"Invalid playback mode"};
        if (method == "setLoopMode") loop_mode_ = static_cast<int>(mode);
        else shuffle_mode_ = static_cast<int>(mode);
        ApplyModes();
      } else if (method == "setShuffleOrder") {
        auto tree = tree_;
        tree.UpdateShuffle(jaw::SourceNode::Parse(jaw::Require<EncodableMap>(*args, "audioSource")));
        tree_ = std::move(tree);
        ApplyShuffle();
      } else if (method == "seek") {
        auto index = jaw::OptionalInteger(*args, "index", -1);
        auto position = jaw::OptionalInteger(*args, "position", 0);
        if (index < -1 || (index >= 0 && static_cast<uint64_t>(index) >= leaves_.size()) || position < 0)
          throw jaw::ArgumentError{"Invalid seek index or position"};
        completed_ = false;
        if (index >= 0) requested_index_ = index;
        if (index >= 0 && list_.CurrentItemIndex() != static_cast<uint32_t>(index)) {
          seek_pending_ = true;
          seek_position_ = position;
          // CurrentItemChanged fires as soon as MoveTo switches items and
          // broadcasts a zero position for the fresh item; keep Broadcast()
          // reporting the requested position until Opened() lands the seek.
          BeginSeek(position);
          list_.MoveTo(static_cast<uint32_t>(index));
        } else {
          BeginSeek(position);
          player_.PlaybackSession().Position(TimeSpan(std::chrono::microseconds(position)));
        }
        if (playing_) player_.Play();
      } else if (method == "concatenatingInsertAll" || method == "concatenatingRemoveRange" || method == "concatenatingMove") {
        Mutate(method, *args);
      } else if (method == "setAndroidAudioAttributes") {
        // just_audio sends this during platform initialization on every OS.
      } else if (method == "dispose") {
        Dispose();
      } else {
        return result->NotImplemented();
      }
      if (!disposed_) Broadcast();
      result->Success(EncodableMap());
    } catch (const jaw::ArgumentError& error) {
      if (result) result->Error("argument_error", error.message);
      else Fail(error.message);
    } catch (const winrt::hresult_error& error) {
      if (result) result->Error("native_error", winrt::to_string(error.message()));
      else Fail(winrt::to_string(error.message()));
    } catch (const std::exception& error) {
      if (result) result->Error("native_error", error.what());
      else Fail(error.what());
    } catch (...) {
      if (result) result->Error("native_error", "Unknown native playback error");
      else Fail("Unknown native playback error");
    }
  }

  int ProcessingState() const {
    if (failed_ || !source_set_) return 0;
    if (completed_) return 4;
    if (loading_) return 1;
    if (leaves_.empty()) return 3;
    auto state = player_.PlaybackSession().PlaybackState();
    if (state == Playback::MediaPlaybackState::Opening) return 1;
    if (state == Playback::MediaPlaybackState::Buffering) return 2;
    return 3;
  }
  void Broadcast(const std::string& error = {}) {
    if (disposed_ || !player_) return;
    auto session = player_.PlaybackSession();
    // WinRT properties can be temporarily unavailable while opening or
    // switching items. A missing progress sample must not fail playback.
    int64_t duration = 0;
    int64_t position = 0;
    double progress = 0.0;
    try { duration = TimeSpanToMicroseconds(session.NaturalDuration()); } catch (...) {}
    try { position = TimeSpanToMicroseconds(session.Position()); } catch (...) {}
    try { progress = session.DownloadProgress(); } catch (...) {}
    if (seek_in_progress_) {
      // While a seek is landing on the media pipeline, session.Position()
      // still reports the pre-seek value (or zero on a fresh item), which
      // confuses Dart-side bookkeeping. Report the requested target instead,
      // unless the completion event was lost and the safety deadline passed.
      if (std::chrono::steady_clock::now() >= seek_deadline_) {
        ClearSeek();
      } else {
        position = target_seek_position_;
      }
    }
    // CurrentItemIndex is UINT32_MAX or a default 0 before the list settles.
    // Reporting a spurious index there briefly desynchronizes Dart; while the
    // initial load is pending, the requested index is the truthful one.
    const auto rawIndex = list_.CurrentItemIndex();
    const bool indexKnown = rawIndex != UINT32_MAX && static_cast<size_t>(rawIndex) < leaves_.size();
    const auto index = indexKnown ? static_cast<size_t>(rawIndex) : size_t{0};
    if (indexKnown) {
      const auto& leaf = leaves_[index];
      const auto& source = jaw::Require<std::string>(leaf, "type") == "clipping" ? jaw::Require<EncodableMap>(leaf, "child") : leaf;
      const auto& uri = jaw::Require<std::string>(source, "uri");
      if (uri.compare(0, 5, "file:") == 0) progress = 1.0;
    }
    int64_t reportedIndex = -1;
    if (loading_) reportedIndex = static_cast<int64_t>(requested_index_);
    else if (indexKnown) reportedIndex = static_cast<int64_t>(rawIndex);
    auto now = std::chrono::system_clock::now();
    EncodableMap event{
      {EncodableValue("processingState"), EncodableValue(ProcessingState())},
      {EncodableValue("updatePosition"), EncodableValue(position)},
      {EncodableValue("updateTime"), EncodableValue(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()))},
      {EncodableValue("bufferedPosition"), EncodableValue((std::max)(position, ClampBufferedPosition(duration, progress)))},
      {EncodableValue("duration"), duration > 0 ? EncodableValue(duration) : EncodableValue()},
      {EncodableValue("currentIndex"), reportedIndex >= 0 ? EncodableValue(reportedIndex) : EncodableValue()}
    };
    if (!error.empty()) {
      event[EncodableValue("errorCode")] = EncodableValue(int32_t{1});
      event[EncodableValue("errorMessage")] = EncodableValue(error);
    }
    if (event_sink_) event_sink_->Success(event);
    if (data_sink_) data_sink_->Success(EncodableMap{
      {EncodableValue("playing"), EncodableValue(playing_)},
      {EncodableValue("volume"), EncodableValue(volume_)},
      {EncodableValue("speed"), EncodableValue(speed_)},
      {EncodableValue("loopMode"), EncodableValue(loop_mode_)},
      {EncodableValue("shuffleMode"), EncodableValue(shuffle_mode_)}
    });
  }
};
