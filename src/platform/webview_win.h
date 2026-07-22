// Native window with embedded browser engine.
// Priority: bundled WebView2 Fixed Version > system WebView2 > default browser.
#pragma once

#ifdef _WIN32

namespace kforge::platform {

/// Show a native window with an embedded browser pointing to `url`.
/// Checks for bundled runtime (webview2_runtime/ next to exe) first.
/// Falls back to system WebView2, then default browser.
/// Blocks until the window is closed.
bool show_webview_window(const char* url, const char* title,
                          int width, int height);

}  // namespace kforge::platform

#endif

