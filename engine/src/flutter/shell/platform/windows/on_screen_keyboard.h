// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_ON_SCREEN_KEYBOARD_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_ON_SCREEN_KEYBOARD_H_

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/fml/memory/weak_ptr.h"
#include "flutter/shell/platform/windows/task_runner.h"

namespace flutter {

// Controls the Windows on-screen (touch) keyboard.
//
// This type owns show/hide of the system InputPane. IMM32 remains the IME
// stack; this interface does not use TSF.
//
// Display and Dismiss are coalesced with a short delay so moving focus from
// one text field to another does not blink the keyboard.
class OnScreenKeyboard {
 public:
  using VisibilityChanged =
      std::function<void(bool shown, double physical_bottom_inset)>;

  virtual ~OnScreenKeyboard() = default;

  // Sets the callback invoked when on-screen keyboard visibility changes.
  //
  // |physical_bottom_inset| is the occluded height of the Flutter HWND client
  // area, in physical pixels.
  virtual void SetVisibilityChangedCallback(VisibilityChanged callback) = 0;

  // Requests that the on-screen keyboard be shown for |hwnd|.
  //
  // No-op if |hwnd| is null.
  virtual void Display(HWND hwnd) = 0;

  // Requests that the on-screen keyboard be hidden for |hwnd|.
  //
  // No-op if |hwnd| is null.
  virtual void Dismiss(HWND hwnd) = 0;

  // Returns whether the on-screen keyboard is currently shown.
  virtual bool shown() const = 0;

  // Returns the current bottom inset caused by the on-screen keyboard, in
  // physical pixels.
  virtual double physical_bottom_inset() const = 0;
};

// Default |OnScreenKeyboard| implementation.
//
// Debounces Display/Dismiss on the platform |TaskRunner|, then drives WinRT
// IInputPane2::TryShow / TryHide. Showing/Hiding update |shown| and the
// bottom inset.
class OnScreenKeyboardWin : public OnScreenKeyboard {
 public:
  // Coalesces Display/Dismiss so field-to-field focus changes do not blink
  // the on-screen keyboard.
  static constexpr std::chrono::milliseconds kDisplayDismissDebounce{300};

  // |task_runner| must outlive this object and is used to debounce
  // Display/Dismiss and to marshal InputPane events onto the platform thread.
  explicit OnScreenKeyboardWin(TaskRunner* task_runner);

  ~OnScreenKeyboardWin() override;

  // |OnScreenKeyboard|
  void SetVisibilityChangedCallback(VisibilityChanged callback) override;

  // |OnScreenKeyboard|
  void Display(HWND hwnd) override;

  // |OnScreenKeyboard|
  void Dismiss(HWND hwnd) override;

  // |OnScreenKeyboard|
  bool shown() const override;

  // |OnScreenKeyboard|
  double physical_bottom_inset() const override;

  // |client_screen| and |occluded_screen| are in screen coordinates.
  // Returns the bottom inset in physical pixels, clamped to the client height.
  static double ComputeBottomInset(const RECT& client_screen,
                                   const RECT& occluded_screen);

 protected:
  // Applies a coalesced show or hide via IInputPane2. Failures are ignored.
  virtual void ApplyVisibility(HWND hwnd, bool show);

  // Invokes |callback_| with the current shown state and bottom inset.
  void NotifyVisibilityChanged();

 private:
  struct InputPaneSession;

  void RequestVisibility(HWND hwnd, bool show);

  // Subscribes to InputPane Showing/Hiding for |hwnd|. No-op on COM/WinRT
  // failure (CO_E_NOTINITIALIZED, REGDB_E_CLASSNOTREG, invalid HWND).
  bool EnsureInputPane(HWND hwnd);

  void HandleInputPaneEvent(bool shown, const RECT& occluded_screen);

  TaskRunner* task_runner_;
  VisibilityChanged callback_;
  uint64_t generation_ = 0;
  HWND pending_hwnd_ = nullptr;
  bool pending_show_ = false;
  bool shown_ = false;
  double physical_bottom_inset_ = 0.0;
  std::unique_ptr<InputPaneSession> pane_session_;

  fml::WeakPtrFactory<OnScreenKeyboardWin> weak_factory_;

  FML_DISALLOW_COPY_AND_ASSIGN(OnScreenKeyboardWin);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_ON_SCREEN_KEYBOARD_H_
