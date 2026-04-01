// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Emscripten/JS bridge for EditudeTestActions — WASM builds only.
//
// Exports C functions callable from JavaScript via Module.ccall().
// The iOS Swift TestDriverServer calls these through
// WKWebView.evaluateJavaScript("window.editudeTest.xxx(...)").

#ifdef Q_OS_WASM

namespace mu::editude::internal {
class EditudeService;

/// Initialise the WASM test bridge.  Must be called after the
/// EditudeService is fully constructed (typically in EditudeModule::onInit).
/// Registers `window.editudeTest` on the JS global scope.
void initTestBridge(EditudeService* svc);

} // namespace mu::editude::internal

#endif // Q_OS_WASM
