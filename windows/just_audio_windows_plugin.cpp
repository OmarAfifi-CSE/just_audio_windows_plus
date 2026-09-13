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

std::vector<std::unique_ptr<AudioPlayer>> players_;
std::mutex players_mutex_;

// Marshals WinRT event callbacks onto Flutter's platform UI thread.
std::shared_ptr<PlatformThreadDispatcher> dispatcher_;

class JustAudioWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  JustAudioWindowsPlugin();

  virtual ~JustAudioWindowsPlugin();

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
      flutter::BinaryMessenger* messenger);

  void DisposePlayerByPlayerId(const std::string& id);
};

// static
void JustAudioWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "com.ryanheise.just_audio.methods",
          &flutter::StandardMethodCodec::GetInstance());

  dispatcher_ = std::make_shared<PlatformThreadDispatcher>();

  auto plugin = std::make_unique<JustAudioWindowsPlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get(), messenger_pointer = registrar->messenger()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result), messenger_pointer);
      });

  registrar->AddPlugin(std::move(plugin));
}

JustAudioWindowsPlugin::JustAudioWindowsPlugin() {}

JustAudioWindowsPlugin::~JustAudioWindowsPlugin() {
  std::vector<std::unique_ptr<AudioPlayer>> old_players;
  {
    std::lock_guard<std::mutex> lock(players_mutex_);
    old_players = std::move(players_);
  }
  // old_players destruct safely outside the lock while dispatcher_ is still active
  old_players.clear();
  dispatcher_.reset();
}

void JustAudioWindowsPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
    flutter::BinaryMessenger* messenger) {
  const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
  if (!args) {
    result->NotImplemented();
    return;
  }

  if (method_call.method_name().compare("init") == 0) {
    const auto* id = std::get_if<std::string>(ValueOrNull(*args, "id"));
    if (!id) {
      return result->Error("argument_error", "id argument missing");
    }
    // Clean up any pre-existing instance with the same ID to prevent orphaned channels or leaks.
    DisposePlayerByPlayerId(*id);

    try {
      auto player = std::make_unique<AudioPlayer>(*id, messenger, dispatcher_);
      {
        std::lock_guard<std::mutex> lock(players_mutex_);
        players_.push_back(std::move(player));
      }
      result->Success(flutter::EncodableMap());
    } catch (const winrt::hresult_error& error) {
      return result->Error("init_error", winrt::to_string(error.message()));
    } catch (const std::exception& error) {
      return result->Error("init_error", error.what());
    } catch (...) {
      return result->Error("init_error", "Unknown error initializing player");
    }
  } else if (method_call.method_name().compare("disposePlayer") == 0) {
    const auto* id = std::get_if<std::string>(ValueOrNull(*args, "id"));
    if (!id) {
      return result->Error("argument_error", "id argument missing");
    }
    DisposePlayerByPlayerId(*id);
    result->Success(flutter::EncodableMap());
  } else if (method_call.method_name().compare("disposeAllPlayers") == 0) {
    std::vector<std::unique_ptr<AudioPlayer>> old_players;
    {
      std::lock_guard<std::mutex> lock(players_mutex_);
      old_players = std::move(players_);
    }
    // old_players destruct safely outside the players_mutex_ lock
    result->Success(flutter::EncodableMap());
  } else {
    result->NotImplemented();
  }
}

void JustAudioWindowsPlugin::DisposePlayerByPlayerId(const std::string& id) {
  std::unique_ptr<AudioPlayer> player_to_dispose = nullptr;
  {
    std::lock_guard<std::mutex> lock(players_mutex_);
    for (auto it = begin(players_); it != end(players_); ++it) {
      if ((*it)->HasPlayerId(id)) {
        player_to_dispose = std::move(*it);
        players_.erase(it);
        break;
      }
    }
  }
  // player_to_dispose destructs safely outside the players_mutex_ lock
}

}  // namespace

void JustAudioWindowsPlusPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  JustAudioWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

void JustAudioWindowsPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  JustAudioWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
