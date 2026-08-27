// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/on_screen_keyboard.h"

#include <inputpaneinterop.h>
#include <roapi.h>
#include <windows.ui.viewmanagement.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "flutter/fml/logging.h"
#include "flutter/shell/platform/windows/dpi_utils.h"

namespace flutter {

namespace {

using ABI::Windows::Foundation::Rect;
using ABI::Windows::UI::ViewManagement::IInputPane;
using ABI::Windows::UI::ViewManagement::IInputPane2;
using ABI::Windows::UI::ViewManagement::IInputPaneVisibilityEventArgs;
using ABI::Windows::UI::ViewManagement::InputPane;
using ABI::Windows::UI::ViewManagement::InputPaneVisibilityEventArgs;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

using InputPaneVisibilityHandler =
    ABI::Windows::Foundation::ITypedEventHandler<InputPane*,
                                                 InputPaneVisibilityEventArgs*>;

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using WindowsCreateStringReferenceFn = HRESULT(WINAPI*)(PCWSTR,
                                                        UINT32,
                                                        HSTRING_HEADER*,
                                                        HSTRING*);

HMODULE CombaseModule() {
  static HMODULE module = LoadLibraryW(L"combase.dll");
  return module;
}

HRESULT RoGetActivationFactoryDyn(HSTRING class_id, REFIID iid, void** out) {
  static RoGetActivationFactoryFn fn = []() -> RoGetActivationFactoryFn {
    HMODULE module = CombaseModule();
    if (!module) {
      return nullptr;
    }
    return reinterpret_cast<RoGetActivationFactoryFn>(
        GetProcAddress(module, "RoGetActivationFactory"));
  }();
  if (!fn) {
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }
  return fn(class_id, iid, out);
}

HRESULT CreateHStringReference(const wchar_t* source,
                               HSTRING_HEADER* header,
                               HSTRING* out) {
  static WindowsCreateStringReferenceFn fn =
      []() -> WindowsCreateStringReferenceFn {
    HMODULE module = CombaseModule();
    if (!module) {
      return nullptr;
    }
    return reinterpret_cast<WindowsCreateStringReferenceFn>(
        GetProcAddress(module, "WindowsCreateStringReference"));
  }();
  if (!fn) {
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  }
  return fn(source, static_cast<UINT32>(wcslen(source)), header, out);
}

void LogInputPaneFailure(const char* api, HRESULT hr) {
  FML_LOG(WARNING) << "On-screen keyboard " << api << " failed: 0x" << std::hex
                   << static_cast<unsigned long>(hr);
}

HWND RootWindow(HWND hwnd) {
  HWND root = GetAncestor(hwnd, GA_ROOT);
  return root ? root : hwnd;
}

bool MapClientRectToScreen(HWND hwnd, RECT* client_screen) {
  RECT client{};
  if (!GetClientRect(hwnd, &client)) {
    return false;
  }
  POINT top_left{client.left, client.top};
  POINT bottom_right{client.right, client.bottom};
  if (!ClientToScreen(hwnd, &top_left) ||
      !ClientToScreen(hwnd, &bottom_right)) {
    return false;
  }
  client_screen->left = top_left.x;
  client_screen->top = top_left.y;
  client_screen->right = bottom_right.x;
  client_screen->bottom = bottom_right.y;
  return true;
}

HRESULT GetInputPaneForWindow(HWND hwnd, IInputPane** pane) {
  HSTRING_HEADER header;
  HSTRING class_id = nullptr;
  HRESULT hr = CreateHStringReference(
      RuntimeClass_Windows_UI_ViewManagement_InputPane, &header, &class_id);
  if (FAILED(hr)) {
    return hr;
  }

  ComPtr<IInputPaneInterop> interop;
  hr = RoGetActivationFactoryDyn(class_id, IID_PPV_ARGS(&interop));
  if (FAILED(hr)) {
    return hr;
  }
  return interop->GetForWindow(RootWindow(hwnd), IID_PPV_ARGS(pane));
}

}  // namespace

struct OnScreenKeyboardWin::InputPaneSession {
  HWND view_hwnd = nullptr;
  ComPtr<IInputPane> pane;
  EventRegistrationToken showing_token{};
  EventRegistrationToken hiding_token{};
  bool showing_subscribed = false;
  bool hiding_subscribed = false;

  ~InputPaneSession() {
    if (!pane) {
      return;
    }
    if (showing_subscribed) {
      pane->remove_Showing(showing_token);
    }
    if (hiding_subscribed) {
      pane->remove_Hiding(hiding_token);
    }
  }
};

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
  if (suppress_display_) {
    return;
  }
  RequestVisibility(hwnd, true);
}

void OnScreenKeyboardWin::Dismiss(HWND hwnd) {
  RequestVisibility(hwnd, false);
}

void OnScreenKeyboardWin::OnUserGesture() {
  suppress_display_ = false;
}

void OnScreenKeyboardWin::OnClientCleared() {
  CancelPendingDisplay();
}

bool OnScreenKeyboardWin::display_suppressed() const {
  return suppress_display_;
}

bool OnScreenKeyboardWin::shown() const {
  return shown_;
}

double OnScreenKeyboardWin::physical_bottom_inset() const {
  return physical_bottom_inset_;
}

RECT OnScreenKeyboardWin::OccludedDipToPhysicalScreenRect(
    const DipRect& occluded_dip,
    double dpi_scale,
    POINT root_client_origin_screen) {
  const double physical_x = occluded_dip.x * dpi_scale;
  const double physical_y = occluded_dip.y * dpi_scale;
  const double physical_width = occluded_dip.width * dpi_scale;
  const double physical_height = occluded_dip.height * dpi_scale;
  RECT result;
  result.left =
      static_cast<LONG>(std::lround(physical_x + root_client_origin_screen.x));
  result.top =
      static_cast<LONG>(std::lround(physical_y + root_client_origin_screen.y));
  result.right = static_cast<LONG>(
      std::lround(physical_x + physical_width + root_client_origin_screen.x));
  result.bottom = static_cast<LONG>(
      std::lround(physical_y + physical_height + root_client_origin_screen.y));
  return result;
}

double OnScreenKeyboardWin::ComputeBottomInset(
    const RECT& view_client_screen,
    const RECT& occluded_physical_screen) {
  const LONG intersect_left =
      std::max(view_client_screen.left, occluded_physical_screen.left);
  const LONG intersect_top =
      std::max(view_client_screen.top, occluded_physical_screen.top);
  const LONG intersect_right =
      std::min(view_client_screen.right, occluded_physical_screen.right);
  const LONG intersect_bottom =
      std::min(view_client_screen.bottom, occluded_physical_screen.bottom);
  if (intersect_right <= intersect_left || intersect_bottom <= intersect_top) {
    return 0.0;
  }

  const double client_height =
      static_cast<double>(view_client_screen.bottom - view_client_screen.top);
  if (client_height <= 0.0) {
    return 0.0;
  }

  const double inset = static_cast<double>(view_client_screen.bottom) -
                       static_cast<double>(intersect_top);
  return std::clamp(inset, 0.0, client_height);
}

double OnScreenKeyboardWin::ComputePhysicalBottomInset(
    const DipRect& occluded_dip,
    double dpi_scale,
    POINT root_client_origin_screen,
    const RECT& view_client_screen) {
  const RECT occluded_screen = OccludedDipToPhysicalScreenRect(
      occluded_dip, dpi_scale, root_client_origin_screen);
  return ComputeBottomInset(view_client_screen, occluded_screen);
}

void OnScreenKeyboardWin::ApplyVisibility(HWND hwnd, bool show) {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return;
  }
  if (!EnsureInputPane(hwnd) || !pane_session_ || !pane_session_->pane) {
    return;
  }

  ComPtr<IInputPane2> pane2;
  HRESULT hr = pane_session_->pane.As(&pane2);
  if (FAILED(hr) || !pane2) {
    LogInputPaneFailure("QueryInterface(IInputPane2)", hr);
    return;
  }

  boolean succeeded = FALSE;
  hr = show ? pane2->TryShow(&succeeded) : pane2->TryHide(&succeeded);
  if (FAILED(hr)) {
    LogInputPaneFailure(show ? "TryShow" : "TryHide", hr);
  }
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
  if (!show) {
    hide_requested_ = true;
  }
  const uint64_t generation = ++generation_;
  task_runner_->PostDelayedTask(
      [weak = weak_factory_.GetWeakPtr(), generation]() {
        if (!weak || generation != weak->generation_) {
          return;
        }
        const bool show = weak->pending_show_;
        const HWND hwnd = weak->pending_hwnd_;
        weak->pending_show_ = false;
        weak->ApplyVisibility(hwnd, show);
      },
      kDisplayDismissDebounce);
}

void OnScreenKeyboardWin::CancelPendingDisplay() {
  if (!pending_show_) {
    return;
  }
  ++generation_;
  pending_show_ = false;
}

bool OnScreenKeyboardWin::EnsureInputPane(HWND hwnd) {
  if (pane_session_ && pane_session_->view_hwnd == hwnd &&
      pane_session_->pane) {
    return true;
  }

  pane_session_.reset();
  if (!IsWindow(hwnd)) {
    return false;
  }

  ComPtr<IInputPane> pane;
  HRESULT hr = GetInputPaneForWindow(hwnd, &pane);
  if (FAILED(hr) || !pane) {
    LogInputPaneFailure("GetForWindow", hr);
    return false;
  }

  auto session = std::make_unique<InputPaneSession>();
  session->view_hwnd = hwnd;
  session->pane = pane;

  auto showing_handler = Callback<InputPaneVisibilityHandler>(
      [runner = task_runner_, weak = weak_factory_.GetWeakPtr()](
          IInputPane* /*sender*/, IInputPaneVisibilityEventArgs* args) {
        DipRect occluded_dip{};
        if (args) {
          Rect occluded{};
          if (SUCCEEDED(args->get_OccludedRect(&occluded))) {
            occluded_dip.x = occluded.X;
            occluded_dip.y = occluded.Y;
            occluded_dip.width = occluded.Width;
            occluded_dip.height = occluded.Height;
          }
        }
        // InputPane is not agile; marshal before touching engine state.
        runner->RunNowOrPostTask([weak, occluded_dip]() {
          if (!weak) {
            return;
          }
          HWND view = weak->pane_session_ ? weak->pane_session_->view_hwnd
                                          : weak->pending_hwnd_;
          if (!view || !IsWindow(view)) {
            return;
          }
          HWND root = RootWindow(view);
          const double scale = static_cast<double>(GetDpiForHWND(root)) /
                               static_cast<double>(kDefaultDpi);
          POINT origin{0, 0};
          ClientToScreen(root, &origin);
          RECT view_client{};
          if (!MapClientRectToScreen(view, &view_client)) {
            return;
          }
          weak->HandleVisibilityEvent(true, occluded_dip, scale, origin,
                                      view_client);
        });
        return S_OK;
      });
  auto hiding_handler = Callback<InputPaneVisibilityHandler>(
      [runner = task_runner_, weak = weak_factory_.GetWeakPtr()](
          IInputPane* /*sender*/, IInputPaneVisibilityEventArgs* /*args*/) {
        runner->RunNowOrPostTask([weak]() {
          if (!weak) {
            return;
          }
          RECT empty{};
          weak->HandleVisibilityEvent(false, DipRect{}, 1.0, POINT{0, 0},
                                      empty);
        });
        return S_OK;
      });

  if (!showing_handler || !hiding_handler) {
    LogInputPaneFailure("Callback", E_OUTOFMEMORY);
    return false;
  }

  hr = pane->add_Showing(showing_handler.Get(), &session->showing_token);
  if (FAILED(hr)) {
    LogInputPaneFailure("add_Showing", hr);
    return false;
  }
  session->showing_subscribed = true;

  hr = pane->add_Hiding(hiding_handler.Get(), &session->hiding_token);
  if (FAILED(hr)) {
    LogInputPaneFailure("add_Hiding", hr);
    return false;
  }
  session->hiding_subscribed = true;

  pane_session_ = std::move(session);
  return true;
}

void OnScreenKeyboardWin::HandleVisibilityEvent(
    bool shown,
    const DipRect& occluded_dip,
    double dpi_scale,
    POINT root_client_origin_screen,
    const RECT& view_client_screen) {
  shown_ = shown;
  if (shown) {
    // Do not clear suppress_display_. An OS auto-show must not unlock
    // TryShow; only OnUserGesture (a pointer event) does.
    hide_requested_ = false;
    physical_bottom_inset_ = ComputePhysicalBottomInset(
        occluded_dip, dpi_scale, root_client_origin_screen, view_client_screen);
  } else {
    const bool hide_was_requested = hide_requested_;
    hide_requested_ = false;
    if (!hide_was_requested) {
      // The user dismissed the InputPane (taskbar, tap on the SIP, etc.).
      // Do not TryShow again until a new pointer gesture.
      suppress_display_ = true;
      CancelPendingDisplay();
    }
    physical_bottom_inset_ = 0.0;
  }
  NotifyVisibilityChanged();
}

}  // namespace flutter
