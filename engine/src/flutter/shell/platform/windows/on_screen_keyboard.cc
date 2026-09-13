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
#include <mutex>
#include <unordered_map>

#include "flutter/fml/logging.h"
#include "flutter/shell/platform/windows/dpi_utils.h"
#include "flutter/shell/platform/windows/windows_text_input_trace.h"

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

std::mutex g_move_size_hooks_mutex;
std::unordered_map<HWINEVENTHOOK, OnScreenKeyboardWin*> g_move_size_hooks;

void CALLBACK OnMoveSizeWinEvent(HWINEVENTHOOK hook,
                                 DWORD event,
                                 HWND hwnd,
                                 LONG /*object_id*/,
                                 LONG /*child_id*/,
                                 DWORD /*event_thread*/,
                                 DWORD /*event_time*/) {
  if (event != EVENT_SYSTEM_MOVESIZEEND || hwnd == nullptr) {
    return;
  }
  OnScreenKeyboardWin* keyboard = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_move_size_hooks_mutex);
    const auto iterator = g_move_size_hooks.find(hook);
    if (iterator != g_move_size_hooks.end()) {
      keyboard = iterator->second;
    }
  }
  if (keyboard != nullptr) {
    keyboard->OnRootWindowMoveSizeEnded(hwnd);
  }
}

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

OnScreenKeyboardWin::~OnScreenKeyboardWin() {
  RestoreWindowAfterKeyboard();
}

void OnScreenKeyboardWin::SetVisibilityChangedCallback(
    VisibilityChanged callback) {
  callback_ = std::move(callback);
}

void OnScreenKeyboardWin::Display(HWND hwnd) {
  TraceWindowsTextInput("InputPane", "Display requested hwnd=", hwnd);
  RequestVisibility(hwnd, true);
}

void OnScreenKeyboardWin::Dismiss(HWND hwnd) {
  if (!shown_ && !pending_show_ && !show_request_in_flight_) {
    TraceWindowsTextInput(
        "InputPane",
        "Dismiss ignored because keyboard is hidden and no Display is pending");
    return;
  }
  TraceWindowsTextInput("InputPane", "Dismiss requested hwnd=", hwnd);
  RequestVisibility(hwnd, false);
}

void OnScreenKeyboardWin::OnClientCleared() {
  TraceWindowsTextInput("InputPane",
                        "client cleared; cancel pending Display if present");
  CancelPendingDisplay();
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

RECT OnScreenKeyboardWin::ComputeWindowRectAboveOcclusion(
    const RECT& window_screen,
    const RECT& work_area,
    const RECT& occluded_screen) {
  const bool overlaps_horizontally =
      window_screen.left < occluded_screen.right &&
      window_screen.right > occluded_screen.left;
  const LONG available_bottom =
      std::min(work_area.bottom, occluded_screen.top);
  if (!overlaps_horizontally || available_bottom <= work_area.top ||
      window_screen.bottom <= available_bottom) {
    return window_screen;
  }

  RECT result = window_screen;
  const LONG window_height = window_screen.bottom - window_screen.top;
  const LONG available_height = available_bottom - work_area.top;
  if (window_height <= available_height) {
    result.bottom = available_bottom;
    result.top = available_bottom - window_height;
  } else {
    result.top = work_area.top;
    result.bottom = available_bottom;
  }
  return result;
}

void OnScreenKeyboardWin::ApplyVisibility(HWND hwnd, bool show) {
  TraceWindowsTextInput("InputPane", "apply ", show ? "Display" : "Dismiss",
                        " hwnd=", hwnd, " generation=", generation_);
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    TraceWindowsTextInput("InputPane",
                          "apply ignored because HWND is null or invalid");
    return;
  }
  if (!EnsureInputPane(hwnd) || !pane_session_ || !pane_session_->pane) {
    TraceWindowsTextInput("InputPane",
                          "apply ignored because InputPane is unavailable");
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
  TraceWindowsTextInput("InputPane", show ? "TryShow" : "TryHide",
                        " hr=0x", std::hex,
                        static_cast<unsigned long>(hr), std::dec,
                        " succeeded=", succeeded != FALSE);
}

void OnScreenKeyboardWin::NotifyVisibilityChanged() {
  if (callback_) {
    callback_(shown_, physical_bottom_inset_);
  }
}

void OnScreenKeyboardWin::RequestVisibility(HWND hwnd, bool show) {
  if (hwnd == nullptr) {
    TraceWindowsTextInput("InputPane", show ? "Display" : "Dismiss",
                          " ignored because HWND is null");
    return;
  }

  const uint64_t previous_generation = generation_;
  const bool previous_show = pending_show_;
  pending_hwnd_ = hwnd;
  pending_show_ = show;
  const uint64_t generation = ++generation_;
  TraceWindowsTextInput(
      "InputPane", "queue ", show ? "Display" : "Dismiss", " hwnd=", hwnd,
      " generation=", generation, " supersedes_generation=",
      previous_generation, " previous_pending_show=", previous_show,
      " delay_ms=", kDisplayDismissDebounce.count());
  task_runner_->PostDelayedTask(
      [weak = weak_factory_.GetWeakPtr(), generation]() {
        if (!weak || generation != weak->generation_) {
          if (weak) {
            TraceWindowsTextInput(
                "InputPane", "skip superseded request generation=", generation,
                " current_generation=", weak->generation_);
          }
          return;
        }
        const bool show = weak->pending_show_;
        const HWND hwnd = weak->pending_hwnd_;
        weak->pending_show_ = false;
        // A Dismiss may supersede an applied Display before Showing arrives.
        weak->show_request_in_flight_ = show;
        weak->ApplyVisibility(hwnd, show);
      },
      kDisplayDismissDebounce);
}

void OnScreenKeyboardWin::CancelPendingDisplay() {
  if (!pending_show_) {
    TraceWindowsTextInput("InputPane",
                          "no pending Display to cancel");
    return;
  }
  const uint64_t cancelled_generation = generation_;
  ++generation_;
  pending_show_ = false;
  TraceWindowsTextInput("InputPane",
                        "cancel pending Display generation=",
                        cancelled_generation, " new_generation=", generation_);
}

bool OnScreenKeyboardWin::EnsureInputPane(HWND hwnd) {
  if (pane_session_ && pane_session_->view_hwnd == hwnd &&
      pane_session_->pane) {
    TraceWindowsTextInput("InputPane", "reuse session view_hwnd=", hwnd);
    return true;
  }

  pane_session_.reset();
  if (!IsWindow(hwnd)) {
    TraceWindowsTextInput("InputPane",
                          "cannot create session for invalid hwnd=", hwnd);
    return false;
  }

  ComPtr<IInputPane> pane;
  HRESULT hr = GetInputPaneForWindow(hwnd, &pane);
  if (FAILED(hr) || !pane) {
    LogInputPaneFailure("GetForWindow", hr);
    return false;
  }
  TraceWindowsTextInput("InputPane", "GetForWindow succeeded view_hwnd=", hwnd,
                        " root_hwnd=", RootWindow(hwnd));

  auto session = std::make_unique<InputPaneSession>();
  session->view_hwnd = hwnd;
  session->pane = pane;

  auto showing_handler = Callback<InputPaneVisibilityHandler>(
      [runner = task_runner_, weak = weak_factory_.GetWeakPtr(),
       view_hwnd = hwnd](IInputPane* /*sender*/,
                         IInputPaneVisibilityEventArgs* args) {
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
        // Capture the coordinate-space conversion with the event. The root
        // window may move before the marshalled task runs.
        HWND root = RootWindow(view_hwnd);
        const double scale = static_cast<double>(GetDpiForHWND(root)) /
                             static_cast<double>(kDefaultDpi);
        POINT origin{0, 0};
        ClientToScreen(root, &origin);
        // InputPane is not agile; marshal before touching engine state.
        runner->RunNowOrPostTask([weak, occluded_dip, scale, origin]() {
          if (!weak) {
            return;
          }
          TraceWindowsTextInput(
              "InputPane", "Showing callback marshalled dip_rect=(",
              occluded_dip.x, ",", occluded_dip.y, ",", occluded_dip.width,
              ",", occluded_dip.height, ")");
          HWND view = weak->pane_session_ ? weak->pane_session_->view_hwnd
                                          : weak->pending_hwnd_;
          if (!view || !IsWindow(view)) {
            TraceWindowsTextInput(
                "InputPane",
                "Showing callback ignored because view HWND is invalid");
            return;
          }
          RECT view_client{};
          if (!MapClientRectToScreen(view, &view_client)) {
            TraceWindowsTextInput(
                "InputPane",
                "Showing callback ignored because view rect mapping failed");
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
          TraceWindowsTextInput("InputPane",
                                "Hiding callback marshalled");
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

void OnScreenKeyboardWin::OnRootWindowMoveSizeEnded(HWND hwnd) {
  task_runner_->RunNowOrPostTask(
      [weak = weak_factory_.GetWeakPtr(), hwnd]() {
        if (!weak || !weak->shown_ || hwnd != weak->tracked_root_ ||
            weak->applying_window_placement_) {
          return;
        }
        weak->UpdateWindowForOcclusion(hwnd);
        weak->NotifyVisibilityChanged();
      });
}

void OnScreenKeyboardWin::StartTrackingWindow(HWND root) {
  if (tracked_root_ == root) {
    return;
  }
  if (original_window_placement_ && IsWindow(original_placement_root_)) {
    applying_window_placement_ = true;
    SetWindowPlacement(original_placement_root_, &*original_window_placement_);
    applying_window_placement_ = false;
    original_window_placement_.reset();
    original_placement_root_ = nullptr;
  }
  StopTrackingWindow();
  tracked_root_ = root;
  move_size_hook_ = SetWinEventHook(
      EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr,
      OnMoveSizeWinEvent, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
  if (move_size_hook_ != nullptr) {
    std::lock_guard<std::mutex> lock(g_move_size_hooks_mutex);
    g_move_size_hooks[move_size_hook_] = this;
  }
}

void OnScreenKeyboardWin::StopTrackingWindow() {
  if (move_size_hook_ != nullptr) {
    {
      std::lock_guard<std::mutex> lock(g_move_size_hooks_mutex);
      g_move_size_hooks.erase(move_size_hook_);
    }
    UnhookWinEvent(move_size_hook_);
    move_size_hook_ = nullptr;
  }
  tracked_root_ = nullptr;
}

void OnScreenKeyboardWin::UpdateWindowForOcclusion(HWND root) {
  if (!IsWindow(root) || !has_occluded_physical_screen_) {
    return;
  }

  RECT view_client_screen{};
  HWND view = pane_session_ ? pane_session_->view_hwnd : nullptr;
  if (IsZoomed(root) || IsIconic(root)) {
    // A user-initiated maximize supersedes any placement that was saved while
    // the keyboard was open.
    original_window_placement_.reset();
    original_placement_root_ = nullptr;
    if (view != nullptr && MapClientRectToScreen(view, &view_client_screen)) {
      physical_bottom_inset_ =
          ComputeBottomInset(view_client_screen, occluded_physical_screen_);
    }
    return;
  }

  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  HMONITOR monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
  RECT window_screen{};
  if (monitor == nullptr || !GetMonitorInfo(monitor, &monitor_info) ||
      !GetWindowRect(root, &window_screen)) {
    return;
  }

  const RECT adjusted = ComputeWindowRectAboveOcclusion(
      window_screen, monitor_info.rcWork, occluded_physical_screen_);
  if (adjusted.left != window_screen.left ||
      adjusted.top != window_screen.top ||
      adjusted.right != window_screen.right ||
      adjusted.bottom != window_screen.bottom) {
    if (!original_window_placement_) {
      WINDOWPLACEMENT placement{};
      placement.length = sizeof(placement);
      if (GetWindowPlacement(root, &placement)) {
        original_window_placement_ = placement;
        original_placement_root_ = root;
      }
    }
    applying_window_placement_ = true;
    SetWindowPos(root, nullptr, adjusted.left, adjusted.top,
                 adjusted.right - adjusted.left,
                 adjusted.bottom - adjusted.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
    applying_window_placement_ = false;
  }

  if (view != nullptr && MapClientRectToScreen(view, &view_client_screen)) {
    physical_bottom_inset_ =
        ComputeBottomInset(view_client_screen, occluded_physical_screen_);
  }
}

void OnScreenKeyboardWin::RestoreWindowAfterKeyboard() {
  StopTrackingWindow();
  if (original_window_placement_ && IsWindow(original_placement_root_)) {
    applying_window_placement_ = true;
    SetWindowPlacement(original_placement_root_, &*original_window_placement_);
    applying_window_placement_ = false;
  }
  original_window_placement_.reset();
  original_placement_root_ = nullptr;
  has_occluded_physical_screen_ = false;
}

void OnScreenKeyboardWin::HandleVisibilityEvent(
    bool shown,
    const DipRect& occluded_dip,
    double dpi_scale,
    POINT root_client_origin_screen,
    const RECT& view_client_screen) {
  shown_ = shown;
  bool notify_immediately = true;
  if (shown) {
    const uint64_t geometry_generation = ++geometry_generation_;
    show_request_in_flight_ = false;
    occluded_physical_screen_ = OccludedDipToPhysicalScreenRect(
        occluded_dip, dpi_scale, root_client_origin_screen);
    has_occluded_physical_screen_ = true;
    HWND view = pane_session_ ? pane_session_->view_hwnd : nullptr;
    HWND root = view != nullptr ? RootWindow(view) : nullptr;
    if (root != nullptr && IsWindow(root)) {
      StartTrackingWindow(root);
      if (IsZoomed(root)) {
        // Maximized windows remain fixed and consume the occlusion as an
        // inset, so there is no geometry animation to coalesce.
        UpdateWindowForOcclusion(root);
      } else {
        // InputPane may report several intermediate rectangles while opening.
        // Apply restored-window geometry only after the final observation has
        // remained stable for the debounce interval.
        notify_immediately = false;
        task_runner_->PostDelayedTask(
            [weak = weak_factory_.GetWeakPtr(), geometry_generation, root]() {
              if (!weak || !weak->shown_ ||
                  geometry_generation != weak->geometry_generation_ ||
                  root != weak->tracked_root_) {
                return;
              }
              weak->UpdateWindowForOcclusion(root);
              weak->NotifyVisibilityChanged();
            },
            kDisplayDismissDebounce);
      }
    } else {
      physical_bottom_inset_ =
          ComputeBottomInset(view_client_screen, occluded_physical_screen_);
    }
    TraceWindowsTextInput(
        "InputPane", "Showing dip_rect=(", occluded_dip.x, ",",
        occluded_dip.y, ",", occluded_dip.width, ",", occluded_dip.height,
        ") dpi_scale=", dpi_scale, " root_origin=(",
        root_client_origin_screen.x, ",", root_client_origin_screen.y,
        ") view_client_screen=(", view_client_screen.left, ",",
        view_client_screen.top, ",", view_client_screen.right, ",",
        view_client_screen.bottom, ") physical_bottom_inset=",
        physical_bottom_inset_);
  } else {
    show_request_in_flight_ = false;
    physical_bottom_inset_ = 0.0;
    TraceWindowsTextInput("InputPane", "Hiding physical_bottom_inset=0");
    const uint64_t geometry_generation = ++geometry_generation_;
    task_runner_->PostDelayedTask(
        [weak = weak_factory_.GetWeakPtr(), geometry_generation]() {
          if (weak && geometry_generation == weak->geometry_generation_) {
            weak->RestoreWindowAfterKeyboard();
          }
        },
        kDisplayDismissDebounce);
  }
  if (notify_immediately) {
    NotifyVisibilityChanged();
  }
}

}  // namespace flutter
