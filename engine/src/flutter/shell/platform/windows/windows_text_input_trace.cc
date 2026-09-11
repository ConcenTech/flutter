// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/windows_text_input_trace.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "flutter/fml/logging.h"

namespace flutter {

bool IsWindowsTextInputTraceEnabled() {
  static const bool enabled = [] {
    char value[16] = {};
    const DWORD length = GetEnvironmentVariableA(
        "FLUTTER_WINDOWS_TSF_TRACE", value, sizeof(value));
    return length > 0 && length < sizeof(value) &&
           std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

void WriteWindowsTextInputTrace(const char* component,
                                const std::string& message) {
  static std::atomic_uint64_t sequence = 0;
  const uint64_t event = ++sequence;
  FML_LOG(WARNING) << "[Windows TSF trace #" << event
                   << " thread=" << GetCurrentThreadId() << " " << component
                   << "] " << message;
}

}  // namespace flutter
