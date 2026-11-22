#pragma comment(lib, "windowsapp")

#include "include/just_audio_windows_plus/just_audio_windows_plus_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include "platform_thread.hpp"
#include "player.hpp"

using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {

class MessengerHandle {
 public:
  explicit MessengerHandle(FlutterDesktopMessengerRef messenger)
      : messenger_(messenger ? FlutterDesktopMessengerAddRef(messenger)
                             : nullptr) {}
  ~MessengerHandle() {
    if (messenger_) {
      FlutterDesktopMessengerRelease(messenger_);
    }
  }
  MessengerHandle(const MessengerHandle&) = delete;
  MessengerHandle& operator=(const MessengerHandle&) = delete;

  // All callers run on Flutter's platform thread (same thread as engine
  // teardown), so holding FlutterDesktopMessengerLock across SetMessageHandler
  // or Send is unnecessary and would risk lock inversion with the wrapper.
  bool IsAvailable() const {
    if (!messenger_) return true;
    FlutterDesktopMessengerLock(messenger_);
    const bool available = FlutterDesktopMessengerIsAvailable(messenger_);
    FlutterDesktopMessengerUnlock(messenger_);
    return available;
  }

 private:
  FlutterDesktopMessengerRef messenger_ = nullptr;
};

class JustAudioWindowsPlusPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(
      flutter::PluginRegistrarWindows *registrar,
      FlutterDesktopMessengerRef raw_messenger = nullptr);

  explicit JustAudioWindowsPlusPlugin(
      std::function<bool()> is_messenger_available = {});

  virtual ~JustAudioWindowsPlusPlugin();

 private:
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::vector<std::shared_ptr<AudioPlayer>> players_;
  std::shared_ptr<PlatformThreadDispatcher> dispatcher_;
  std::function<bool()> is_messenger_available_;
  std::unique_ptr<flutter::MethodChannel<EncodableValue>> channel_;
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
      flutter::BinaryMessenger* messenger);

  void DisposePlayerByPlayerId(const std::string& id);
};

// static
void JustAudioWindowsPlusPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar,
    FlutterDesktopMessengerRef raw_messenger) {
  auto messenger_handle = std::make_shared<MessengerHandle>(raw_messenger);
  auto is_messenger_available = [messenger_handle]() {
    return messenger_handle->IsAvailable();
  };

  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "com.ryanheise.just_audio.methods",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<JustAudioWindowsPlusPlugin>(is_messenger_available);

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get(), messenger_pointer = registrar->messenger()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result), messenger_pointer);
      });

  plugin->channel_ = std::move(channel);
  registrar->AddPlugin(std::move(plugin));
}

JustAudioWindowsPlusPlugin::JustAudioWindowsPlusPlugin(
    std::function<bool()> is_messenger_available)
    : dispatcher_(std::make_shared<PlatformThreadDispatcher>()),
      is_messenger_available_(std::move(is_messenger_available)) {}

JustAudioWindowsPlusPlugin::~JustAudioWindowsPlusPlugin() {
  alive_.reset();
  const bool messenger_ok =
      !is_messenger_available_ || is_messenger_available_();
  if (channel_ && messenger_ok) channel_->SetMethodCallHandler(nullptr);
  for (auto& player : players_) player->Dispose();
  players_.clear();
  dispatcher_->Shutdown();
  dispatcher_.reset();
}

void JustAudioWindowsPlusPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
    flutter::BinaryMessenger* messenger) {
  const auto* args = method_call.arguments() ? std::get_if<flutter::EncodableMap>(method_call.arguments()) : nullptr;
  if (!args) {
    result->Error("argument_error", "arguments must be a map");
    return;
  }

  if (method_call.method_name().compare("init") == 0) {
    const auto* id = jaw::Get<std::string>(*args, "id");
    if (!id) {
      return result->Error("argument_error", "id argument missing");
    }
    // Clean up any pre-existing instance with the same ID to prevent orphaned channels or leaks.
    DisposePlayerByPlayerId(*id);

    try {
      std::weak_ptr<bool> plugin_alive = alive_;
      const std::string player_id = *id;
      auto player = std::make_shared<AudioPlayer>(
          player_id, messenger, dispatcher_, is_messenger_available_,
          [this, plugin_alive, player_id]() {
            if (plugin_alive.expired()) return;
            DisposePlayerByPlayerId(player_id);
          });
      player->Initialize();
      players_.push_back(std::move(player));
      result->Success(flutter::EncodableMap());
    } catch (const winrt::hresult_error& error) {
      return result->Error(
          "init_error",
          FormatWinRtPlaybackError("", error.code().value,
                                   winrt::to_string(error.message())));
    } catch (const std::exception& error) {
      return result->Error("init_error", error.what());
    } catch (...) {
      return result->Error("init_error", "Unknown error initializing player");
    }
  } else if (method_call.method_name().compare("disposePlayer") == 0) {
    const auto* id = jaw::Get<std::string>(*args, "id");
    if (!id) {
      return result->Error("argument_error", "id argument missing");
    }
    DisposePlayerByPlayerId(*id);
    result->Success(flutter::EncodableMap());
  } else if (method_call.method_name().compare("disposeAllPlayers") == 0) {
    for (auto& player : players_) player->Dispose();
    players_.clear();
    result->Success(flutter::EncodableMap());
  } else {
    result->NotImplemented();
  }
}

void JustAudioWindowsPlusPlugin::DisposePlayerByPlayerId(const std::string& id) {
  for (auto it = players_.begin(); it != players_.end(); ++it) {
    if ((*it)->HasPlayerId(id)) {
      (*it)->Dispose();
      players_.erase(it);
      return;
    }
  }
}

}  // namespace

void JustAudioWindowsPlusPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  JustAudioWindowsPlusPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar),
      FlutterDesktopPluginRegistrarGetMessenger(registrar));
}
