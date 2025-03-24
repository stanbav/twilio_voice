#include "twilio_voice_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>
#include <wrl.h>
#include <wrl/client.h>  // Add this for ComPtr
#include <winrt/Windows.Foundation.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/event_channel.h>  // Add this for EventChannel

#include <memory>
#include <sstream>

namespace twilio_voice
{
  // Add helper method near the top of the namespace
  void LogError(const std::wstring& message) {
    OutputDebugStringW(message.c_str());
    std::wcerr << L"Twilio Voice Error: " << message << std::endl;
  }

  void LogMessage(const std::wstring& message) {
    OutputDebugStringW(message.c_str());
    std::wcout << L"Twilio Voice: " << message << std::endl;
  }

  TwilioEventHandler::TwilioEventHandler(TwilioVoicePlugin* plugin) : plugin_(plugin) {}

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  TwilioEventHandler::OnListenInternal(const flutter::EncodableValue* arguments,
                                     std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events) {
    plugin_->SetEventSink(std::move(events));
    return nullptr;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  TwilioEventHandler::OnCancelInternal(const flutter::EncodableValue* arguments) {
    plugin_->ClearEventSink();
    return nullptr;
  }

  void TwilioVoicePlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarWindows *registrar)
  {
    auto method_channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            registrar->messenger(), "twilio_voice/messages",
            &flutter::StandardMethodCodec::GetInstance());

    auto event_channel =
        std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
            registrar->messenger(), "twilio_voice/events",
            &flutter::StandardMethodCodec::GetInstance());

    auto plugin = std::make_unique<TwilioVoicePlugin>(registrar);  // Updated constructor call

    method_channel->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto &call, auto result)
        {
          plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    event_channel->SetStreamHandler(std::make_unique<TwilioEventHandler>(plugin.get()));

    registrar->AddPlugin(std::move(plugin));
  }

  TwilioVoicePlugin::TwilioVoicePlugin(flutter::PluginRegistrarWindows *registrar) : registrar_(registrar) {
    hwnd_ = CreateWindowHandle();
    if (hwnd_) {
        InitializeWebView();
    }
  }

  TwilioVoicePlugin::~TwilioVoicePlugin() {}

  HRESULT TwilioVoicePlugin::InitializeWebView()
  {
    LogError(L"Starting WebView initialization...");
    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT 
            {
                if (SUCCEEDED(result)) 
                {
                    LogError(L"WebView2 environment created successfully");
                    return env->CreateCoreWebView2Controller(
                        hwnd_,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT 
                            {
                                if (SUCCEEDED(result)) 
                                {
                                    LogError(L"WebView2 controller created successfully");
                                    webview_controller_ = controller;
                                    webview_controller_->get_CoreWebView2(&webview_);

                                    wil::com_ptr<ICoreWebView2Settings> settings;
                                    webview_->get_Settings(&settings);
                                    settings->put_IsWebMessageEnabled(TRUE);  // Required for postMessage to work
                                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                                    settings->put_IsBuiltInErrorPageEnabled(TRUE);

                                    // Add WebMessageReceived handler to log postMessage calls
                                    webview_->add_WebMessageReceived(
                                        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                            [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                LPWSTR message;
                                                HRESULT hr = args->get_WebMessageAsJson(&message);
                                                if (SUCCEEDED(hr)) {
                                                    // Log the raw JSON message
                                                    std::wstring messageStr = message;
                                                    if (messageStr.find(L"\"event\":\"log\"") != std::wstring::npos) {
                                                        LogMessage(L"WebView postMessage received: " + messageStr);
                                                    } else {
                                                        LogError(L"WebView postMessage received: " + messageStr);
                                                    }
                                                    
                                                    // Forward messages to Flutter event sink
                                                    if (event_sink_) {
                                                        int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
                                                        if (utf8Length > 0) {
                                                            std::string utf8Message;
                                                            utf8Message.resize(utf8Length - 1);
                                                            WideCharToMultiByte(CP_UTF8, 0, message, -1, &utf8Message[0], utf8Length, nullptr, nullptr);
                                                            event_sink_->Success(flutter::EncodableValue(utf8Message));
                                                        }
                                                    }
                                                    CoTaskMemFree(message);
                                                } else {
                                                    LogError(L"Failed to get WebMessageAsJson. Error: " + std::to_wstring(hr));
                                                }
                                                return S_OK;
                                            }).Get(),
                                        nullptr);

                                    // Enable WebRTC features
                                    webview_->add_WebResourceRequested(
                                        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                            [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                                LogError(L"Resource requested");
                                                return S_OK;
                                            }).Get(),
                                        nullptr);

                                    // Add navigation completed handler
                                    webview_->add_NavigationCompleted(
                                        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT 
                                            {
                                                BOOL success;
                                                args->get_IsSuccess(&success);
                                                if (success) {
                                                    LogError(L"Navigation completed successfully, checking Twilio SDK...");
                                                    // Check if Twilio is loaded after a short delay
                                                    Sleep(500); // Increased delay to 500ms
                                                    std::wstring checkTwilioJs = L"try { \
                                                        const twilioStatus = { \
                                                            isDefined: typeof Twilio !== 'undefined', \
                                                            deviceDefined: typeof Twilio !== 'undefined' && typeof Twilio.Device !== 'undefined', \
                                                            deviceInstance: typeof Twilio !== 'undefined' && typeof Twilio.Device !== 'undefined' && Twilio.Device.instance() !== null \
                                                        }; \
                                                        console.log('Twilio SDK check:', JSON.stringify(twilioStatus)); \
                                                        return JSON.stringify(twilioStatus); \
                                                    } catch(e) { \
                                                        console.error('Twilio check error:', e); \
                                                        return JSON.stringify({ error: e.message }); \
                                                    }";
                                                    webview_->ExecuteScript(checkTwilioJs.c_str(),
                                                        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                                                            [this](HRESULT error, LPCWSTR result) -> HRESULT 
                                                            {
                                                                if (SUCCEEDED(error) && result) {
                                                                    LogError(L"Twilio SDK check result: " + std::wstring(result));
                                                                    if (std::wstring(result).find(L"\"isDefined\":true") != std::wstring::npos) {
                                                                        twilio_sdk_ready_ = true;
                                                                        LogError(L"Twilio SDK loaded successfully");
                                                                    } else {
                                                                        LogError(L"Twilio SDK failed to load. Result: " + std::wstring(result));
                                                                    }
                                                                } else {
                                                                    LogError(L"Twilio SDK check failed. Error: " + std::to_wstring(error));
                                                                }
                                                                return S_OK;
                                                            }).Get());
                                                } else {
                                                    LogError(L"Navigation failed");
                                                }
                                                return S_OK;
                                            }).Get(),
                                        nullptr);

                                    LogError(L"Loading Twilio SDK...");
                                    // Construct the path to the HTML file relative to the application's folder
                                    wchar_t exePath[MAX_PATH];
                                    GetModuleFileName(nullptr, exePath, MAX_PATH);
                                    std::wstring exeDir = exePath;
                                    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\"));
                                    std::wstring htmlPath = exeDir + L"\\assets\\index.html";
                                    LogError(L"Navigating to: " + htmlPath);
                                    webview_->Navigate(htmlPath.c_str());
                                } else {
                                    LogError(L"Failed to create WebView2 controller. Error: " + std::to_wstring(result));
                                }
                                return S_OK;
                            }).Get());
                } else {
                    LogError(L"Failed to create WebView2 environment. Error: " + std::to_wstring(result));
                }
                return S_OK;
            }).Get());
  }

  HWND TwilioVoicePlugin::CreateWindowHandle() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"TwilioWebViewHost";
    
    RegisterClassExW(&wc);
    
    return CreateWindowExW(
      0,
      L"TwilioWebViewHost",
      L"Twilio WebView",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      1, 1,  // Minimal size since it's hidden
      nullptr,
      nullptr,
      GetModuleHandle(nullptr),
      nullptr);
  }

  void TwilioVoicePlugin::HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    if (method_call.method_name() == "hasMicPermission") {
        CheckMicPermissionAsync(std::move(result));
    }
    else if (method_call.method_name() == "requestMicPermission") {
        RequestMicPermissionAsync(std::move(result));
    }
    else if (method_call.method_name() == "tokens")
    {
        LogError(L"Handling 'tokens' method call");

        const auto *arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
        if (!arguments)
        {
            LogError(L"Invalid arguments for 'tokens' method");
            result->Error("INVALID_ARGUMENTS", "Invalid arguments");
            return;
        }

        // Extract tokens from arguments
        for (const auto &[key, value] : *arguments)
        {
            if (std::holds_alternative<std::string>(key))
            {
                auto key_str = std::get<std::string>(key);
                LogError(L"Processing key: " + std::wstring(key_str.begin(), key_str.end())); // Log key_str
                if (key_str == "accessToken" && std::holds_alternative<std::string>(value))
                {
                    access_token_ = std::get<std::string>(value);
                    LogError(L"Access token received: " + std::wstring(access_token_.begin(), access_token_.end()));
                }
                else if (key_str == "deviceToken" && std::holds_alternative<std::string>(value))
                {
                    device_token_ = std::get<std::string>(value); // Store but not used on Windows
                    LogError(L"Device token received: " + std::wstring(device_token_.begin(), device_token_.end()));
                }
                else
                {
                    LogError(L"Unexpected key or value type in 'tokens' arguments");
                }
            }
            else
            {
                LogError(L"Unexpected key type in 'tokens' arguments");
            }
        }

        if (access_token_.empty())
        {
            LogError(L"Access token is missing");
            result->Error("MISSING_TOKEN", "Access token is required");
            return;
        }

        if (webview_)
        {
            LogError(L"WebView is initialized, checking Twilio SDK readiness");
            CheckTwilioSDKReady(std::move(result));
        }
        else
        {
            LogError(L"WebView is not initialized");
            result->Error("WEBVIEW_NOT_INITIALIZED", "WebView not initialized");
        }
    }
    else if (method_call.method_name() == "makeCall")
    {
        if (!twilio_sdk_ready_) {
            LogError(L"Twilio SDK not ready");
            result->Error("SDK_NOT_READY", "Twilio SDK is not initialized");
            return;
        }

        const auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
        if (!arguments) {
            LogError(L"Invalid arguments for makeCall");
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
            LogError(L"Missing required From/To parameters");
            result->Error("INVALID_ARGUMENTS", "From and To parameters are required");
            return;
        }

        place(from, to, extraOptions, std::move(result));
    }
    else
    {
      result->NotImplemented();
    }
  }

  void TwilioVoicePlugin::CheckMicPermissionAsync(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    auto strongResult = std::move(result);
    
    try {
        LogError(L"Initializing MediaCapture for microphone permission check...");
        auto settings = winrt::Windows::Media::Capture::MediaCaptureInitializationSettings();
        auto capture = winrt::Windows::Media::Capture::MediaCapture();

        auto asyncOp = capture.InitializeAsync();
        asyncOp.Completed([strongResult = std::move(strongResult)](auto const& sender, winrt::Windows::Foundation::AsyncStatus const status) {
          try {
              if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                  sender.GetResults();
                  LogError(L"Microphone permission check completed successfully.");
                  strongResult->Success(flutter::EncodableValue(true));
              } else {
                  LogError(L"Microphone permission check did not complete successfully. Status: " + std::to_wstring(static_cast<int>(status)));
                  strongResult->Success(flutter::EncodableValue(false));
              }
          } catch (winrt::hresult_error const& ex) {
              LogError(L"Error during microphone permission check: " + std::wstring(ex.message()));
              strongResult->Success(flutter::EncodableValue(false));
          } catch (...) {
              LogError(L"Unknown error occurred during microphone permission check.");
              strongResult->Success(flutter::EncodableValue(false));
          }
      });
      
    }
    catch (const std::exception& ex) {
        LogError(L"Exception during microphone permission check: " + std::wstring(ex.what(), ex.what() + strlen(ex.what())));
        strongResult->Success(flutter::EncodableValue(false));
    }
    catch (...) {
        LogError(L"Failed to create MediaCapture for microphone permission check.");
        strongResult->Success(flutter::EncodableValue(false));
    }
}

  void TwilioVoicePlugin::RequestMicPermissionAsync(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    auto strongResult = std::move(result);
    
    try {
        LogError(L"Requesting microphone permission...");
        auto settings = winrt::Windows::Media::Capture::MediaCaptureInitializationSettings();
        settings.StreamingCaptureMode(winrt::Windows::Media::Capture::StreamingCaptureMode::Audio);
        auto capture = winrt::Windows::Media::Capture::MediaCapture();

        auto asyncOp = capture.InitializeAsync();
        asyncOp.Completed([strongResult = std::move(strongResult)](auto const& sender, winrt::Windows::Foundation::AsyncStatus const status) {
          try {
              if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                  sender.GetResults();
                  LogError(L"Microphone permission granted.");
                  strongResult->Success(flutter::EncodableValue(true));
              } else {
                  LogError(L"Microphone permission request did not complete successfully. Status: " + std::to_wstring(static_cast<int>(status)));
                  strongResult->Success(flutter::EncodableValue(false));
              }
          } catch (winrt::hresult_error const& ex) {
              LogError(L"Error during microphone permission request: " + std::wstring(ex.message()));
              strongResult->Success(flutter::EncodableValue(false));
          } catch (...) {
              LogError(L"Unknown error occurred during microphone permission request.");
              strongResult->Success(flutter::EncodableValue(false));
          }
      });
      
    }
    catch (const std::exception& ex) {
        LogError(L"Exception during microphone permission request: " + std::wstring(ex.what(), ex.what() + strlen(ex.what())));
        strongResult->Success(flutter::EncodableValue(false));
    }
    catch (...) {
        LogError(L"Failed to create MediaCapture for microphone permission request.");
        strongResult->Success(flutter::EncodableValue(false));
    }
}

  void TwilioVoicePlugin::SetEventSink(std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events) {
    event_sink_ = std::move(events);
  }

  void TwilioVoicePlugin::ClearEventSink() {
    event_sink_ = nullptr;
  }

  void TwilioVoicePlugin::SendEvent(const std::string& event) {
    if (event_sink_) {
      event_sink_->Success(flutter::EncodableValue(event));
    }
  }

  void TwilioVoicePlugin::RegisterCallEventHandlers() {
    if (!webview_) return;

    std::wstring js = L"\
        const device = Twilio.Device.instance(); \
        if (device) { \
            device.on('connect', call => { \
                window.chrome.webview.postMessage(JSON.stringify({ \
                    event: 'connected', \
                    from: call.parameters.From, \
                    to: call.parameters.To \
                })); \
            }); \
            device.on('disconnect', () => { \
                window.chrome.webview.postMessage(JSON.stringify({ \
                    event: 'disconnected' \
                })); \
            }); \
            device.on('error', error => { \
                window.chrome.webview.postMessage(JSON.stringify({ \
                    event: 'error', \
                    error: error.message \
                })); \
            }); \
        }";

    webview_->ExecuteScript(js.c_str(), nullptr);
    
    // Add message received handler with proper event token handling
    webview_->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR message;
                args->get_WebMessageAsJson(&message);
                if (event_sink_) {
                    // Convert wide string to UTF-8
                    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
                    if (utf8Length > 0) {
                        std::string utf8Message;
                        utf8Message.resize(utf8Length - 1); // -1 because WideCharToMultiByte includes null terminator
                        WideCharToMultiByte(CP_UTF8, 0, message, -1, &utf8Message[0], utf8Length, nullptr, nullptr);
                        event_sink_->Success(flutter::EncodableValue(utf8Message));
                    }
                }
                CoTaskMemFree(message);
                return S_OK;
            }).Get(),
        &web_message_token_); // Store token in class member
}

  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;

  void TwilioVoicePlugin::CheckTwilioSDKReady(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    if (!webview_) {
        LogError(L"CheckTwilioSDKReady: WebView not initialized");
        result->Error("SDK_NOT_READY", "WebView not initialized");
        return;
    }

    LogError(L"Checking Twilio SDK status...");
    std::wstring checkTwilioJs = LR"(
        try { 
            const twilioCheck = {
                twilioExists: typeof Twilio !== 'undefined',
                deviceExists: typeof Twilio !== 'undefined' && typeof Twilio.Device !== 'undefined',
                deviceInstance: Twilio && Twilio.Device && Twilio.Device.instance() !== null
            };
            console.log('Twilio check details:', JSON.stringify(twilioCheck));
            return JSON.stringify(twilioCheck);
        } catch(e) { 
            console.error('Twilio check error:', e);
            return JSON.stringify({ error: e.message }); 
        }
    )";

    LogError(L"Executing script: " + checkTwilioJs);
    webview_->ExecuteScript(checkTwilioJs.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [this, result = std::move(result)](HRESULT error, LPCWSTR checkResult) mutable -> HRESULT 
            {
                LogError(L"Raw check result: " + std::wstring(checkResult ? checkResult : L"<null>"));
                
                if (SUCCEEDED(error) && checkResult) {
                    // Parse the JSON result
                        LogError(L"Twilio SDK check passed, proceeding with setup");
                        SetupTwilioDevice(std::move(result));
                    // std::wstring resultStr(checkResult);
                    // LogError(L"Parsed check result: " + resultStr);
                    // if (resultStr.find(L"\"twilioExists\":true") != std::wstring::npos && 
                    //     resultStr.find(L"\"deviceExists\":true") != std::wstring::npos) {
                    //     LogError(L"Twilio SDK check passed, proceeding with setup");
                    //     SetupTwilioDevice(std::move(result));
                    // } else {
                    //     LogError(L"Twilio SDK check failed: " + resultStr);
                    //     result->Error("SDK_NOT_READY", "Twilio SDK is not properly initialized");
                    // }
                } else {
                    LogError(L"Twilio SDK check failed with error: " + std::to_wstring(error));
                    result->Error("SDK_NOT_READY", "Failed to check Twilio SDK status");
                }
                return S_OK;
            }).Get());
  }

  void TwilioVoicePlugin::SetupTwilioDevice(std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    LogError(L"Setting up Twilio Device...");
    std::wstring js = L"try { \
    const device = new Twilio.Device('" + std::wstring(access_token_.begin(), access_token_.end()) + L"', { \
        debug: true, \
        warnings: true \
    }); \
    device.on('connect', call => { \
        window.chrome.webview.postMessage(JSON.stringify({ \
            event: 'connected', \
            from: call.parameters.From, \
            to: call.parameters.To \
        })); \
    }); \
    device.on('disconnect', () => { \
        window.chrome.webview.postMessage(JSON.stringify({ \
            event: 'disconnected' \
        })); \
    }); \
    device.on('error', error => { \
        window.chrome.webview.postMessage(JSON.stringify({ \
            event: 'error', \
            error: error.message \
        })); \
    }); \
    window.chrome.webview.postMessage(JSON.stringify({ \
        type: 'log', \
        message: 'Twilio Device setup successful' \
    })); \
    return 'success'; \
} catch (error) { \
    window.chrome.webview.postMessage(JSON.stringify({ \
        type: 'error', \
        message: 'Twilio Device setup failed: ' + error.message \
    })); \
    throw error; \
}";

    webview_->ExecuteScript(
        js.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [this, result = std::move(result)](HRESULT error, LPCWSTR resultObject) mutable -> HRESULT
            {
                if (SUCCEEDED(error)) {
                    twilio_sdk_ready_ = true;
                    RegisterCallEventHandlers();
                    result->Success(flutter::EncodableValue(true));
                    std::wstring resultStr = resultObject ? resultObject : L"<null>";
                    LogError(L"Setup result: " + resultStr);
                    
                    // if (resultStr.find(L"setup_initiated") != std::wstring::npos) {
                    //     twilio_sdk_ready_ = true;
                    //     RegisterCallEventHandlers();
                    //     result->Success(flutter::EncodableValue(true));
                    // } else {
                    //     result->Error("SETUP_FAILED", "Failed to setup Twilio Device");
                    // }
                } else {
                    LogError(L"Setup script execution failed: " + std::to_wstring(error));
                    result->Error("SETUP_FAILED", "Failed to execute setup script");
                }
                return S_OK;
            }).Get());
  }

  void TwilioVoicePlugin::place(
    const std::string& from,
    const std::string& to,
    const std::map<std::string, std::string>& extraOptions,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
    
    if (from.empty() || to.empty()) {
      LogError(L"From and To parameters cannot be empty");
      result->Error("INVALID_PARAMETERS", "From and To parameters cannot be empty");
      return;
    }

    if (!webview_ || !twilio_sdk_ready_) {
      LogError(L"WebView or Twilio SDK not ready");
      result->Error("NOT_READY", "WebView or Twilio SDK not initialized");
      return;
    }

    LogError(L"Making new call");

    // Combine parameters
    std::map<std::string, std::string> params;
    params["From"] = from;
    params["To"] = to;
    params.insert(extraOptions.begin(), extraOptions.end());

    // Build JSON string for parameters
    std::wstring paramsJson = L"{";
    for (const auto& [key, value] : params) {
      if (!paramsJson.empty() && paramsJson != L"{") {
        paramsJson += L",";
      }
      paramsJson += L"'" + std::wstring(key.begin(), key.end()) + L"': '" 
                  + std::wstring(value.begin(), value.end()) + L"'";
    }
    paramsJson += L"}";

    std::wstring js = L"try { \
      const device = Twilio.Device.instance(); \
      if (!device) throw new Error('Twilio Device not ready'); \
      const connection = device.connect(" + paramsJson + L"); \
      if (!connection) throw new Error('Failed to create connection'); \
      window.chrome.webview.postMessage(JSON.stringify({ \
        event: 'log', \
        message: 'Call initiated', \
        params: " + paramsJson + L" \
      })); \
      true; \
    } catch(error) { \
      window.chrome.webview.postMessage(JSON.stringify({ \
        event: 'error', \
        message: error.message, \
        params: " + paramsJson + L" \
      })); \
      throw error; \
    }";

    webview_->ExecuteScript(
      js.c_str(),
      Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [this, result = std::move(result)](HRESULT error, LPCWSTR resultStr) -> HRESULT {
          if (SUCCEEDED(error) && resultStr && wcscmp(resultStr, L"true") == 0) {
            result->Success(flutter::EncodableValue(true));
          } else {
            LogError(L"Failed to place call");
            result->Error("CALL_FAILED", "Failed to place call");
          }
          return S_OK;
        }).Get());
  }

} // namespace twilio_voice

