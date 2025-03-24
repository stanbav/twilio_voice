#include "tv_webview.h"
#include <WebView2EnvironmentOptions.h>

TVWebView::TVWebView(HWND parentWindow) : parentWindow_(parentWindow) {
}

void TVWebView::initialize(std::function<void()> completionHandler) {
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    
    // Add browser command line options to help with permission handling
    options->put_AdditionalBrowserArguments(L"--use-fake-ui-for-media-stream");
    
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, options.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, completionHandler](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                env->CreateCoreWebView2Controller(parentWindow_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, completionHandler](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            controller_ = controller;
                            controller_->get_CoreWebView2(&webview_);
                            
                            // Initialize WebView settings
                            webview_->get_Settings(&settings_);
                            settings_->put_IsScriptEnabled(TRUE);
                            settings_->put_AreDefaultScriptDialogsEnabled(TRUE);
                            
                            // Add permission request handler to handle microphone permissions properly
                            webview_->add_PermissionRequested(
                                Microsoft::WRL::Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                    [this](ICoreWebView2* sender, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                                        COREWEBVIEW2_PERMISSION_KIND kind;
                                        args->get_PermissionKind(&kind);
                                        
                                        // If this is a microphone permission request
                                        if (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) {
                                            // Set state to allow and mark the decision as persisted
                                            // This will prevent showing the dialog a second time
                                            args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
                                        }
                                        
                                        return S_OK;
                                    }
                                ).Get(),
                                nullptr
                            );
                            
                            completionHandler();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void TVWebView::evaluateJavaScript(const std::wstring& javascript,
                                 std::function<void(void*, std::string)> completionHandler) {
    webview_->ExecuteScript(javascript.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [completionHandler](HRESULT error, LPCWSTR result) -> HRESULT {
                if (FAILED(error)) {
                    completionHandler(nullptr, "JavaScript execution failed");
                    return error;
                }
                // Handle result parsing
                completionHandler(const_cast<LPWSTR>(result), "");
                return S_OK;
            }).Get());
}

TVWebView::~TVWebView() {
    if (controller_) {
        controller_->Close();
    }
}

void TVWebView::loadHtmlString(const std::wstring& html) {
    if (webview_) {
        webview_->NavigateToString(html.c_str());
    }
}

void TVWebView::loadFile(const std::wstring& filePath, std::function<void()> completionHandler) {
    if (webview_) {
        // Add navigation completed handler
        webview_->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [completionHandler](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    if (completionHandler) {
                        completionHandler();
                    }
                    return S_OK;
                }).Get(),
            nullptr);
        
        // Navigate to the file
        webview_->Navigate(filePath.c_str());
    }
}
