// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/on_screen_keyboard.h"

#include "flutter/fml/logging.h"

namespace flutter {

OnScreenKeyboardWin::OnScreenKeyboardWin(TaskRunner* task_runner)
    : task_runner_(task_runner), weak_factory_(this) {
  FML_DCHECK(task_runner_);
}

OnScreenKeyboardWin::~OnScreenKeyboardWin() = default;

void OnScreenKeyboardWin::SetVisibilityChangedCallback(
    VisibilityChanged callback) {
  callback_ = std::move(callback);
}

void OnScreenKeyboardWin::Display(HWND hwnd) {
  RequestVisibility(hwnd, true);
}

void OnScreenKeyboardWin::Dismiss(HWND hwnd) {
  RequestVisibility(hwnd, false);
}

bool OnScreenKeyboardWin::shown() const {
  return shown_;
}

double OnScreenKeyboardWin::physical_bottom_inset() const {
  return physical_bottom_inset_;
}

void OnScreenKeyboardWin::ApplyVisibility(HWND /*hwnd*/, bool /*show*/) {
  // WinRT IInputPane2::TryShow / TryHide is applied by a follow-up.
}

void OnScreenKeyboardWin::NotifyVisibilityChanged() {
  if (callback_) {
    callback_(shown_, physical_bottom_inset_);
  }
}

void OnScreenKeyboardWin::RequestVisibility(HWND hwnd, bool show) {
  if (hwnd == nullptr) {
    return;
  }

  pending_hwnd_ = hwnd;
  pending_show_ = show;
  const uint64_t generation = ++generation_;
  task_runner_->PostDelayedTask(
      [weak = weak_factory_.GetWeakPtr(), generation]() {
        if (!weak || generation != weak->generation_) {
          return;
        }
        weak->ApplyVisibility(weak->pending_hwnd_, weak->pending_show_);
      },
      kDisplayDismissDebounce);
}

}  // namespace flutter
