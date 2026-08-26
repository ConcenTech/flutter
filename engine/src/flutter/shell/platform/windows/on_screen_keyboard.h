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

// Controls the Windows on-screen (touch) keyboard via WinRT InputPane.
//
// Display and Dismiss request IInputPane2::TryShow / TryHide. Showing / Hiding
// events update |shown| and the physical bottom inset used for viewInsets.
// TSF, not this type, owns IME policy and auto-invoke suppression.
class OnScreenKeyboard {
 public:
  using VisibilityChanged =
      std::function<void(bool shown, double physical_bottom_inset)>;

  virtual ~OnScreenKeyboard() = default;

  // Sets the callback invoked when on-screen keyboard visibility changes.
  //
  // |physical_bottom_inset| is the occluded height of the Flutter view HWND
  // client area, in physical pixels.
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
// bottom inset. Does not call TryHide from a Showing handler.
class OnScreenKeyboardWin : public OnScreenKeyboard {
 public:
  // OccludedRect from IInputPaneVisibilityEventArgs, in root-window client
  // DIPs (96 DPI).
  struct DipRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
  };

  // Coalesces Display/Dismiss so field-to-field focus changes do not blink
  // the on-screen keyboard. Chromium chose 300 ms after experimenting with
  // users on Windows touch devices.
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

  // Converts |occluded_dip| (root-window client DIPs) to a physical screen
  // RECT: multiply by |dpi_scale|, then add |root_client_origin_screen|.
  static RECT OccludedDipToPhysicalScreenRect(
      const DipRect& occluded_dip,
      double dpi_scale,
      POINT root_client_origin_screen);

  // Bottom inset in physical pixels: the occluded strip at the bottom of
  // |view_client_screen|, clamped to [0, client height]. Returns 0 when the
  // rectangles do not intersect.
  static double ComputeBottomInset(const RECT& view_client_screen,
                                   const RECT& occluded_physical_screen);

  // Combines DIP → physical conversion with |ComputeBottomInset|.
  static double ComputePhysicalBottomInset(
      const DipRect& occluded_dip,
      double dpi_scale,
      POINT root_client_origin_screen,
      const RECT& view_client_screen);

  // Applies a Showing/Hiding observation. |occluded_dip| is ignored when
  // |shown| is false. Exposed for tests.
  void HandleVisibilityEvent(bool shown,
                             const DipRect& occluded_dip,
                             double dpi_scale,
                             POINT root_client_origin_screen,
                             const RECT& view_client_screen);

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
