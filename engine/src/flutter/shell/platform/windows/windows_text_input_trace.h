// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_WINDOWS_TEXT_INPUT_TRACE_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_WINDOWS_TEXT_INPUT_TRACE_H_

#include <sstream>
#include <string>
#include <utility>

namespace flutter {

// Returns whether Windows TSF/InputPane diagnostic tracing is enabled.
//
// Tracing is opt-in to avoid adding noise to normal engine logs. Set the
// FLUTTER_WINDOWS_TSF_TRACE environment variable to a value other than "0"
// before starting the application to enable it.
bool IsWindowsTextInputTraceEnabled();

// Writes one sequenced diagnostic event to the engine log.
void WriteWindowsTextInputTrace(const char* component,
                                const std::string& message);

template <typename... Args>
void TraceWindowsTextInput(const char* component, Args&&... args) {
  if (!IsWindowsTextInputTraceEnabled()) {
    return;
  }
  std::ostringstream message;
  (message << ... << std::forward<Args>(args));
  WriteWindowsTextInputTrace(component, message.str());
}

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_WINDOWS_TEXT_INPUT_TRACE_H_
