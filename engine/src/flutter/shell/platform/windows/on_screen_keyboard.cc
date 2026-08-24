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
    ABI::Windows::Foundation::ITypedEventHandler<
        InputPane*,
        InputPaneVisibilityEventArgs*>;

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using WindowsCreateStringReferenceFn =
    HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);

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

RECT WinrtRectToScreenRect(const Rect& rect) {
  RECT result;
  result.left = static_cast<LONG>(std::lround(rect.X));
  result.top = static_cast<LONG>(std::lround(rect.Y));
  result.right = static_cast<LONG>(std::lround(rect.X + rect.Width));
  result.bottom = static_cast<LONG>(std::lround(rect.Y + rect.Height));
  return result;
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
  RequestVisibility(hwnd, true);
}

void OnScreenKeyboardWin::Dismiss(HWND hwnd) {
  // Record hide intent even when |hwnd| is null so a later OS auto-show is
  // rejected. Display still requires a window to TryShow.
  want_visible_ = false;
  RequestVisibility(hwnd, false);
}

bool OnScreenKeyboardWin::shown() const {
  return shown_;
}

double OnScreenKeyboardWin::physical_bottom_inset() const {
  return physical_bottom_inset_;
}

double OnScreenKeyboardWin::ComputeBottomInset(const RECT& client_screen,
                                               const RECT& occluded_screen) {
  const LONG intersect_left =
      std::max(client_screen.left, occluded_screen.left);
  const LONG intersect_top = std::max(client_screen.top, occluded_screen.top);
  const LONG intersect_right =
      std::min(client_screen.right, occluded_screen.right);
  const LONG intersect_bottom =
      std::min(client_screen.bottom, occluded_screen.bottom);
  if (intersect_right <= intersect_left ||
      intersect_bottom <= intersect_top) {
    return 0.0;
  }

  const double client_height =
      static_cast<double>(client_screen.bottom - client_screen.top);
  if (client_height <= 0.0) {
    return 0.0;
  }

  const double inset = static_cast<double>(client_screen.bottom) -
                       static_cast<double>(occluded_screen.top);
  return std::clamp(inset, 0.0, client_height);
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
  want_visible_ = show;
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
      [weak = weak_factory_.GetWeakPtr(), runner = task_runner_](
          IInputPane* /*sender*/, IInputPaneVisibilityEventArgs* args) {
        if (!args) {
          return S_OK;
        }
        Rect occluded{};
        if (FAILED(args->get_OccludedRect(&occluded))) {
          return S_OK;
        }
        const RECT screen = WinrtRectToScreenRect(occluded);
        // InputPane is not agile. Always post so TryHide is not invoked from
        // inside the Showing handler (WinRT event reentrancy).
        runner->PostTask([weak, screen]() {
          if (!weak) {
            return;
          }
          weak->HandleInputPaneEvent(true, screen);
        });
        return S_OK;
      });
  auto hiding_handler = Callback<InputPaneVisibilityHandler>(
      [weak = weak_factory_.GetWeakPtr(), runner = task_runner_](
          IInputPane* /*sender*/, IInputPaneVisibilityEventArgs* /*args*/) {
        RECT empty{};
        runner->PostTask([weak, empty]() {
          if (!weak) {
            return;
          }
          weak->HandleInputPaneEvent(false, empty);
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

void OnScreenKeyboardWin::HandleInputPaneEvent(bool pane_shown,
                                               const RECT& occluded_screen) {
  HWND hwnd = pane_session_ ? pane_session_->view_hwnd : pending_hwnd_;
  if (pane_shown && !want_visible_) {
    // OS auto-invoked the pane after Dismiss or a user SIP dismiss. Hide
    // without reporting occlusion so request and inset observation stay
    // decoupled.
    ApplyVisibility(hwnd, false);
    return;
  }

  shown_ = pane_shown;
  if (!pane_shown) {
    want_visible_ = false;
  }
  if (pane_shown && hwnd) {
    RECT client_screen{};
    if (MapClientRectToScreen(hwnd, &client_screen)) {
      physical_bottom_inset_ =
          ComputeBottomInset(client_screen, occluded_screen);
    } else {
      physical_bottom_inset_ = 0.0;
    }
  } else {
    physical_bottom_inset_ = 0.0;
  }
  NotifyVisibilityChanged();
}

}  // namespace flutter
