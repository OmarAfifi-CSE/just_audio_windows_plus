#pragma once
#pragma comment(lib, "windowsapp")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include "source_model.hpp"
#include <functional>
#include <iostream>
#include <limits>
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

#define JAW_ERROR(expr) do { std::cerr << "[just_audio_windows_plus] " << expr << std::endl; } while (0)

using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Uri;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using winrt::Windows::Media::Core::MediaSource;

using flutter::EncodableMap;
using flutter::EncodableValue;

inline int64_t TimeSpanToMicroseconds(winrt::Windows::Foundation::TimeSpan timespan) {
  // One tick is 100 nanoseconds; one microsecond is 10 ticks.
  return timespan.count() / 10;
}

// Converts caller-supplied microseconds to a TimeSpan, clamped to the range
// TimeSpan can represent. A duration is 100 ns ticks, so a microsecond value
// near int64_t max would overflow the tick conversion (UB); positions beyond
// ~292,000 years carry no meaning anyway, and every input here is validated
// non-negative.
inline winrt::Windows::Foundation::TimeSpan MicrosToTimeSpan(int64_t microseconds) {
  constexpr int64_t kMaxMicros = (std::numeric_limits<int64_t>::max)() / 10;
  return winrt::Windows::Foundation::TimeSpan(std::chrono::microseconds(
      (std::min)((std::max)(microseconds, int64_t{0}), kMaxMicros)));
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

  bool Success(const EncodableValue& event) {
    bool schedule_recovery = false;
    bool delivered = false;
    {
      std::lock_guard<std::mutex> lock(sink_mutex_);
      if (sink) {
        try {
          sink->Success(event);
          delivered = true;
        } catch (...) {
          try {
            sink->Success(event);
            delivered = true;
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
    return delivered;
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
  // Bound on consecutive recovery-broadcast rounds. Delivery failure under
  // exhaustion would otherwise schedule each next round from the previous
  // one; after this many rounds the loop is abandoned until a state event is
  // delivered successfully again.
  static constexpr int kMaxRecoveryAttempts = 3;
  std::atomic<bool> recovery_broadcast_pending_{false};
  std::atomic<bool> recovery_reschedule_requested_{false};
  std::atomic<int> recovery_attempts_{0};
  Playback::MediaPlayer player_{nullptr};
  Playback::MediaPlaybackList list_{nullptr};
  std::vector<std::function<void()>> revoke_;
  jaw::SourceNode tree_;
  std::vector<EncodableMap> leaves_;
  std::unique_ptr<Result> pending_load_;
  std::vector<std::unique_ptr<Result>> pending_play_;
  // Atomic because PostRecoveryBroadcast() captures it before posting, and
  // that entry point is documented as callable from any thread. Every other
  // access is serialized on the platform thread.
  std::atomic<uint64_t> generation_{0};
  bool disposed_ = false;
  bool source_set_ = false;
  bool loading_ = false;
  bool failed_ = false;
  bool completed_ = false;
  bool playing_ = false;
  bool seek_pending_ = false;
  int64_t seek_position_ = 0;
  // True once the CurrentItemChanged belonging to the armed seek's item
  // switch has been observed. The Broadcast() retry is gated on it so a
  // retry can never apply a position into a session still describing the
  // previous item.
  bool seek_arm_cic_seen_ = false;
  bool seek_in_progress_ = false;
  int64_t target_seek_position_ = 0;
  std::atomic<uint64_t> seek_generation_{0};
  std::chrono::steady_clock::time_point seek_deadline_{};
  int64_t requested_index_ = 0;
  Playback::MediaPlaybackItem pending_item_{nullptr};
  int loop_mode_ = 0;
  int shuffle_mode_ = 0;
  double volume_ = 1.0;
  double speed_ = 1.0;
  // Last map handed to the data sink, used to suppress redundant republication.
  EncodableMap published_data_;

  auto Enqueue() {
    std::weak_ptr<AudioPlayer> weak = shared_from_this();
    std::weak_ptr<PlatformThreadDispatcher> dispatcher = dispatcher_;
    const auto generation = generation_.load();
    return [weak, dispatcher, generation](std::function<void(AudioPlayer&)> action) {
      if (auto queue = dispatcher.lock()) {
        // Post() allocates, so it can throw under exhaustion. This lambda
        // runs inside a WinRT event callback, and an exception escaping the
        // WinRT raise boundary risks terminating the process — dropping the
        // one event broadcast is the correct degradation.
        try {
          queue->Post([weak, generation, action = std::move(action)] {
            auto owner = weak.lock();
            if (!owner || owner->disposed_ || owner->generation_ != generation) return;
            try { action(*owner); }
            catch (const winrt::hresult_error& error) { owner->Fail(winrt::to_string(error.message())); }
            catch (const jaw::ArgumentError& error) { owner->Fail(error.message); }
            catch (...) { owner->Fail("Native playback callback failed"); }
          });
        } catch (...) {
          JAW_ERROR("Unable to dispatch native playback callback");
        }
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
    // Capture seek generation on the WinRT thread-pool before posting so
    // an earlier seek's SeekCompleted cannot clear a newer fake-position.
    std::weak_ptr<AudioPlayer> seekWeak = weak_from_this();
    auto seekCompleted = session.SeekCompleted([enqueue, seekWeak](auto, auto) {
      uint64_t gen = 0;
      if (auto sp = seekWeak.lock()) gen = sp->seek_generation_.load();
      enqueue([gen](AudioPlayer& owner) {
        owner.OnSeekCompleted(gen);
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
      enqueue([](AudioPlayer& owner) {
        // Mark the arm's switch as observed *before* any application, so
        // the Broadcast() retry can never fire inside the pre-switch window.
        owner.seek_arm_cic_seen_ = true;
        owner.completed_ = false;
        // A cross-item seek whose target is still open in the keep-open
        // window never receives MediaOpened, so Opened() would never
        // consume the deferred seek. Apply it here when the pipeline is not
        // mid-open; fresh items stay deferred for Opened().
        owner.ApplyDeferredSeekIfReady();
        owner.Broadcast();
      });
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
    if (recovery_broadcast_pending_.exchange(true)) {
      recovery_reschedule_requested_.store(true);
      return;
    }
    // Bound the self-retry loop: under sustained delivery failure each
    // recovery round immediately schedules the next one. The budget resets
    // whenever a state event is delivered successfully (see Broadcast), so a
    // later transient failure gets a fresh budget instead of a wedged loop.
    if (recovery_attempts_.fetch_add(1) >= kMaxRecoveryAttempts) {
      recovery_reschedule_requested_.store(false);
      recovery_broadcast_pending_.store(false);
      JAW_ERROR("Recovery broadcast abandoned after " << kMaxRecoveryAttempts << " attempts");
      return;
    }
    const auto generation_at_post = generation_.load();
    std::weak_ptr<AudioPlayer> weak = weak_from_this();
    std::weak_ptr<PlatformThreadDispatcher> dispatcher = dispatcher_;
    if (auto queue = dispatcher.lock()) {
      bool posted = false;
      try {
        posted = queue->Post([weak, generation_at_post] {
          if (auto owner = weak.lock()) {
            if (owner->disposed_ || owner->generation_ != generation_at_post) {
              owner->recovery_attempts_.store(0);
              owner->recovery_broadcast_pending_.store(false);
              if (owner->recovery_reschedule_requested_.exchange(false)) {
                owner->PostRecoveryBroadcast();
              }
              return;
            }
            struct ReschedulingGuard {
              AudioPlayer* owner;
              ~ReschedulingGuard() {
                owner->recovery_broadcast_pending_.store(false);
                if (owner->recovery_reschedule_requested_.exchange(false)) {
                  // Runs in a noexcept destructor: a throw here would
                  // terminate the process, so contain it explicitly.
                  try { owner->PostRecoveryBroadcast(); }
                  catch (...) { JAW_ERROR("Unable to reschedule recovery broadcast"); }
                } else {
                  owner->recovery_attempts_.store(0);
                }
              }
            } guard{owner.get()};
            try {
              owner->Broadcast(/*force_data=*/true);
            } catch (const winrt::hresult_error& error) {
              owner->Fail(winrt::to_string(error.message()));
            } catch (const std::exception& error) {
              owner->Fail(error.what());
            } catch (...) {
              owner->Fail("Recovery broadcast failed");
            }
          }
        });
      } catch (...) {
        // Post() allocates; under exhaustion it can throw instead of
        // returning false. Treat that as a failed post so the pending flag
        // cannot wedge on a task that will never run.
        posted = false;
      }
      if (!posted) {
        recovery_broadcast_pending_.store(false);
        if (recovery_reschedule_requested_.exchange(false)) {
          PostRecoveryBroadcast();
        }
      }
    } else {
      recovery_broadcast_pending_.store(false);
      if (recovery_reschedule_requested_.exchange(false)) {
        PostRecoveryBroadcast();
      }
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
    // Serialize seek intents: a new seek overwrites the target but extends
    // the deadline only if later, and a SeekCompleted for an earlier seek
    // must not clear the newer target.
    seek_in_progress_ = true;
    target_seek_position_ = position;
    seek_generation_++;
    // Built from a monotonic clock, so this can only ever move the deadline
    // forward: a rapid scrub can never shorten an already-armed window.
    seek_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  }

  void OnSeekCompleted(uint64_t generation_at_post) {
    // Only called via Enqueue on platform thread for the MediaPlayer's own
    // SeekCompleted. Ignore completions for an earlier seek that was
    // superseded by a newer BeginSeek — otherwise the fake position snaps.
    if (generation_at_post != seek_generation_) return;
    seek_in_progress_ = false;
    // A seek issued while the pipeline was switching items can swallow the
    // Play() that accompanied it: the session was still transitional. Once
    // the pipeline confirms the seek landed, re-assert the intent so a rapid
    // cross-item scrub cannot leave the audio frozen while Dart believes it
    // is playing.
    if (playing_ && source_set_ && !completed_ && !loading_) {
      try { player_.Play(); } catch (...) {}
    }
    Broadcast();
  }
  // Applies the deferred seek when the target item is already open. Called
  // from CurrentItemChanged (the definitive switch-settled signal) and — as
  // a retry — from Broadcast(); a no-op unless a deferred seek is armed,
  // the arm's own CurrentItemChanged has been observed (so a retry can never
  // race the item switch the arm is waiting on), the initial load has left
  // its opening window, and the pipeline is not opening a fresh item (that
  // case stays deferred for Opened(), which fires when the item becomes
  // available).
  void ApplyDeferredSeekIfReady() {
    if (!seek_pending_ || !seek_arm_cic_seen_ || loading_ || disposed_) return;
    try {
      // During an item switch the list can briefly report no current item;
      // applying a position into that void loses it for good.
      if (list_.CurrentItemIndex() == UINT32_MAX) return;
      if (player_.PlaybackSession().PlaybackState() == Playback::MediaPlaybackState::Opening) return;
      BeginSeek(seek_position_);
      player_.PlaybackSession().Position(MicrosToTimeSpan(seek_position_));
      seek_pending_ = false;
    } catch (...) {
      // WinRT properties can be transiently unavailable while the list is
      // switching items; the deferred seek stays armed — Broadcast() retries.
    }
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
      // Apply only when the item that opened is the one the arm targets.
      // A cross-item seek racing the load's in-flight open can deliver this
      // MediaOpened for the *previous* item after MoveTo already switched the
      // list; applying blindly would land the position on the wrong item.
      // Declining leaves the arm set — Broadcast() retries it once the
      // pipeline settles on the intended item.
      if (list_.CurrentItemIndex() == static_cast<uint32_t>(requested_index_)) {
        BeginSeek(seek_position_);
        player_.PlaybackSession().Position(MicrosToTimeSpan(seek_position_));
        seek_pending_ = false;
      }
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
    // The engine reports idle from here on, so any audio still sounding must
    // stop: a replacement load whose items fail to construct (e.g. a malformed
    // URI) throws after the old source was left attached, and without this the
    // old track keeps playing while Dart believes the player is idle. Pausing
    // alone is not enough — a later play() would resume the paused orphan —
    // so the source is detached outright and every Play() site (the play
    // handler, the seek handler's resume, the self-heals) becomes a no-op on
    // this player until the next successful load re-arms the engine.
    if (player_) {
      try { player_.Pause(); } catch (...) {}
      try { player_.Source(nullptr); } catch (...) {}
    }
    // Roll the source back so a later play() cannot wait on media that will
    // never open. Load() re-arms it, so a retry after a failure still works.
    source_set_ = false;
    // A failed engine must not keep a deferred seek armed: no Opened() is
    // coming to consume it, so a later same-item seek would take the deferral
    // branch and defer onto a dead arm forever. Mirrors `stop`'s hygiene and
    // the pinned contract in the Dart mock.
    ClearSeek();
    seek_pending_ = false;
    seek_position_ = 0;
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
    // SetShuffledItems defines the automatic progression order regardless of
    // ShuffleEnabled, so applying it while shuffling is off would play a
    // permuted order. just_audio still sends a shuffled `shuffleOrder` with
    // every load, so this guard is what keeps shuffle-off playback natural.
    if (shuffle_mode_ != 1 || leaves_.empty()) return;
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
    if (end >= 0) return Playback::MediaPlaybackItem(media, MicrosToTimeSpan(start), MicrosToTimeSpan(end - start));
    return Playback::MediaPlaybackItem(media, MicrosToTimeSpan(start));
  }

  // Validates the initial seek before any state is touched. Kept separate from
  // Load() so the caller can reject it as an argument error: Load() runs after
  // the request result has been moved into pending_load_, where a throw can only
  // be reported through Fail() as a generic playback failure.
  static void ValidateInitialSeek(uint64_t size, int64_t index, int64_t position) {
    if (index < 0 || (size && static_cast<uint64_t>(index) >= size) ||
        (!size && index != 0) || position < 0)
      throw jaw::ArgumentError{"Invalid initial index or position"};
  }

  void Load(jaw::SourceNode tree, int64_t index, int64_t position) {
    ValidateInitialSeek(tree.Size(), index, position);
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
    // An empty playlist can never open an item, so arming a deferred seek
    // there would be an arm nothing consumes (and the Broadcast retry could
    // paint its position onto an idle engine).
    seek_pending_ = !leaves_.empty();
    seek_position_ = position;
    seek_arm_cic_seen_ = false;
    requested_index_ = index;
    for (const auto& item : items) list_.Items().Append(item);
    pending_item_ = items.empty() ? Playback::MediaPlaybackItem{nullptr} : items[static_cast<size_t>(index)];
    if (!items.empty()) list_.StartingItem(items[static_cast<size_t>(index)]);
    ApplyModes();
    // Report the requested initial position from the first broadcast onward:
    // the fresh session reports 0 until the item opens, so without this a
    // seek bar would briefly show 0 and then jump when the deferred seek
    // lands. The 2s deadline still guards a pathologically slow open.
    if (!leaves_.empty() && position > 0) BeginSeek(position);
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
    // Captured before any list edit: where the current item sits, so a
    // mutation that removes it can advance to the item that shifts into
    // its position, exactly as the Android implementation does.
    const auto current_index = list_.CurrentItemIndex();
    // Mirrors Broadcast(): WinRT properties can be temporarily unavailable while
    // the pipeline switches items, and a missing sample must not fail the
    // mutation.
    int64_t position = 0;
    try { position = TimeSpanToMicroseconds(player_.PlaybackSession().Position()); } catch (...) {}
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
    // Snapshot original items for transactional rollback if any WinRT COM
    // mutation fails. Snapshot itself can throw (COM), so it must not mutate
    // the live list on failure.
    std::vector<Playback::MediaPlaybackItem> original_items;
    try {
      for (uint32_t i = 0; i < items.Size(); ++i) {
        original_items.push_back(items.GetAt(i));
      }
    } catch (...) {
      JAW_ERROR("Failed to snapshot MediaPlaybackList for transactional mutate");
      throw;
    }

    try {
      // Validate and construct every new item before touching the live list.
      for (uint32_t index = 0; index < desired.size(); ++index) {
        if (index < items.Size() && items.GetAt(index) == desired[index]) continue;
        uint32_t found = index;
        while (found < items.Size() && items.GetAt(found) != desired[index]) ++found;
        if (found < items.Size()) items.RemoveAt(found);
        items.InsertAt(index, desired[index]);
      }
      while (items.Size() > desired.size()) items.RemoveAtEnd();
    } catch (...) {
      // In-place restore without Clear(): Clear()+Append can truncate on OOM
      // precisely when rollback is most needed. Restore by trimming and
      // patching individual indices so a second OOM leaves prefix intact.
      bool rollback_ok = true;
      try {
        while (items.Size() > original_items.size()) {
          items.RemoveAtEnd();
        }
        while (items.Size() < original_items.size()) {
          items.Append(original_items[items.Size()]);
        }
        for (uint32_t i = 0; i < original_items.size(); ++i) {
          if (items.GetAt(i) != original_items[i]) {
            items.RemoveAt(i);
            items.InsertAt(i, original_items[i]);
          }
        }
      } catch (...) {
        rollback_ok = false;
        JAW_ERROR("Failed to rollback MediaPlaybackList after mutation exception");
      }
      // If in-place rollback also failed, the list may be truncated/corrupt.
      // Escalate to a clean reset so the engine does not retain a half-state
      // that corrupts future Broadcast/ApplyShuffle. The tree_ has not yet been
      // committed, so Dart and native can re-converge on next Load.
      if (!rollback_ok) {
        try {
          // Best-effort: detach source and rebuild from committed tree_
          // (old tree_, not the mutated one) on next Load. For now, ensure
          // list is at least empty rather than half-truncated.
          items.Clear();
        } catch (...) {}
      }
      throw;
    }
    tree_ = std::move(tree);
    leaves_ = std::move(leaves);
    // A playlist that was loaded empty never attached the list to the player;
    // the first insertion must attach it or playback stays silent.
    if (!leaves_.empty() && player_.Source() == nullptr) {
      player_.Source(list_.as<Playback::IMediaPlaybackSource>());
    }
    ApplyModes();
    if (current) {
      auto found = std::find(desired.begin(), desired.end(), current);
      if (found != desired.end()) {
        if (list_.CurrentItem() != current) {
          // Best-effort after the tree and list edits are already committed:
          // a transient COM throw from MoveTo must not error the whole
          // mutation (Dart's tree now describes the edited list, so an error
          // here would diverge the two) — it just skips the re-selection,
          // and the surviving item keeps playing where it is.
          try { list_.MoveTo(static_cast<uint32_t>(found - desired.begin())); } catch (...) {
            JAW_ERROR("Mutate: MoveTo to surviving item failed");
          }
          requested_index_ = static_cast<int64_t>(found - desired.begin());
          seek_pending_ = true;
          seek_position_ = position;
          // A same-item MoveTo does not fire CurrentItemChanged, so mark the
          // switch as already observed: the Broadcast retry may run for this
          // arm if the inline application below transiently throws.
          seek_arm_cic_seen_ = true;
          // The surviving item is the same object the session is already
          // describing, so the position re-set is race-free — but MoveTo to
          // the same item does not fire CurrentItemChanged, so nothing else
          // would consume the armed seek and a later same-item seek would
          // defer forever. Consume it here; if WinRT chose to re-open the
          // item for the move, the Position() set is discarded and the arm
          // stays for Opened().
          BeginSeek(position);
          try {
            player_.PlaybackSession().Position(MicrosToTimeSpan(position));
            seek_pending_ = false;
          } catch (...) {
          }
        }
      } else if (!leaves_.empty()) {
        // The currently playing item was removed. Without intervention the
        // list keeps reporting no current item, playback wedges, and every
        // broadcast carries a null duration and index. Advance to the item
        // that shifted into the removed item's position instead; the switch
        // to a different item fires CurrentItemChanged, which applies the
        // armed seek once the pipeline settles (or defers to Opened()).
        const auto next = current_index != UINT32_MAX
            ? (std::min)(current_index, static_cast<uint32_t>(leaves_.size() - 1))
            : static_cast<uint32_t>(0);
        try { list_.MoveTo(next); } catch (...) {
          // Same best-effort contract as above: the tree/list edits are
          // committed, so the mutation succeeds; the advance is skipped.
          JAW_ERROR("Mutate: MoveTo after current-item removal failed");
        }
        requested_index_ = static_cast<int64_t>(next);
        seek_pending_ = true;
        seek_position_ = 0;
        seek_arm_cic_seen_ = false;
        BeginSeek(0);
      }
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
        // Everything that can reject the request as malformed runs while this
        // call still owns its result, so the caller gets argument_error. Once
        // the result moves into pending_load_ the only way out is Fail(), which
        // reports a generic playback failure instead.
        ValidateInitialSeek(tree.Size(), index, position);
        // Cancel the previous load even when the new request later fails.
        CancelLoad("Replaced by a new load");
        failed_ = false;
        pending_load_ = std::move(result);
        Load(std::move(tree), index, position);
        return;
      } else if (method == "play") {
        // A failed engine must not be re-armed by play(): Fail() rolls the
        // source back and detaches it, and a Play() here could otherwise
        // resurrect whatever the failed load left attached.
        if (!failed_) { try { player_.Play(); } catch (...) {} }
        playing_ = true;
        // Coalesce rapid successive play calls: keep at most one pending play
        // future so a double-tap (or 10 Hz spam) does not leak unbounded
        // MethodResults. just_audio may send a second play request while one
        // is in flight (a load() racing play()); its contract says the
        // platform "should ignore the second play request since it is already
        // playing" and that a play issued while already playing "completes
        // immediately" - so the arriving request settles now as "play
        // accepted", while the request already bound to the playback
        // lifecycle keeps waiting for the terminal event (pause/stop/end/
        // fail). Settling the newest instead of the oldest matters: the
        // earlier request is the one whose caller may be awaiting playback,
        // and resolving it early would report completion mid-playback.
        if (!pending_play_.empty()) {
          result->Success(EncodableMap());
        } else {
          pending_play_.push_back(std::move(result));
        }
        // A play settles when MediaEnded or Pause fires. Settle it here only
        // when the engine state is *stably* unplayable, so the future can never
        // be reached by an event that is still coming: a failed engine, or a
        // loaded-but-empty playlist that can never open an item.
        //
        // A missing source is deliberately NOT one of those conditions:
        // just_audio's platform activation sends `play` before `load`, so a play
        // that arrives with nothing attached is the normal first-play path, and
        // settling it would resolve `await player.play()` before the track even
        // starts. Load() settles the play instead when the loaded source turns
        // out to be unplayable, and Fail() settles it when the load errors.
        //
        // A completed engine is NOT terminal here either: the Play() above
        // restarts the ended media/list, so the future must stay bound to the
        // replayed pass and resolve at its end — exactly as the Android
        // implementation behaves.
        if (failed_ || (source_set_ && leaves_.empty()))
          CompletePlay();
        // Restarting after a natural end must re-arm the completion lifecycle:
        // without clearing the flag the plugin keeps reporting
        // ProcessingState.completed forever, so just_audio never sees `ready`
        // again and completion/repeat listeners stall on the second pass.
        completed_ = false;
        Broadcast();
        return;
      } else if (method == "pause" || method == "stop") {
        // The COM calls are guarded individually: a transient throw while the
        // pipeline is switching items must not skip the bookkeeping below and
        // leave Dart's intent diverged from the engine.
        try { player_.Pause(); } catch (...) {}
        playing_ = false;
        if (method == "stop") {
          // Drop both the in-flight seek and the deferred one BEFORE the COM
          // rewind: the deferred position is applied when an item opens, so a
          // throw from Position() must not leave it armed to undo the rewind.
          ClearSeek();
          seek_pending_ = false;
          seek_position_ = 0;
          try { player_.PlaybackSession().Position(TimeSpan::zero()); } catch (...) {}
        }
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
        // An idle or failed engine has nothing to seek. The arguments are
        // still validated above, but no session write and — crucially — no
        // fake-position window: an idle player must never report a position
        // the user "seeked" to, because nothing will ever land there.
        if (!source_set_ || leaves_.empty()) {
          Broadcast();
          return result->Success(EncodableMap());
        }
        completed_ = false;
        if (index >= 0) requested_index_ = index;
        if (index >= 0 && list_.CurrentItemIndex() != static_cast<uint32_t>(index)) {
          seek_pending_ = true;
          seek_position_ = position;
          seek_arm_cic_seen_ = false;
          // CurrentItemChanged fires as soon as MoveTo switches items and
          // broadcasts a zero position for the fresh item; keep Broadcast()
          // reporting the requested position until the seek lands. The
          // application itself happens when the switch settles: for a fresh
          // item MediaOpened fires and Opened() applies the position, and
          // for an item that is still open in the keep-open window
          // (MediaOpened never fires there) CurrentItemChanged applies it
          // via ApplyDeferredSeekIfReady. Applying inline here instead
          // would race the switch: the session can still be describing the
          // previous item.
          BeginSeek(position);
          list_.MoveTo(static_cast<uint32_t>(index));
        } else {
          // A seek issued while the item is still opening would be overwritten
          // by Opened() applying the load's deferred initial position, so arm
          // the deferred seek with the newest position instead of letting the
          // stale one land when the media opens. The direct Position() set is
          // skipped in that window: the pipeline discards it before the open,
          // and Opened() applies the deferred target once the item is open.
          if (loading_ || seek_pending_) {
            seek_pending_ = true;
            seek_position_ = position;
            seek_arm_cic_seen_ = false;
            BeginSeek(position);
          } else {
            BeginSeek(position);
            player_.PlaybackSession().Position(MicrosToTimeSpan(position));
          }
        }
        if (playing_) player_.Play();
      } else if (method == "concatenatingInsertAll" || method == "concatenatingRemoveRange" || method == "concatenatingMove") {
        Mutate(method, *args);
      } else if (method == "setAndroidAudioAttributes") {
        // just_audio sends this during platform initialization on every OS.
      } else if (method == "setAutomaticallyWaitsToMinimizeStalling" ||
                 method == "setCanUseNetworkResourcesForLiveStreamingWhilePaused" ||
                 method == "setPreferredPeakBitRate") {
        // AVPlayer (iOS) knobs exposed on AudioPlayer's cross-platform API.
        // just_audio sends them over the channel whenever the app calls the
        // setter (or activation finds a non-default value), so answering
        // NotImplemented would surface a MissingPluginException from a plain
        // public API. They have no Windows counterpart; accept as a no-op,
        // exactly as the previous implementation did.
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
    Broadcast(/*force_data=*/false, error);
  }

  void Broadcast(bool force_data, const std::string& error = {}) {
    if (disposed_ || !player_) return;
    auto session = player_.PlaybackSession();
    // Retry an armed-but-unapplied deferred seek on every broadcast. The
    // arm's other consumers are one-shot (CurrentItemChanged when the switch
    // settles, Opened() when a fresh item opens); if the single CurrentItem-
    // Changed sample caught the pipeline mid-open — or its Position() call
    // threw transiently — nothing else would ever consume the arm, and every
    // later same-item seek would defer onto it forever. Retrying here makes
    // the application event-driven: the first broadcast after the transient
    // window passes lands the position. Idempotent (guarded by the arm) and
    // inert during a load (loading_ gates the apply).
    ApplyDeferredSeekIfReady();
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
    // Self-heal a stalled session: if Dart's intent is playing but the
    // pipeline reports Paused outside every transitional window, re-assert
    // Play(). Rapid cross-item seeks can swallow the Play() issued alongside
    // them, leaving audio frozen behind a truthful-looking position.
    //
    // Two independent barriers keep this from restarting a track that just
    // ended. (1) The position margin: at a natural end the session pauses
    // essentially at the duration, while a swallowed play stalls mid-track —
    // 250 ms absorbs container/clock skew (VBR estimates, HLS growth) where
    // a tighter margin would misfire. (2) The one-turn deferral: end-of-
    // media also queues a MediaEnded task; deferring the heal by one pump
    // turn lets that task land first, and its completed_ flag cancels the
    // heal. A genuinely swallowed play has no MediaEnded coming, so the
    // deferred heal fires.
    if (playing_ && source_set_ && !loading_ && !completed_ && !failed_ &&
        !seek_in_progress_ &&
        (duration <= 0 || duration - position > 250000)) {
      try {
        if (session.PlaybackState() == Playback::MediaPlaybackState::Paused) {
          auto enqueue = Enqueue();
          enqueue([](AudioPlayer& owner) {
            if (!owner.playing_ || owner.completed_ || owner.failed_ ||
                owner.loading_ || owner.seek_in_progress_ || owner.disposed_) {
              return;
            }
            try {
              if (owner.player_.PlaybackSession().PlaybackState() ==
                  Playback::MediaPlaybackState::Paused) {
                owner.player_.Play();
              }
            } catch (...) {}
          });
        }
      } catch (...) {}
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
    // bufferedPosition drives the app's buffer bar, so it must never exceed the
    // duration. The position itself is deliberately not clamped to the
    // duration: across an item switch the session can still be describing the
    // previous item, and clamping to that stale duration would report a
    // position the user never seeked to.
    const auto buffered = duration > 0
      ? (std::min)((std::max)(position, ClampBufferedPosition(duration, progress)), duration)
      : (std::max)(position, ClampBufferedPosition(duration, progress));
    auto now = std::chrono::system_clock::now();
    EncodableMap event{
      {EncodableValue("processingState"), EncodableValue(ProcessingState())},
      {EncodableValue("updatePosition"), EncodableValue(position)},
      {EncodableValue("updateTime"), EncodableValue(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()))},
      {EncodableValue("bufferedPosition"), EncodableValue(buffered)},
      {EncodableValue("duration"), duration > 0 ? EncodableValue(duration) : EncodableValue()},
      {EncodableValue("currentIndex"), reportedIndex >= 0 ? EncodableValue(reportedIndex) : EncodableValue()}
    };
    if (!error.empty()) {
      event[EncodableValue("errorCode")] = EncodableValue(int32_t{1});
      event[EncodableValue("errorMessage")] = EncodableValue(error);
    }
    if (event_sink_ && event_sink_->Success(event)) {
      // A delivered state event ends any delivery-failure epoch: the next
      // failure gets a fresh recovery budget instead of an exhausted one.
      recovery_attempts_.store(0);
    }
    PublishData(force_data);
  }

  // The player-data map is the player's *intent*, not a progress report: it
  // carries playing, volume, speed, loop and shuffle. just_audio applies every
  // value it receives, so a redundant copy of an unchanged value is not free -
  // it can overwrite a newer decision Dart has already made. It is therefore
  // published only when one of those values actually changes.
  //
  // Crucially, published_data_ is committed only after successful delivery to
  // the sink. If delivery fails or is retried/recovered, the state can be
  // re-delivered or forced without being trapped behind a stale cache.
  void PublishData(bool force = false) {
    EncodableMap data{
      {EncodableValue("playing"), EncodableValue(playing_)},
      {EncodableValue("volume"), EncodableValue(volume_)},
      {EncodableValue("speed"), EncodableValue(speed_)},
      {EncodableValue("loopMode"), EncodableValue(loop_mode_)},
      {EncodableValue("shuffleMode"), EncodableValue(shuffle_mode_)}
    };
    if (!force && data == published_data_) return;
    if (data_sink_) {
      if (data_sink_->Success(data)) {
        published_data_ = std::move(data);
      }
    }
  }
};
