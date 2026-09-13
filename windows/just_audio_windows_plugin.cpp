#pragma comment(lib, "windowsapp")

#include "include/just_audio_windows_plus/just_audio_windows_plus_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <map>
#include <memory>
#include <sstream>

#include "player.hpp"

using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {

// static std::unordered_map<std::string, AudioPlayer> players;
std::vector<std::unique_ptr<AudioPlayer>> players_;
std::mutex players_mutex_;

class JustAudioWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  JustAudioWindowsPlugin();

  virtual ~JustAudioWindowsPlugin();

 private:
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void DisposePlayerByPlayerId(const std::string& id);
};

// static
void JustAudioWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "com.ryanheise.just_audio.methods",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<JustAudioWindowsPlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

JustAudioWindowsPlugin::JustAudioWindowsPlugin() {}

JustAudioWindowsPlugin::~JustAudioWindowsPlugin() {}

void JustAudioWindowsPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("init") == 0) {
    auto data = std::get<EncodableMap>(*method_call.arguments());
    auto id = std::get<std::string>(*ValueOrNull(data, "id"));
    auto player = std::make_unique<AudioPlayer>(
      flutter::PluginRegistrarManager::GetInstance()
        ->GetRegistrar<flutter::PluginRegistrarWindows>(this)
        ->messenger(),
      id
    );
    {
      std::lock_guard<std::mutex> lock(players_mutex_);
      players_.push_back(std::move(player));
    }
    result->Success(EncodableMap());
  } else if (method_call.method_name().compare("disposePlayer") == 0) {
    auto data = std::get<EncodableMap>(*method_call.arguments());
    auto id = std::get<std::string>(*ValueOrNull(data, "id"));
    DisposePlayerByPlayerId(id);
    result->Success(EncodableMap());
  } else if (method_call.method_name().compare("disposeAllPlayers") == 0) {
    std::vector<std::unique_ptr<AudioPlayer>> old_players;
    {
      std::lock_guard<std::mutex> lock(players_mutex_);
      old_players = std::move(players_);
    }
    // old_players destruct safely outside the players_mutex_ lock
    result->Success(EncodableMap());
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
