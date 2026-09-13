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

inline int64_t TimeSpanToMicroseconds(TimeSpan timespan) {
  return timespan.count() / 10;
}

inline int64_t TimeSpanToMilliseconds(TimeSpan timespan) {
  return timespan.count() / 10000;
}

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
  flutter::BinaryMessenger* messenger_ = nullptr;
  std::string id_;
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
  winrt::event_token natural_duration_token_{};
  winrt::event_token media_failed_token_{};
  winrt::event_token media_ended_token_{};
  winrt::event_token item_changed_token_{};
  winrt::event_token item_failed_token_{};

  // Mutex to serialize broadcastState across platform and WinRT background worker threads
  std::mutex broadcast_mutex_;

  int loop_mode_ = 0;
  int shuffle_mode_ = 0;

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

    std::lock_guard<std::mutex> lock(broadcast_mutex_);

    try {
      auto session = mediaPlayer.PlaybackSession();
      if (playback_state_token_) {
        session.PlaybackStateChanged(playback_state_token_);
        playback_state_token_ = {};
      }
      if (natural_duration_token_) {
        session.NaturalDurationChanged(natural_duration_token_);
        natural_duration_token_ = {};
      }
    } catch (...) {}

    try {
      if (media_failed_token_) {
        mediaPlayer.MediaFailed(media_failed_token_);
        media_failed_token_ = {};
      }
    } catch (...) {}

    try {
      if (media_ended_token_) {
        mediaPlayer.MediaEnded(media_ended_token_);
        media_ended_token_ = {};
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
      try {
        broadcastState();
      } catch (...) {}
    });

    natural_duration_token_ = mediaPlayer.PlaybackSession().NaturalDurationChanged([this](auto, const auto&) -> void {
      if (disposed_) return;
      try {
        broadcastState();
      } catch (...) {}
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

    media_ended_token_ = mediaPlayer.MediaEnded([this](auto, const auto&) -> void {
      if (disposed_) return;
      try {
        broadcastState();
      } catch (...) {}
    });

    mediaPlaybackList.MaxPlayedItemsToKeepOpen(2);
    item_changed_token_ = mediaPlaybackList.CurrentItemChanged([this](auto, const auto&) -> void {
      if (disposed_) return;
      try {
        broadcastState();
      } catch (...) {}
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
      if (!audioSourceData) {
        return result->Error("load_error", "audioSource argument is missing or invalid");
      }
      int64_t initialPos = 0;
      bool hasInitialPos = TryGetInt64(ValueOrNull(*args, "initialPosition"), initialPos);
      int64_t initialIdx = 0;
      bool hasInitialIdx = TryGetInt64(ValueOrNull(*args, "initialIndex"), initialIdx);

      source_set_ = true;

      try {
        loadSource(*audioSourceData);

        if (hasInitialIdx && initialIdx >= 0) {
          seekToItem(static_cast<uint32_t>(initialIdx));
        }

        if (hasInitialPos && (!hasInitialIdx || initialPos > 0)) {
          seekToPosition(initialPos);
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
      int64_t loopMode = 0;
      if (!disposed_ && TryGetInt64(ValueOrNull(*args, "loopMode"), loopMode)) {
        loop_mode_ = static_cast<int>(loopMode);
        try {
          bool hasList = false;
          try {
            if (mediaPlaybackList) {
              auto items = mediaPlaybackList.Items();
              hasList = items && items.Size() > 0;
            }
          } catch (...) {}

          switch (loopMode) {
          case 0: // off
            mediaPlayer.IsLoopingEnabled(false);
            if (mediaPlaybackList) mediaPlaybackList.AutoRepeatEnabled(false);
            break;
          case 1: // one
            mediaPlayer.IsLoopingEnabled(true);
            if (mediaPlaybackList) mediaPlaybackList.AutoRepeatEnabled(false);
            break;
          case 2: // all
            if (hasList) {
              mediaPlayer.IsLoopingEnabled(false);
              mediaPlaybackList.AutoRepeatEnabled(true);
            } else {
              mediaPlayer.IsLoopingEnabled(true);
              if (mediaPlaybackList) mediaPlaybackList.AutoRepeatEnabled(false);
            }
            break;
          }
        } catch (...) {}
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("setShuffleMode") == 0) {
      int64_t shuffleMode = 0;
      if (!disposed_ && TryGetInt64(ValueOrNull(*args, "shuffleMode"), shuffleMode)) {
        shuffle_mode_ = static_cast<int>(shuffleMode);
        try {
          if (mediaPlaybackList) {
            mediaPlaybackList.ShuffleEnabled(shuffleMode == 1);
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
      int64_t pos = 0;
      bool hasPos = TryGetInt64(ValueOrNull(*args, "position"), pos);
      int64_t idx = 0;
      bool hasIdx = TryGetInt64(ValueOrNull(*args, "index"), idx);

      if (!disposed_) {
        try {
          if (hasIdx && idx >= 0) {
            seekToItem(static_cast<uint32_t>(idx));
          }
          if (hasPos) {
            if (!hasIdx || pos > 0) {
              seekToPosition(pos);
            }
          }
        } catch (...) {}
      }

      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("concatenatingInsertAll") == 0) {
      int64_t idx = 0;
      bool hasIdx = TryGetInt64(ValueOrNull(*args, "index"), idx);
      const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(*args, "children"));

      if (!disposed_ && hasIdx && children) {
        try {
          auto items = mediaPlaybackList.Items();
          int size = static_cast<int>(items.Size());
          int currentIndex = static_cast<int>(idx);

          if (currentIndex < 0 || currentIndex > size) {
            return result->Error("concatenatingInsertAll_error", "index out of bounds");
          }

          for (const auto& child : *children) {
            const auto* childMap = std::get_if<flutter::EncodableMap>(&child);
            if (childMap) {
              auto item = createMediaPlaybackItem(*childMap);
              items.InsertAt(currentIndex, item);
              currentIndex++;
            }
          }
        } catch (const winrt::hresult_error& ex) {
          return result->Error("concatenatingInsertAll_error", winrt::to_string(ex.message()));
        } catch (const std::exception& ex) {
          return result->Error("concatenatingInsertAll_error", ex.what());
        } catch (...) {
          return result->Error("concatenatingInsertAll_error", "Unknown error inserting items");
        }
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("concatenatingRemoveRange") == 0) {
      int64_t start = 0;
      bool hasStart = TryGetInt64(ValueOrNull(*args, "startIndex"), start);
      int64_t end = 0;
      bool hasEnd = TryGetInt64(ValueOrNull(*args, "endIndex"), end);

      if (!disposed_ && hasStart && hasEnd) {
        int startIndex = static_cast<int>(start);
        int endIndex = static_cast<int>(end);

        try {
          auto items = mediaPlaybackList.Items();
          int size = static_cast<int>(items.Size());

          if (endIndex > startIndex && startIndex >= 0 && endIndex <= size) {
            int count = endIndex - startIndex;
            for (int i = 0; i < count; i++) {
              items.RemoveAt(startIndex);
            }
            return result->Success(flutter::EncodableMap());
          } else {
            return result->Error("concatenatingRemoveRange_error", "invalid range");
          }
        } catch (const winrt::hresult_error& ex) {
          return result->Error("concatenatingRemoveRange_error", winrt::to_string(ex.message()));
        } catch (const std::exception& ex) {
          return result->Error("concatenatingRemoveRange_error", ex.what());
        } catch (...) {
          return result->Error("concatenatingRemoveRange_error", "Unknown error removing items");
        }
      }
      result->Success(flutter::EncodableMap());
    } else if (method_call.method_name().compare("concatenatingMove") == 0) {
      int64_t from = 0;
      bool hasFrom = TryGetInt64(ValueOrNull(*args, "currentIndex"), from);
      int64_t to = 0;
      bool hasTo = TryGetInt64(ValueOrNull(*args, "newIndex"), to);

      if (!disposed_ && hasFrom && hasTo) {
        try {
          auto items = mediaPlaybackList.Items();
          int size = static_cast<int>(items.Size());

          int currentIndex = static_cast<int>(from);
          int newIndex = static_cast<int>(to);

          if (currentIndex < 0 || currentIndex >= size || newIndex < 0 || newIndex >= size) {
            return result->Error("concatenatingMove_error", "index out of bounds");
          }

          if (currentIndex != newIndex) {
            auto item = items.GetAt(currentIndex);
            items.RemoveAt(currentIndex);
            items.InsertAt(newIndex, item);
          }
        } catch (const winrt::hresult_error& ex) {
          return result->Error("concatenatingMove_error", winrt::to_string(ex.message()));
        } catch (const std::exception& ex) {
          return result->Error("concatenatingMove_error", ex.what());
        } catch (...) {
          return result->Error("concatenatingMove_error", "Unknown error moving item");
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

      int64_t start = 0;
      if (!TryGetInt64(ValueOrNull(source, "start"), start)) {
        TryGetInt64(ValueOrNull(*child, "start"), start);
      }

      int64_t end = 0;
      bool hasEnd = TryGetInt64(ValueOrNull(source, "end"), end);
      if (!hasEnd) {
        hasEnd = TryGetInt64(ValueOrNull(*child, "end"), end);
      }

      if (hasEnd && end > start) {
        int64_t duration = end - start;
        return Playback::MediaPlaybackItem(
          childSource,
          TimeSpan(std::chrono::microseconds(std::max<int64_t>(0, start))),
          TimeSpan(std::chrono::microseconds(duration))
        );
      } else {
        return Playback::MediaPlaybackItem(
          childSource,
          TimeSpan(std::chrono::microseconds(std::max<int64_t>(0, start)))
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
    if (disposed_) return;
    std::lock_guard<std::mutex> lock(broadcast_mutex_);
    if (disposed_) return;

    try {
      broadcastPlaybackEvent();
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Broadcast event error: " << winrt::to_string(ex.message()) << std::endl;
    } catch (const std::exception& ex) {
      std::cerr << "[just_audio_windows] Broadcast event std error: " << ex.what() << std::endl;
    } catch (...) {
      std::cerr << "[just_audio_windows] Broadcast event unknown error" << std::endl;
    }

    try {
      broadcastDataEvent();
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Broadcast data error: " << winrt::to_string(ex.message()) << std::endl;
    } catch (const std::exception& ex) {
      std::cerr << "[just_audio_windows] Broadcast data std error: " << ex.what() << std::endl;
    } catch (...) {
      std::cerr << "[just_audio_windows] Broadcast data unknown error" << std::endl;
    }
  }

  void broadcastPlaybackEvent() {
    if (disposed_) return;
    auto session = mediaPlayer.PlaybackSession();
    if (!session) return;

    auto eventData = flutter::EncodableMap();

    // NaturalDuration can throw when session is transitioning between items (PR #57)
    int64_t duration = 0;
    try {
      duration = TimeSpanToMicroseconds(session.NaturalDuration());
    } catch (...) {
      duration = 0;
    }

    auto now = std::chrono::system_clock::now();

    // Try to get BufferingProgress or default to 1.0 (PR #64)
    double bufferingProgress = 1.0;
    try {
      bufferingProgress = session.BufferingProgress();
    } catch (...) {
      JAW_TRACE("[just_audio_windows]: BufferingProgress not available for current source. Using default value of 1.0.");
      bufferingProgress = 1.0;
    }

    // Position can throw when transitioning between items (PR #57)
    int64_t position = 0;
    try {
      position = TimeSpanToMicroseconds(session.Position());
    } catch (...) {
      position = 0;
    }

    eventData[flutter::EncodableValue("processingState")] = flutter::EncodableValue(processingState(session.PlaybackState()));
    eventData[flutter::EncodableValue("updatePosition")] = flutter::EncodableValue(position);
    eventData[flutter::EncodableValue("updateTime")] = flutter::EncodableValue(
        static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
    eventData[flutter::EncodableValue("bufferedPosition")] = flutter::EncodableValue(static_cast<int64_t>(duration * bufferingProgress));
    eventData[flutter::EncodableValue("duration")] = flutter::EncodableValue(duration);

    int64_t currentIndex = 0;
    try {
      if (mediaPlaybackList) {
        auto items = mediaPlaybackList.Items();
        if (items && items.Size() > 0) {
          uint32_t idx = mediaPlaybackList.CurrentItemIndex();
          if (idx != 4294967295) { // UINT32_MAX - 1
            currentIndex = idx;
          }
        }
      }
    } catch (...) {
      currentIndex = 0;
    }
    eventData[flutter::EncodableValue("currentIndex")] = flutter::EncodableValue(currentIndex);

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

    if (dur > 0 && pos >= dur) {
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

  int getLoopMode() const {
    return loop_mode_;
  }

  int getShuffleMode() const {
    return shuffle_mode_;
  }

  void seekToItem(uint32_t index) {
    if (disposed_) return;

    try {
      if (mediaPlaybackList) {
        auto items = mediaPlaybackList.Items();
        if (items && index < items.Size()) {
          mediaPlaybackList.MoveTo(index);
        }
      }
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Failed to seek to item: " << winrt::to_string(ex.message()) << std::endl;
    } catch (const std::exception& ex) {
      std::cerr << "[just_audio_windows] Failed to seek to item (std): " << ex.what() << std::endl;
    } catch (...) {}

    // Do NOT call broadcastState() here (PR #57). MoveTo() is asynchronous.
    // CurrentItemChanged will call broadcastState() when the item has settled.
  }

  void seekToPosition(int64_t microseconds) {
    if (disposed_) return;
    try {
      mediaPlayer.Position(TimeSpan(std::chrono::microseconds(microseconds)));
    } catch (winrt::hresult_error const& ex) {
      std::cerr << "[just_audio_windows] Failed to seek to position: " << winrt::to_string(ex.message()) << std::endl;
    } catch (const std::exception& ex) {
      std::cerr << "[just_audio_windows] Failed to seek to position (std): " << ex.what() << std::endl;
    } catch (...) {}

    broadcastState();
  }

  void setShuffleOrder(const flutter::EncodableMap& source) {
    if (disposed_) return;
    try {
      const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
      if (!type) return;

      if (type->compare("concatenating") == 0) {
        const auto* shuffleOrder = std::get_if<flutter::EncodableList>(ValueOrNull(source, "shuffleOrder"));
        if (!shuffleOrder) return;

        if (!mediaPlaybackList) return;

        std::vector<Playback::MediaPlaybackItem> itemsCopy {};
        for (auto item : mediaPlaybackList.Items()) {
          itemsCopy.push_back(item);
        }

        for (size_t i = 0; i < shuffleOrder->size() && i < itemsCopy.size(); i++) {
          auto item = itemsCopy.at(i);
          int64_t insertAt = 0;
          if (TryGetInt64(&((*shuffleOrder).at(i)), insertAt)) {
            if (insertAt >= 0 && static_cast<size_t>(insertAt) < itemsCopy.size()) {
              itemsCopy.erase(itemsCopy.begin() + i);
              itemsCopy.insert(itemsCopy.begin() + insertAt, item);
            }
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
    } catch (...) {}
  }
};
