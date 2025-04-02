// This must be included before many other Windows headers.
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <propkey.h>
#include <Functiondiscoverykeys_devpkey.h>

#include "twilio_voice_plugin.h"
#include "js_interop/call/tv_error.h"
#include "js_interop/call/tv_call_status.h"
#include "utils/tv_logger.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace twilio_voice {

void TwilioVoicePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto plugin = std::make_unique<TwilioVoicePlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

TwilioVoicePlugin::TwilioVoicePlugin(flutter::PluginRegistrarWindows* registrar) 
    : registrar_(registrar) {
  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "twilio_voice/messages",
      &flutter::StandardMethodCodec::GetInstance());

  // Initialize event channel
  event_channel_ = std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
      registrar->messenger(), "twilio_voice/events",
      &flutter::StandardMethodCodec::GetInstance());

  // Set up stream handler
  auto handler = std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
    [this](const flutter::EncodableValue* arguments,
           std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
      TV_LOG_DEBUG("Event stream handler: onListen called");
      event_sink_ = events.release();
      if (event_sink_) {
        TV_LOG_DEBUG("Event sink successfully set up");
      } else {
        TV_LOG_ERROR("Failed to set up event sink");
      }
      return nullptr;
    },
    [this](const flutter::EncodableValue* arguments) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
      TV_LOG_DEBUG("Event stream handler: onCancel called");
      if (event_sink_) {
        delete event_sink_;
        event_sink_ = nullptr;
        TV_LOG_DEBUG("Event sink successfully cleaned up");
      }
      return nullptr;
    }
  );
  stream_handler_ = std::move(handler);
  event_channel_->SetStreamHandler(std::move(stream_handler_));

  // Initialize logger with method channel
  TVLogger::getInstance().setMethodChannel(channel_.get());
  TV_LOG_INFO("TwilioVoicePlugin initialized");

  channel_->SetMethodCallHandler(
      [this](const auto &call, auto result) {
        HandleMethodCall(call, std::move(result));
      });

  InitializeWebView();
}

void TwilioVoicePlugin::InitializeWebView() {
  HWND hwnd = registrar_->GetView()->GetNativeWindow(); 
  webview_ = std::make_unique<TVWebView>(hwnd);
  
  webview_->initialize([this]() {
    TV_LOG_DEBUG("WebView initialized, getting module path");
    // Get the path to the assets directory
    wchar_t module_path[MAX_PATH];
    GetModuleFileNameW(NULL, module_path, MAX_PATH);
    std::wstring path(module_path);
    path = path.substr(0, path.find_last_of(L"\\/"));
    path = path.substr(0, path.find_last_of(L"\\/"));
    std::wstring assets_path = path + L"\\Debug\\assets";
    std::wstring html_path = assets_path + L"\\index.html";
    std::wstring js_path = assets_path + L"\\twilio.min.js";
    
    // Check if files exist
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    
    // Check for index.html
    find_handle = FindFirstFileW(html_path.c_str(), &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
      // Convert wide string to UTF-8 for logging
      int utf8Length = WideCharToMultiByte(CP_UTF8, 0, html_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (utf8Length > 0) {
        std::string utf8Path;
        utf8Path.resize(utf8Length - 1);
        WideCharToMultiByte(CP_UTF8, 0, html_path.c_str(), -1, &utf8Path[0], utf8Length, nullptr, nullptr);
        TV_LOG_ERROR("index.html not found at: " + utf8Path);
      }
      return;
    }
    FindClose(find_handle);
    
    // Check for twilio.min.js
    find_handle = FindFirstFileW(js_path.c_str(), &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
      // Convert wide string to UTF-8 for logging
      int utf8Length = WideCharToMultiByte(CP_UTF8, 0, js_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (utf8Length > 0) {
        std::string utf8Path;
        utf8Path.resize(utf8Length - 1);
        WideCharToMultiByte(CP_UTF8, 0, js_path.c_str(), -1, &utf8Path[0], utf8Length, nullptr, nullptr);
        TV_LOG_ERROR("twilio.min.js not found at: " + utf8Path);
      }
      return;
    }
    FindClose(find_handle);
    
    TV_LOG_DEBUG("Found required files in assets directory");
    
    // Convert wide string to UTF-8 for logging
    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, html_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length > 0) {
      std::string utf8Path;
      utf8Path.resize(utf8Length - 1);
      WideCharToMultiByte(CP_UTF8, 0, html_path.c_str(), -1, &utf8Path[0], utf8Length, nullptr, nullptr);
      TV_LOG_DEBUG("Loading HTML file from: " + utf8Path);
    }
    
    // Load the local HTML file and wait for navigation to complete
    webview_->loadFile(html_path, [this]() {
      TV_LOG_DEBUG("HTML file loaded, testing communication channel");
      
      // First, verify the file was loaded correctly
      webview_->evaluateJavaScript(
        L"(() => {"
        L"  try {"
        L"    if (typeof window.chrome === 'undefined' || typeof window.chrome.webview === 'undefined') {"
        L"      throw new Error('WebView bridge not available');"
        L"    }"
        L"    window.chrome.webview.postMessage(JSON.stringify({"
        L"      type: 'log',"
        L"      level: 'debug',"
        L"      message: 'WebView bridge is available'"
        L"    }));"
        L"    return true;"
        L"  } catch (error) {"
        L"    window.chrome.webview.postMessage(JSON.stringify({"
        L"      type: 'log',"
        L"      level: 'error',"
        L"      message: 'WebView bridge error: ' + error.message"
        L"    }));"
        L"    return false;"
        L"  }"
        L"})()",
        [](void*, std::string error) {
          if (!error.empty()) {
            TV_LOG_ERROR("WebView bridge test failed: " + error);
          } else {
            TV_LOG_DEBUG("WebView bridge test successful");
          }
        });

      // Then check if Twilio SDK is loaded
      webview_->evaluateJavaScript(
        L"(() => {"
        L"  try {"
        L"    if (typeof Twilio === 'undefined') {"
        L"      throw new Error('Twilio SDK not loaded');"
        L"    }"
        L"    window.chrome.webview.postMessage(JSON.stringify({"
        L"      type: 'log',"
        L"      level: 'debug',"
        L"      message: 'Twilio SDK loaded successfully'"
        L"    }));"
        L"    "
        L"    // Clean up any existing event listeners first"
        L"    if (window.device) {"
        L"      window.device.removeAllListeners('incoming');"
        L"      window.device.removeAllListeners('connect');"
        L"      window.device.removeAllListeners('disconnect');"
        L"      window.device.removeAllListeners('error');"
        L"      window.device.removeAllListeners('offline');"
        L"      window.device.removeAllListeners('ready');"
        L"    }"
        L"    "
        L"    // Set up event listeners for Twilio.Device"
        L"    window.device.on('incoming', (connection) => {"
        L"      // Handle both v1.x and v2.x SDK versions"
        L"      const from = connection.parameters ? connection.parameters.From : connection.From;"
        L"      const to = connection.parameters ? connection.parameters.To : connection.To;"
        L"      window.chrome.webview.postMessage(JSON.stringify({"
        L"        type: 'call_event',"
        L"        event: 'incoming',"
        L"        from: from,"
        L"        to: to"
        L"      }));"
        L"    });"
        L"    "
        L"    window.device.on('connect', (connection) => {"
        L"      // Handle both v1.x and v2.x SDK versions"
        L"      const from = connection.parameters ? connection.parameters.From : connection.From;"
        L"      const to = connection.parameters ? connection.parameters.To : connection.To;"
        L"      window.chrome.webview.postMessage(JSON.stringify({"
        L"        type: 'call_event',"
        L"        event: 'connected',"
        L"        from: from,"
        L"        to: to"
        L"      }));"
        L"    });"
        L"    "
        L"    window.device.on('disconnect', (connection) => {"
        L"      window.chrome.webview.postMessage(JSON.stringify({"
        L"        type: 'call_event',"
        L"        event: 'disconnected'"
        L"      }));"
        L"    });"
        L"    "
        L"    window.device.on('error', (error) => {"
        L"      window.chrome.webview.postMessage(JSON.stringify({"
        L"        type: 'call_event',"
        L"        event: 'error',"
        L"        error: error.message"
        L"      }));"
        L"    });"
        L"    "
        L"    window.device.on('offline', () => {"
        L"      window.chrome.webview.postMessage(JSON.stringify({"
        L"        type: 'call_event',"
        L"        event: 'offline'"
        L"      }));"
        L"    });"
        L"    "
        L"    window.device.on('ready', () => {"
        L"      window.chrome.webview.postMessage(JSON.stringify({"
        L"        type: 'call_event',"
        L"        event: 'ready'"
        L"      }));"
        L"    });"
        L"    "
        L"    return true;"
        L"  } catch (error) {"
        L"    window.chrome.webview.postMessage(JSON.stringify({"
        L"      type: 'log',"
        L"      level: 'error',"
        L"      message: 'Twilio SDK error: ' + error.message"
        L"    }));"
        L"    return false;"
        L"  }"
        L"})()",
        [](void*, std::string error) {
          if (!error.empty()) {
            TV_LOG_ERROR("Twilio SDK test failed: " + error);
          } else {
            TV_LOG_DEBUG("Twilio SDK test successful");
          }
        });

      // Add WebMessageReceived handler to capture JavaScript logs
      TV_LOG_DEBUG("Registering WebMessageReceived handler");
      HRESULT hr = webview_->getWebView()->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            TV_LOG_DEBUG("WebMessageReceived handler called");
            LPWSTR message;
            args->get_WebMessageAsJson(&message);
            if (message) {
              // Convert wide string to UTF-8
              int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
              if (utf8Length > 0) {
                std::string utf8Message;
                utf8Message.resize(utf8Length - 1);
                WideCharToMultiByte(CP_UTF8, 0, message, -1, &utf8Message[0], utf8Length, nullptr, nullptr);
                TV_LOG_DEBUG("Processing WebView message: " + utf8Message);
                
                try {
                  // Remove any BOM or invalid characters at the start
                  size_t jsonStart = utf8Message.find_first_of('{');
                  if (jsonStart != std::string::npos) {
                    utf8Message = utf8Message.substr(jsonStart);
                  }
                  
                  // Remove any trailing garbage
                  size_t jsonEnd = utf8Message.find_last_of('}');
                  if (jsonEnd != std::string::npos) {
                    utf8Message = utf8Message.substr(0, jsonEnd + 1);
                  }
                  
                  // Remove escaped quotes
                  std::string unescapedJson = utf8Message;
                  size_t pos = 0;
                  while ((pos = unescapedJson.find("\\\"", pos)) != std::string::npos) {
                    unescapedJson.replace(pos, 2, "\"");
                    pos += 1;
                  }
                  
                  TV_LOG_DEBUG("Unescaped JSON message: " + unescapedJson);
                  auto json = nlohmann::json::parse(unescapedJson);
                  TV_LOG_DEBUG("Successfully parsed JSON");
                  
                  if (json.contains("type")) {
                    std::string typeValue = json["type"].get<std::string>();
                    TV_LOG_DEBUG("Found 'type' field with value: " + typeValue);
                    
                    if (typeValue == "call_event" && json.contains("event")) {
                      std::string eventValue = json["event"].get<std::string>();
                      TV_LOG_DEBUG("Processing call event: " + eventValue);

                      // Handle special cases
                      if (eventValue == "incoming") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Incoming|" + from + "|" + to + "|Incoming");
                      } else if (eventValue == "connected") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Connected|" + from + "|" + to + "|Outgoing");
                      } else if (eventValue == "accept") {
                        std::string from = json.value("from", "");
                        std::string to = json.value("to", "");
                        SendEventToFlutter("Answer|" + from + "|" + to);
                      } else if (eventValue == "disconnected") {
                        SendEventToFlutter("Call Ended");
                      } else if (eventValue == "error") {
                        std::string error = json.value("error", "Unknown error");
                        SendEventToFlutter("Error|" + error);
                      } else {
                        // For all other events, just send the capitalized name
                        std::string eventName = eventValue;
                        if (!eventName.empty()) {
                          eventName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(eventName[0])));
                        }
                        SendEventToFlutter(eventName);
                      }
                    }
                  }
                } catch (const nlohmann::json::parse_error& e) {
                  TV_LOG_ERROR("JSON parse error: " + std::string(e.what()));
                } catch (const std::exception& e) {
                  TV_LOG_ERROR("Failed to process message: " + std::string(e.what()));
                }
              }
              CoTaskMemFree(message);
            }
            return S_OK;
          }).Get(),
        nullptr);
      
      if (FAILED(hr)) {
        TV_LOG_ERROR("Failed to register WebMessageReceived handler. HRESULT: " + std::to_string(hr));
      } else {
        TV_LOG_DEBUG("Successfully registered WebMessageReceived handler");
      }
    });
  });
}

TwilioVoicePlugin::~TwilioVoicePlugin() {}

bool TwilioVoicePlugin::CheckMicrophonePermission() {
  HRESULT hr = S_OK;
  IMMDeviceEnumerator* pEnumerator = nullptr;
  IMMDevice* pDevice = nullptr;
  IAudioClient* pAudioClient = nullptr;
  bool hasPermission = false;

  // Create device enumerator
  hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator),
      nullptr,
      CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator),
      (void**)&pEnumerator);

  if (SUCCEEDED(hr)) {
    // Get default input device
    hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
    if (SUCCEEDED(hr)) {
      // Try to activate audio client
      hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
      hasPermission = SUCCEEDED(hr);
    }
  }

  // Clean up
  if (pAudioClient) pAudioClient->Release();
  if (pDevice) pDevice->Release();
  if (pEnumerator) pEnumerator->Release();

  return hasPermission;
}

bool TwilioVoicePlugin::RequestMicrophonePermission() {
  // On Windows, permissions are typically handled through Windows Security settings
  // We can only check if we have permission
  return CheckMicrophonePermission();
}

void TwilioVoicePlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    
  const auto& method = method_call.method_name();
  TV_LOG_DEBUG("Handling method: " + method);
  
  if (method == "tokens") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected access token");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with access token");
      return;
    }

    auto token_it = args->find(flutter::EncodableValue("accessToken"));
    if (token_it == args->end()) {
      result->Error("Invalid Arguments", "Missing access token");
      return; 
    }

    const auto& token = std::get<std::string>(token_it->second);
    std::wstring wtoken(token.begin(), token.end());
    
    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));
        
    // First check if SDK is ready
    std::wstring check_sdk_script = L"(() => {"
      L"try {"
      L"  if (typeof Twilio === 'undefined') {"
      L"    throw new Error('Twilio SDK not loaded');"
      L"  }"
      L"  if (typeof Twilio.Device === 'undefined') {"
      L"    throw new Error('Twilio.Device not available');"
      L"  }"
      L"  window.chrome.webview.postMessage(JSON.stringify({"
      L"    type: 'log',"
      L"    level: 'debug',"
      L"    message: 'Twilio SDK is ready, proceeding with device setup'"
      L"  }));"
      L"  return true;"
      L"} catch (error) {"
      L"  window.chrome.webview.postMessage(JSON.stringify({"
      L"    type: 'log',"
      L"    level: 'error',"
      L"    message: 'Twilio SDK not ready: ' + error.message"
      L"  }));"
      L"  return false;"
      L"}"
      L"})()";

    webview_->evaluateJavaScript(
        check_sdk_script,
        [this, wtoken, shared_result](void*, std::string error) {
            if (!error.empty() || error == "false") {
                TV_LOG_ERROR("Twilio SDK not ready: " + error);
                (*shared_result)->Error("SDK_NOT_READY", "Twilio SDK is not ready yet");
                return;
            }

            // SDK is ready, proceed with device setup
            std::wstring setup_script = L"(() => {"
              L"try {"
              L"  window.chrome.webview.postMessage(JSON.stringify({"
              L"    type: 'log',"
              L"    level: 'debug',"
              L"    message: 'Setting up Twilio Device with token'"
              L"  }));"
              L"  if (typeof Twilio === 'undefined') {"
              L"    throw new Error('Twilio SDK not loaded');"
              L"  }"
              L"  if (typeof Twilio.Device === 'undefined') {"
              L"    throw new Error('Twilio.Device not available');"
              L"  }"
              L"  window.device = new Twilio.Device('" + wtoken + L"', {"
              L"    debug: true,"
              L"    closeProtection: false,"
              L"    allowIncomingConnections: true,"
              L"    codecPreferences: ['opus', 'pcmu']"
              L"  });"
              L"  if (!window.device) {"
              L"    throw new Error('Device setup failed - device is null');"
              L"  }"
              L"} catch (error) {"
              L"  window.chrome.webview.postMessage(JSON.stringify({"
              L"    type: 'log',"
              L"    level: 'error',"
              L"    message: 'Error setting up Twilio Device: ' + error.message"
              L"  }));"
              L"  throw error;"
              L"}"
              L"})()";

            webview_->evaluateJavaScript(
                setup_script,
                [shared_result](void*, std::string error) {
                    if (!error.empty()) {
                        TV_LOG_ERROR("Setup Failed: " + error);
                        (*shared_result)->Error("Setup Failed", error);
                    } else {
                        TV_LOG_INFO("Twilio Device setup successful");
                        (*shared_result)->Success(true);
                    }
                });
        });
  }
  else if (method == "makeCall") {
    if (!webview_) {
      TV_LOG_ERROR("WebView not initialized");
      result->Error("NOT_READY", "WebView not initialized");
      return;
    }

    const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!arguments) {
      TV_LOG_ERROR("Invalid arguments for makeCall");
      result->Error("INVALID_ARGUMENTS", "Invalid arguments for makeCall");
      return;
    }

    // Extract required parameters
    std::string from;
    std::string to;
    std::map<std::string, std::string> extraOptions;

    for (const auto& [key, value] : *arguments) {
      if (!std::holds_alternative<std::string>(key) || 
          !std::holds_alternative<std::string>(value)) {
        continue;
      }
      
      auto keyStr = std::get<std::string>(key);
      auto valueStr = std::get<std::string>(value);

      if (keyStr == "From") {
        from = valueStr;
      } else if (keyStr == "To") {
        to = valueStr;
      } else {
        extraOptions[keyStr] = valueStr;
      }
    }

    if (from.empty() || to.empty()) {
      TV_LOG_ERROR("Missing required From/To parameters");
      result->Error("INVALID_ARGUMENTS", "From and To parameters are required");
      return;
    }

    TV_LOG_INFO("Making call with From: " + from + ", To: " + to);

    // Convert parameters to wide strings for JavaScript
    std::wstring wfrom(from.begin(), from.end());
    std::wstring wto(to.begin(), to.end());

    // Build JavaScript code with extensive logging
    std::wstring js_code = L"(async () => {"
      L"try {"
      L"  window.chrome.webview.postMessage(JSON.stringify({"
      L"    type: 'call_event',"
      L"    event: 'ringing'"
      L"  }));"
      L"  if (typeof Twilio === 'undefined') {"
      L"    throw new Error('Twilio SDK not loaded - please wait for initialization');"
      L"  }"
      L"  if (!window.device) {"
      L"    throw new Error('Twilio Device not initialized - please call tokens() first');"
      L"  }"
      L"  const params = {"
      L"    To: '" + wto + L"',"
      L"    From: '" + wfrom + L"',"
      L"    codecPreferences: ['opus', 'pcmu']"
      L"  };"
      L"    window.chrome.webview.postMessage(JSON.stringify({"
      L"    type: 'call_event',"
      L"    event: 'ringing',"
      L"    params: params"
      L"    }));"
      L"  const connection = await window.device.connect(params);"
      L"  if (!connection) {"
      L"    throw new Error('Failed to create connection - connection is null');"
      L"  }"
      L"  connection.on('accept', () => {"
      L"    window.chrome.webview.postMessage(JSON.stringify({"
      L"      type: 'call_event',"
      L"      event: 'accept'"
      L"    }));"
      L"  });"
      L"  connection.on('disconnect', () => {"
      L"    window.chrome.webview.postMessage(JSON.stringify({"
      L"      type: 'call_event',"
      L"      event: 'disconnected'"
      L"    }));"
      L"  });"
      L"  connection.on('error', (error) => {"
      L"    window.chrome.webview.postMessage(JSON.stringify({"
      L"      type: 'call_event',"
      L"      event: 'error',"
      L"      error: error.message"
      L"    }));"
      L"  });"
      L"  connection.on('reject', () => {"
      L"    window.chrome.webview.postMessage(JSON.stringify({"
      L"      type: 'call_event',"
      L"      event: 'reject'"
      L"    }));"
      L"  });"
      L"  connection.on('cancel', () => {"
      L"    window.chrome.webview.postMessage(JSON.stringify({"
      L"      type: 'call_event',"
      L"      event: 'cancel'"
      L"    }));"
      L"  });"
      L"  return '';"
      L"} catch (error) {"
      L"  window.chrome.webview.postMessage(JSON.stringify({"
      L"    type: 'call_event',"
      L"    event: 'error',"
      L"    error: error.message"
      L"  }));"
      L"  throw error;"
      L"}"
      L"})()";

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    TV_LOG_DEBUG("Executing JavaScript for makeCall");
    webview_->evaluateJavaScript(
      js_code,
      [shared_result](void*, std::string error) {
        if (!error.empty()) {
          TV_LOG_ERROR("JavaScript error: " + error);
          (*shared_result)->Error("CALL_FAILED", error);
        } else {
          TV_LOG_INFO("Call initiated successfully");
          (*shared_result)->Success(true);
        }
      });
  }
  else if (method == "toggleMute") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected mute state");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with mute state");
      return;
    }

    auto muted_it = args->find(flutter::EncodableValue("muted"));
    if (muted_it == args->end()) {
      result->Error("Invalid Arguments", "Missing 'muted' parameter");
      return;
    }

    bool muted = std::get<bool>(muted_it->second);

    if (!activeCall_) {
      result->Error("No Active Call", "Cannot mute without an active call");
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring mute_script = std::wstring(L"Twilio.Device.activeConnection().mute(") + (muted ? L"true" : L"false") + L")";
    webview_->evaluateJavaScript(
        mute_script,
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Mute Failed", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "isMuted") {
    if (!activeCall_) {
      result->Success(false);
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    webview_->evaluateJavaScript(
        L"Twilio.Device.activeConnection().muted()",
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Failed to get mute state", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "toggleSpeaker") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected speaker state");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with speaker state");
      return;
    }

    auto speaker_it = args->find(flutter::EncodableValue("speakerIsOn"));
    if (speaker_it == args->end()) {
      result->Error("Invalid Arguments", "Missing 'speakerIsOn' parameter");
      return;
    }

    if (!activeCall_) {
      result->Error("No Active Call", "Cannot toggle speaker without an active call");
      return;
    }

    // On Windows, we'll use the default audio output device
    result->Success(true);
  }
  else if (method == "isOnSpeaker") {
    // On Windows, we'll always return false as we use the default audio output
    result->Success(false);
  }
  else if (method == "toggleBluetooth") {
    // Not supported on Windows
    result->Success(true);
  }
  else if (method == "isBluetoothOn") {
    // Not supported on Windows
    result->Success(false);
  }
  else if (method == "call-sid") {
    if (!activeCall_) {
      result->Success(nullptr);
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    webview_->evaluateJavaScript(
        L"Twilio.Device.activeConnection().parameters.CallSid",
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Failed to get call SID", error);
            } else {
                (*shared_result)->Success(error);
            }
        });
  }
  else if (method == "isOnCall") {
    result->Success(activeCall_ != nullptr);
  }
  else if (method == "sendDigits") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected digits");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with digits");
      return;
    }

    auto digits_it = args->find(flutter::EncodableValue("digits"));
    if (digits_it == args->end()) {
      result->Error("Invalid Arguments", "Missing 'digits' parameter");
      return;
    }

    const auto& digits = std::get<std::string>(digits_it->second);
    std::wstring wdigits(digits.begin(), digits.end());

    if (!activeCall_) {
      result->Error("No Active Call", "Cannot send digits without an active call");
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring digits_script = std::wstring(L"Twilio.Device.activeConnection().sendDigits('") + wdigits + L"')";
    webview_->evaluateJavaScript(
        digits_script,
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Failed to send digits", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "holdCall") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected hold state");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with hold state");
      return;
    }

    auto hold_it = args->find(flutter::EncodableValue("shouldHold"));
    if (hold_it == args->end()) {
      result->Error("Invalid Arguments", "Missing 'shouldHold' parameter");
      return;
    }

    bool shouldHold = std::get<bool>(hold_it->second);

    if (!activeCall_) {
      result->Error("No Active Call", "Cannot hold without an active call");
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring hold_script = std::wstring(L"Twilio.Device.activeConnection().hold(") + (shouldHold ? L"true" : L"false") + L")";
    webview_->evaluateJavaScript(
        hold_script,
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Hold Failed", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "isHolding") {
    if (!activeCall_) {
      result->Success(false);
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring is_holding_script = L"Twilio.Device.activeConnection().isHolding()";
    webview_->evaluateJavaScript(
        is_holding_script,
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Failed to get hold state", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "answer") {
    if (!activeCall_) {
      result->Error("No Active Call", "Cannot answer without an active call");
      return;
    }

    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring accept_script = L"Twilio.Device.activeConnection().accept()";
    webview_->evaluateJavaScript(
        accept_script,
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Answer Failed", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "unregister") {
    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring destroy_script = L"Twilio.Device.destroy()";
    webview_->evaluateJavaScript(
        destroy_script,
        [shared_result](void*, std::string error) {
            if (!error.empty()) {
                (*shared_result)->Error("Unregister Failed", error);
            } else {
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "hangUp") {
    TV_LOG_DEBUG("Executing hangUp command");
    
    auto shared_result = std::make_shared<std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>>(
        std::move(result));

    std::wstring disconnect_script = L"(() => { \n"
      L"  try { \n"
      L"    if (!window.device) { \n"
      L"      return 'No active device'; \n"
      L"    } \n"
      L"    \n"
      L"    // Disconnect active connection \n"
      L"    const activeConnection = window.device.activeConnection(); \n"
      L"    if (activeConnection) { \n"
      L"      activeConnection.disconnect(); \n"
      L"    } \n"
      L"    \n"
      L"    // Clean up event listeners \n"
      L"    window.device.removeAllListeners('incoming'); \n"
      L"    window.device.removeAllListeners('connect'); \n"
      L"    window.device.removeAllListeners('disconnect'); \n"
      L"    window.device.removeAllListeners('error'); \n"
      L"    window.device.removeAllListeners('offline'); \n"
      L"    window.device.removeAllListeners('ready'); \n"
      L"    window.device.removeAllListeners('reject'); \n"
      L"    window.device.removeAllListeners('cancel'); \n"
      L"    \n"
      L"    // Force audio to stop \n"
      L"    if (window.device.audio && window.device.audio.disconnect) { \n"
      L"      window.device.audio.disconnect(); \n"
      L"    } \n"
      L"    \n"
      L"    // Clean up any tracked audio resources \n"
      L"    if (typeof window.cleanupAudioResources === 'function') { \n"
      L"      window.cleanupAudioResources(); \n"
      L"    } \n"
      L"    \n"
      L"    return ''; \n"
      L"  } catch (error) { \n"
      L"    return error.message; \n"
      L"  } \n"
      L"})()";
    
    webview_->evaluateJavaScript(
        disconnect_script,
        [shared_result, this](void*, std::string error) {
            // Always reset the activeCall_ pointer
            activeCall_.reset();
            
            if (!error.empty()) {
                TV_LOG_ERROR("Hangup error: " + error);
                (*shared_result)->Error("HANGUP_FAILED", "Failed to hang up call: " + error);
            } else {
                TV_LOG_INFO("Call successfully disconnected");
                (*shared_result)->Success(true);
            }
        });
  }
  else if (method == "registerClient") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected client parameters");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with client parameters");
      return;
    }

    auto id_it = args->find(flutter::EncodableValue("id"));
    auto name_it = args->find(flutter::EncodableValue("name"));
    if (id_it == args->end() || name_it == args->end()) {
      result->Error("Invalid Arguments", "Missing required client parameters");
      return;
    }

    const auto& id = std::get<std::string>(id_it->second);
    const auto& name = std::get<std::string>(name_it->second);

    // Store client info in local storage
    std::wstring client_script = L"localStorage.setItem('client_" + std::wstring(id.begin(), id.end()) + 
                                L"', '" + std::wstring(name.begin(), name.end()) + L"')";
    
    webview_->evaluateJavaScript(client_script, [](void*, std::string) {});
    result->Success(true);
  }
  else if (method == "unregisterClient") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected client ID");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with client ID");
      return;
    }

    auto id_it = args->find(flutter::EncodableValue("id"));
    if (id_it == args->end()) {
      result->Error("Invalid Arguments", "Missing client ID");
      return;
    }

    const auto& id = std::get<std::string>(id_it->second);

    // Remove client info from local storage
    std::wstring unregister_script = L"localStorage.removeItem('client_" + std::wstring(id.begin(), id.end()) + L"')";
    
    webview_->evaluateJavaScript(unregister_script, [](void*, std::string) {});
    result->Success(true);
  }
  else if (method == "defaultCaller") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected default caller name");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with default caller name");
      return;
    }

    auto caller_it = args->find(flutter::EncodableValue("defaultCaller"));
    if (caller_it == args->end()) {
      result->Error("Invalid Arguments", "Missing default caller name");
      return;
    }

    const auto& caller = std::get<std::string>(caller_it->second);

    // Store default caller in local storage
    std::wstring default_caller_script = L"localStorage.setItem('defaultCaller', '" + std::wstring(caller.begin(), caller.end()) + L"')";
    
    webview_->evaluateJavaScript(default_caller_script, [](void*, std::string) {});
    result->Success(true);
  }
  else if (method == "hasMicPermission") {
    result->Success(CheckMicrophonePermission());
  }
  else if (method == "requestMicPermission") {
    result->Success(RequestMicrophonePermission());
  }
  else if (method == "hasBluetoothPermission") {
    // Not supported on Windows
    result->Success(true);
  }
  else if (method == "requestBluetoothPermission") {
    // Not supported on Windows
    result->Success(true);
  }
  else if (method == "requiresBackgroundPermissions") {
    // Not needed on Windows
    result->Success(false);
  }
  else if (method == "requestBackgroundPermissions") {
    // Not needed on Windows
    result->Success(true);
  }
  else if (method == "showNotifications") {
    if (!method_call.arguments()) {
      result->Error("Invalid Arguments", "Expected notification state");
      return;
    }

    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("Invalid Arguments", "Expected map with notification state");
      return;
    }

    auto show_it = args->find(flutter::EncodableValue("show"));
    if (show_it == args->end()) {
      result->Error("Invalid Arguments", "Missing 'show' parameter");
      return;
    }

    bool show = std::get<bool>(show_it->second);

    // Store notification preference in local storage
    std::wstring notifications_script = std::wstring(L"localStorage.setItem('showNotifications', '") + (show ? L"true" : L"false") + L"')";
    
    webview_->evaluateJavaScript(notifications_script, [](void*, std::string) {});
    result->Success(true);
  }
  else {
    result->NotImplemented();
  }
}

void TwilioVoicePlugin::SendEventToFlutter(const std::string& event) {
  if (!event_sink_) {
    TV_LOG_ERROR("Cannot send event to Flutter: event_sink_ is null, Event: " + event);
    return;
  }

  TV_LOG_DEBUG("Attempting to send event to Flutter: " + event);
  try {
    event_sink_->Success(flutter::EncodableValue(event));
    TV_LOG_DEBUG("Successfully sent event to Flutter: " + event);
  } catch (const std::exception& e) {
    TV_LOG_ERROR("Failed to send event to Flutter: " + std::string(e.what()) + ", Event: " + event);
  }
}

// TVCallDelegate implementations
void TwilioVoicePlugin::onCallAccept(TVCall* call) {
  SendEventToFlutter("Accept");
}

void TwilioVoicePlugin::onCallCancel(TVCall* call) {
  SendEventToFlutter("Cancel");
}

void TwilioVoicePlugin::onCallDisconnect(TVCall* call) {
  SendEventToFlutter("Disconnect");
}

void TwilioVoicePlugin::onCallError(const TVError& error) {
  json event;
  event["type"] = "error";
  event["code"] = error.code;
  event["message"] = error.message;
  SendEventToFlutter(event.dump());
}

void TwilioVoicePlugin::onCallReconnecting(const TVError& error) {
  json event;
  event["type"] = "reconnecting";
  event["error"] = error.message;
  SendEventToFlutter(event.dump());
}

void TwilioVoicePlugin::onCallReconnected() {
  SendEventToFlutter("Reconnected");
}

void TwilioVoicePlugin::onCallReject() {
  SendEventToFlutter("Reject");
}

void TwilioVoicePlugin::onCallStatus(const TVCallStatus& status) {
  json event;
  event["type"] = "status";
  event["status"] = status.status;
  event["callSid"] = status.callSid;
  SendEventToFlutter(event.dump());
}

}  // namespace twilio_voice
