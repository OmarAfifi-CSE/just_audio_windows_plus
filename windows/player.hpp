#pragma comment(lib, "windowsapp")

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// This must be included before many other Windows headers.
#include <windows.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

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

#define TO_MILLISECONDS(timespan) (timespan.count() / 10000)
#define TO_MICROSECONDS(timespan) (TO_MILLISECONDS(timespan) * 1000)

using flutter::EncodableMap;
using flutter::EncodableValue;

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;

using winrt::Windows::Media::Core::MediaSource;

// Looks for |key| in |map|, returning the associated value if it is present, or
// a nullptr if not.
const EncodableValue* ValueOrNull(const EncodableMap& map, const char* key) {
  auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &(it->second);
}

// Converts a std::string to std::wstring
inline std::wstring TO_WIDESTRING(const std::string& string) {
  if (string.empty()) {
    return std::wstring();
  }
  int32_t target_length =
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
      static_cast<int32_t>(string.length()), nullptr, 0);
  if (target_length == 0) {
    return std::wstring();
  }
  std::wstring utf16_string;
  utf16_string.resize(target_length);
  int32_t converted_length =
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
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

  JustAudioEventSink(flutter::BinaryMessenger* messenger, const std::string& id) {
    auto event_channel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
        messenger, id, &flutter::StandardMethodCodec::GetInstance());

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

  void Success(const EncodableValue& event) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sink) {
      try {
        sink->Success(event);
      } catch (...) {}
    }
  }

  void Error(const std::string& error_code, const std::string& error_message) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    if (sink) {
      try {
        sink->Error(error_code, error_message);
      } catch (...) {}
    }
  }

private:
  std::mutex sink_mutex_;
  std::unique_ptr<flutter::EventSink<>> sink = nullptr;
};

class AudioPlayer {
private:
  // Read from WinRT callback threads, written from the platform thread.
  std::atomic<bool> disposed_{false};

  // Whether `load` has ever been handled for this player.
  // WinRT reports MediaPlaybackState::None while a source is being swapped in.
  // Mapping None to idle mid-load causes just_audio to abort loading in flight.
  bool source_set_ = false;

  bool buffering_progress_warned_ = false;

  // Marshals WinRT callbacks to the Flutter platform thread.
  std::shared_ptr<PlatformThreadDispatcher> dispatcher_;

  // Liveness token for tasks posted to the platform thread.
  std::shared_ptr<int> life_ = std::make_shared<int>(0);

  void OnPlatformThread(std::function<void()> task) {
    if (!dispatcher_ || !dispatcher_->available() ||
        dispatcher_->on_platform_thread()) {
      task();
      return;
    }
    std::weak_ptr<int> life = life_;
    dispatcher_->Post([life, task = std::move(task)]() {
      if (life.expired()) return;
      task();
    });
  }

  // Tokens for event unsubscription
  winrt::event_token playback_state_token_{};
  winrt::event_token media_failed_token_{};
  winrt::event_token item_changed_token_{};
  winrt::event_token item_failed_token_{};

public:
  std::string id;
  Playback::MediaPlayer mediaPlayer{};
  Playback::MediaPlaybackList mediaPlaybackList{};

  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> player_channel_;
  std::unique_ptr<JustAudioEventSink> event_sink_ = nullptr;
  std::unique_ptr<JustAudioEventSink> data_sink_ = nullptr;

  void Dispose() {
    bool expected = false;
    if (!disposed_.compare_exchange_strong(expected, true)) {
      return;
    }

    // Immediately invalidate pending platform thread callbacks
    life_.reset();

    try {
      auto session = mediaPlayer.PlaybackSession();
      if (playback_state_token_) {
        session.PlaybackStateChanged(playback_state_token_);
        playback_state_token_ = {};
      }
    } catch (...) {}

    try {
      if (media_failed_token_) {
        mediaPlayer.MediaFailed(media_failed_token_);
        media_failed_token_ = {};
      }
    } catch (...) {}

    try {
      if (item_changed_token_) {
        mediaPlaybackList.CurrentItemChanged(item_changed_token_);
        item_changed_token_ = {};
      }
    } catch (...) {}

    try {
      if (item_failed_token_) {
        mediaPlaybackList.ItemFailed(item_failed_token_);
        item_failed_token_ = {};
      }
    } catch (...) {}

    if (player_channel_) {
      player_channel_->SetMethodCallHandler(nullptr);
    }
    event_sink_.reset();
    data_sink_.reset();

    try {
      mediaPlayer.Close();
    } catch (...) {}
  }

  AudioPlayer(std::string idx, flutter::BinaryMessenger* messenger,
              std::shared_ptr<PlatformThreadDispatcher> dispatcher) {
    id = idx;
    dispatcher_ = std::move(dispatcher);

    // Opt out of the System Media Transport Controls (SMTC)
    mediaPlayer.CommandManager().IsEnabled(false);

    // Set up channels
    player_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        messenger, "com.ryanheise.just_audio.methods." + idx,
        &flutter::StandardMethodCodec::GetInstance()
      );

    player_channel_->SetMethodCallHandler(
      [player = this](const auto& call, auto result) {
        player->HandleMethodCall(call, std::move(result));
      });

    event_sink_ = std::make_unique<JustAudioEventSink>(messenger, "com.ryanheise.just_audio.events." + idx);
    data_sink_ = std::make_unique<JustAudioEventSink>(messenger, "com.ryanheise.just_audio.data." + idx);

    // Set up event callbacks
    playback_state_token_ = mediaPlayer.PlaybackSession().PlaybackStateChanged([this](auto, const auto&) -> void {
      if (disposed_) return;
      broadcastState();
    });

    media_failed_token_ = mediaPlayer.MediaFailed([this](auto, const Playback::MediaPlayerFailedEventArgs& args) -> void {
      if (disposed_) return;
      std::string errorMessage = winrt::to_string(args.ErrorMessage());

      std::cerr << "[just_audio_windows] Media error: " << errorMessage << std::endl;

      auto code = "unknown";
      switch (args.Error()) {
      case Playback::MediaPlayerError::Unknown:
        break;
      case Playback::MediaPlayerError::Aborted:
        code = "aborted";
        break;
      case Playback::MediaPlayerError::NetworkError:
        code = "networkError";
        break;
      case Playback::MediaPlayerError::DecodingError:
        code = "decodingError";
        break;
      case Playback::MediaPlayerError::SourceNotSupported:
        code = "sourceNotSupported";
        break;
      }

      OnPlatformThread([this, code, errorMessage] {
        if (disposed_) return;
        if (event_sink_) {
          event_sink_->Error(code, errorMessage);
        }
      });
    });

    mediaPlaybackList.MaxPlayedItemsToKeepOpen(2);
    item_changed_token_ = mediaPlaybackList.CurrentItemChanged([this](auto, const auto&) -> void {
      if (disposed_) return;
      broadcastState();
    });

    item_failed_token_ = mediaPlaybackList.ItemFailed([this](auto, const Playback::MediaPlaybackItemFailedEventArgs& args) -> void {
      if (disposed_) return;
      auto error = winrt::hresult_error(args.Error().ExtendedError());
      auto message = winrt::to_string(error.message());

      std::cerr << "[just_audio_windows] Item error: " << message << std::endl;

      auto code = "unknown";
      switch (args.Error().ErrorCode()) {
      case Playback::MediaPlaybackItemErrorCode::Aborted:
        code = "aborted";
        break;
      case Playback::MediaPlaybackItemErrorCode::NetworkError:
        code = "networkError";
        break;
      case Playback::MediaPlaybackItemErrorCode::DecodeError:
        code = "decodeError";
        break;
      case Playback::MediaPlaybackItemErrorCode::SourceNotSupportedError:
        code = "sourceNotSupportedError";
        break;
      case Playback::MediaPlaybackItemErrorCode::EncryptionError:
        code = "encryptionError";
        break;
      }

      OnPlatformThread([this, code, message] {
        if (disposed_) return;
        if (event_sink_) {
          event_sink_->Error(code, message);
        }
      });
    });
  }

  ~AudioPlayer() {
    Dispose();
  }

  bool HasPlayerId(const std::string& playerId) const {
    return id == playerId;
  }

  void HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result
  ) {
    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());

    JAW_TRACE("[just_audio_windows] Called " << method_call.method_name());

    if (method_call.method_name().compare("load") == 0) {
      const auto* audioSourceData = std::get_if<flutter::EncodableMap>(ValueOrNull(*args, "audioSource"));
      const auto* initialPosition = std::get_if<int>(ValueOrNull(*args, "initialPosition"));
      const auto* initialIndex = std::get_if<int>(ValueOrNull(*args, "initialIndex"));

      source_set_ = true;

      try {
        loadSource(*audioSourceData);

        if (initialIndex != nullptr) {
          seekToItem(static_cast<uint32_t>(*initialIndex));
        }

        if (initialPosition != nullptr) {
          seekToPosition(*initialPosition);
        }
      } catch (const winrt::hresult_error& error) {
        return result->Error("load_error", winrt::to_string(error.message()));
      } catch (const std::exception& error) {
        return result->Error("load_error", error.what());
      } catch (...) {
        return result->Error("load_error", "Unknown error loading the audio source");
      }

      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("play") == 0) {
      if (!disposed_) {
        try {
          mediaPlayer.Play();
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("pause") == 0) {
      if (!disposed_) {
        try {
          mediaPlayer.Pause();
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("stop") == 0) {
      if (!disposed_) {
        try {
          mediaPlayer.Pause();
          seekToPosition(0);
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setVolume") == 0) {
      const auto* volume = std::get_if<double>(ValueOrNull(*args, "volume"));
      if (!disposed_ && volume) {
        try {
          mediaPlayer.Volume(*volume);
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setSpeed") == 0) {
      const auto* speed = std::get_if<double>(ValueOrNull(*args, "speed"));
      if (!disposed_ && speed) {
        try {
          mediaPlayer.PlaybackSession().PlaybackRate(*speed);
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setPitch") == 0) {
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setSkipSilence") == 0) {
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setLoopMode") == 0) {
      const auto* loopMode = std::get_if<int>(ValueOrNull(*args, "loopMode"));
      if (!disposed_ && loopMode) {
        try {
          switch (*loopMode) {
          case 0: // off
            mediaPlayer.IsLoopingEnabled(false);
            mediaPlaybackList.AutoRepeatEnabled(false);
            break;
          case 1: // one
            mediaPlayer.IsLoopingEnabled(true);
            mediaPlaybackList.AutoRepeatEnabled(false);
            break;
          case 2: // all
            mediaPlayer.IsLoopingEnabled(false);
            mediaPlaybackList.AutoRepeatEnabled(true);
            break;
          }
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setShuffleMode") == 0) {
      const auto* shuffleMode = std::get_if<int>(ValueOrNull(*args, "shuffleMode"));
      if (!disposed_ && shuffleMode) {
        try {
          switch (*shuffleMode) {
          case 0: // none
            mediaPlaybackList.ShuffleEnabled(false);
            break;
          case 1: // all
            mediaPlaybackList.ShuffleEnabled(true);
            break;
          }
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setShuffleOrder") == 0) {
      const auto* source = std::get_if<flutter::EncodableMap>(ValueOrNull(*args, "audioSource"));
      if (!disposed_ && source) {
        try {
          setShuffleOrder(*source);
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("seek") == 0) {
      const auto* position = std::get_if<int>(ValueOrNull(*args, "position"));
      const auto* index = std::get_if<int>(ValueOrNull(*args, "index"));

      if (!disposed_) {
        try {
          if (index != nullptr) {
            seekToItem(static_cast<uint32_t>(*index));
          }
          if (position != nullptr) {
            seekToPosition(*position);
          }
        } catch (...) {}
      }

      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("concatenatingInsertAll") == 0) {
      const auto* index = std::get_if<int>(ValueOrNull(*args, "index"));
      const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(*args, "children"));

      if (!disposed_ && index && children) {
        try {
          auto items = mediaPlaybackList.Items();
          int currentIndex = *index;
          for (const auto& child : *children) {
            const auto* childMap = std::get_if<flutter::EncodableMap>(&child);
            if (childMap) {
              auto mediaSource = createMediaPlaybackItem(*childMap);
              auto item = Playback::MediaPlaybackItem(mediaSource);
              items.InsertAt(currentIndex, item);
              currentIndex++;
            }
          }
        } catch (const winrt::hresult_error& ex) {
          return result->Error("concatenatingInsertAll_error", winrt::to_string(ex.message()));
        }
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("concatenatingRemoveRange") == 0) {
      const auto* start = std::get_if<int>(ValueOrNull(*args, "startIndex"));
      const auto* end = std::get_if<int>(ValueOrNull(*args, "endIndex"));

      if (!disposed_ && start && end) {
        int startIndex = *start;
        int endIndex = *end;

        auto items = mediaPlaybackList.Items();
        int size = static_cast<int>(items.Size());

        if (endIndex > startIndex && startIndex >= 0 && endIndex <= size) {
          int count = endIndex - startIndex;
          try {
            for (int i = 0; i < count; i++) {
              items.RemoveAt(startIndex);
            }
          } catch (const winrt::hresult_error& ex) {
            return result->Error("concatenatingRemoveRange_error", winrt::to_string(ex.message()));
          }
          return result->Success(flutter::EncodableMap());
        } else {
          return result->Error("concatenatingRemoveRange_error", "invalid range");
        }
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("concatenatingMove") == 0) {
      const auto* from = std::get_if<int>(ValueOrNull(*args, "currentIndex"));
      const auto* to = std::get_if<int>(ValueOrNull(*args, "newIndex"));

      if (!disposed_ && from && to) {
        auto items = mediaPlaybackList.Items();
        int size = static_cast<int>(items.Size());

        int currentIndex = *from;
        int newIndex = *to;

        if (currentIndex >= size || newIndex > size) {
          return result->Error("concatenatingMove_error", "index out of bounds");
        }

        try {
          auto item = items.GetAt(currentIndex);
          items.RemoveAt(currentIndex);
          items.InsertAt(newIndex, item);
        } catch (const winrt::hresult_error& ex) {
          return result->Error("concatenatingMove_error", winrt::to_string(ex.message()));
        }
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setAndroidAudioAttributes") == 0 ||
               method_call.method_name().compare("audioEffectSetEnabled") == 0 ||
               method_call.method_name().compare("androidLoudnessEnhancerSetTargetGain") == 0 ||
               method_call.method_name().compare("androidEqualizerGetParameters") == 0 ||
               method_call.method_name().compare("androidEqualizerBandSetGain") == 0) {
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("dispose") == 0) {
      Dispose();
      result->Success(flutter::EncodableMap());
    } else {
      result->NotImplemented();
    }
  }

  void loadSource(const flutter::EncodableMap& source) const& {
    if (disposed_) return;
    auto items = mediaPlaybackList.Items();
    items.Clear();

    const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
    if (!type) return;

    if (type->compare("concatenating") == 0) {
      const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(source, "children"));
      if (children) {
        for (const auto& child : *children) {
          const auto* childMap = std::get_if<flutter::EncodableMap>(&child);
          if (childMap) {
            auto item = createMediaPlaybackItem(*childMap);
            items.Append(item);
          }
        }
      }
      mediaPlayer.Source(mediaPlaybackList.as<Playback::IMediaPlaybackSource>());
    } else {
      mediaPlayer.Source(createMediaPlaybackItem(source).as<Playback::IMediaPlaybackSource>());
    }
  }

  Playback::MediaPlaybackItem createMediaPlaybackItem(const flutter::EncodableMap& source) const& {
    const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
    if (!type) {
      throw std::invalid_argument("Source type is missing");
    }

    if (type->compare("clipping") == 0) {
      const auto* child = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "child"));
      if (!child) {
        throw std::invalid_argument("Clipping source child is missing");
      }
      auto childSource = createMediaSource(*child);

      const auto* startUs = std::get_if<int32_t>(ValueOrNull(*child, "start"));
      const auto* endUs = std::get_if<int32_t>(ValueOrNull(*child, "end"));

      int64_t start = (startUs != nullptr) ? *startUs : 0;

      if (endUs != nullptr) {
        int64_t duration = *endUs - start;
        return Playback::MediaPlaybackItem(
          childSource,
          TimeSpan(std::chrono::microseconds(start)),
          TimeSpan(std::chrono::microseconds(duration))
        );
      } else {
        return Playback::MediaPlaybackItem(
          childSource,
          TimeSpan(std::chrono::microseconds(start))
        );
      }
    } else {
      return Playback::MediaPlaybackItem(createMediaSource(source));
    }
  }

  MediaSource createMediaSource(const flutter::EncodableMap& source) const {
    const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
    if (!type) {
      throw std::invalid_argument("MediaSource type is missing");
    }
    if (type->compare("progressive") == 0 || type->compare("dash") == 0 || type->compare("hls") == 0) {
      const auto* uri = std::get_if<std::string>(ValueOrNull(source, "uri"));
      if (!uri) {
        throw std::invalid_argument("MediaSource uri is missing");
      }
      return MediaSource::CreateFromUri(
        Uri(TO_WIDESTRING(EncodeSpacesInUri(*uri)))
      );
    } else {
      throw std::invalid_argument("Source is unsupported or can not be nested: " + *type);
    }
  }

  void broadcastState() {
    try {
      broadcastPlaybackEvent();
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Broadcast event error: " << winrt::to_string(ex.message()) << std::endl;
    }

    try {
      broadcastDataEvent();
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Broadcast data error: " << winrt::to_string(ex.message()) << std::endl;
    }
  }

  void broadcastPlaybackEvent() {
    if (disposed_) return;
    auto session = mediaPlayer.PlaybackSession();

    auto eventData = flutter::EncodableMap();

    // NaturalDuration can throw when session is transitioning between items (PR #57)
    int64_t duration = 0;
    try {
      duration = TO_MICROSECONDS(session.NaturalDuration());
    } catch (...) {
      duration = 0;
    }

    auto now = std::chrono::system_clock::now();

    // Try to get BufferingProgress or default to 1.0 (PR #64)
    double bufferingProgress = 1.0;
    try {
      bufferingProgress = session.BufferingProgress();
    } catch (...) {
      if (!buffering_progress_warned_) {
        buffering_progress_warned_ = true;
        std::cerr << "[just_audio_windows]: Broadcast playback event error: Error accessing BufferingProgress. Using default value of 1." << std::endl;
      }
      bufferingProgress = 1.0;
    }

    // Position can throw when transitioning between items (PR #57)
    int64_t position = 0;
    try {
      position = TO_MICROSECONDS(session.Position());
    } catch (...) {
      position = 0;
    }

    eventData[flutter::EncodableValue("processingState")] = flutter::EncodableValue(processingState(session.PlaybackState()));
    eventData[flutter::EncodableValue("updatePosition")] = flutter::EncodableValue(position);
    eventData[flutter::EncodableValue("updateTime")] = flutter::EncodableValue(TO_MILLISECONDS(now.time_since_epoch()));
    eventData[flutter::EncodableValue("bufferedPosition")] = flutter::EncodableValue(static_cast<int64_t>(duration * bufferingProgress));
    eventData[flutter::EncodableValue("duration")] = flutter::EncodableValue(duration);

    if (mediaPlaybackList.Items().Size() > 0) {
      int64_t currentIndex = mediaPlaybackList.CurrentItemIndex();
      if (currentIndex != 4294967295) { // UINT32_MAX - 1
        eventData[flutter::EncodableValue("currentIndex")] = flutter::EncodableValue(currentIndex);
      }
    } else {
      eventData[flutter::EncodableValue("currentIndex")] = flutter::EncodableValue(0);
    }

    // Defer channel write onto Flutter platform thread (PR #63)
    OnPlatformThread([this, eventData = std::move(eventData)] {
      if (disposed_) return;
      if (event_sink_) {
        event_sink_->Success(eventData);
      }
    });
  }

  int processingState(Playback::MediaPlaybackState state) {
    if (disposed_) return 0;
    auto session = mediaPlayer.PlaybackSession();

    if (state == Playback::MediaPlaybackState::None) {
      // Once a source has been set, None is a gap between sources rather than idle (PR #65)
      return source_set_ ? 1 /*loading*/ : 0 /*idle*/;
    } else if (state == Playback::MediaPlaybackState::Opening) {
      return 1; //loading
    } else if (state == Playback::MediaPlaybackState::Buffering) {
      return 2; //buffering
    }

    // Guard duration check defensively against transitions (PR #57, PR #66)
    int64_t dur = 0;
    int64_t pos = 0;
    try {
      dur = session.NaturalDuration().count();
      pos = session.Position().count();
    } catch (...) {
      dur = 0;
      pos = 0;
    }

    if (dur > 0 && pos == dur) {
      return 4; //completed
    }
    return 3; //ready
  }

  void broadcastDataEvent() {
    if (disposed_) return;
    auto session = mediaPlayer.PlaybackSession();
    auto eventData = flutter::EncodableMap();

    auto isPlaying = session.PlaybackState() == Playback::MediaPlaybackState::Playing;

    eventData[flutter::EncodableValue("playing")] = flutter::EncodableValue(isPlaying);
    eventData[flutter::EncodableValue("volume")] = flutter::EncodableValue(mediaPlayer.Volume());
    eventData[flutter::EncodableValue("speed")] = flutter::EncodableValue(session.PlaybackRate());
    eventData[flutter::EncodableValue("loopMode")] = flutter::EncodableValue(getLoopMode());
    eventData[flutter::EncodableValue("shuffleMode")] = flutter::EncodableValue(getShuffleMode());

    // Defer channel write onto Flutter platform thread (PR #63)
    OnPlatformThread([this, eventData = std::move(eventData)] {
      if (disposed_) return;
      if (data_sink_) {
        data_sink_->Success(eventData);
      }
    });
  }

  int getLoopMode() {
    try {
      if (mediaPlayer.IsLoopingEnabled()) {
        return 1; // one
      } else if (mediaPlaybackList.AutoRepeatEnabled()) {
        return 2; // all
      } else {
        return 0; // off
      }
    } catch (...) {
      return 0;
    }
  }

  int getShuffleMode() {
    return 0;
  }

  void seekToItem(uint32_t index) {
    if (index >= mediaPlaybackList.Items().Size()) {
      return;
    }

    try {
      mediaPlaybackList.MoveTo(index);
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Failed to seek to item: " << winrt::to_string(ex.message()) << std::endl;
    }

    // Do NOT call broadcastState() here (PR #57). MoveTo() is asynchronous.
    // CurrentItemChanged will call broadcastState() when the item has settled.
  }

  void seekToPosition(int64_t microseconds) {
    if (disposed_) return;
    try {
      mediaPlayer.Position(TimeSpan(std::chrono::microseconds(microseconds)));
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Failed to seek to position: " << winrt::to_string(ex.message()) << std::endl;
    }

    broadcastState();
  }

  void setShuffleOrder(const flutter::EncodableMap& source) {
    const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
    if (!type) return;

    if (type->compare("concatenating") == 0) {
      const auto* shuffleOrder = std::get_if<flutter::EncodableList>(ValueOrNull(source, "shuffleOrder"));
      if (!shuffleOrder) return;

      std::vector<Playback::MediaPlaybackItem> itemsCopy {};
      for (auto item : mediaPlaybackList.Items()) {
        itemsCopy.push_back(item);
      }

      for (size_t i = 0; i < shuffleOrder->size() && i < itemsCopy.size(); i++) {
        auto item = itemsCopy.at(i);
        auto insertAt = (*shuffleOrder).at(i).LongValue();

        if (insertAt >= 0 && static_cast<size_t>(insertAt) < itemsCopy.size()) {
          itemsCopy.erase(itemsCopy.begin() + i);
          itemsCopy.insert(itemsCopy.begin() + insertAt, item);
        }
      }

      mediaPlaybackList.SetShuffledItems(itemsCopy);

      const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(source, "children"));
      if (children) {
        for (const auto& child : *children) {
          const auto* childMap = std::get_if<flutter::EncodableMap>(&child);
          if (childMap) {
            setShuffleOrder(*childMap);
          }
        }
      }
    } else if (type->compare("looping") == 0) {
      const auto* child = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "child"));
      if (child) {
        setShuffleOrder(*child);
      }
    }
  }
};
