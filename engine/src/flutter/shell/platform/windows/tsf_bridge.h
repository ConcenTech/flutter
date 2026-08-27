// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_TSF_BRIDGE_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_TSF_BRIDGE_H_

#include <msctf.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/windows/tsf_text_store.h"

namespace flutter {

// Owns the TSF thread manager and the editable / non-editable documents.
//
// Non-editable focus matches Chromium TEXT_INPUT_TYPE_NONE:
// - Windows 10: a document manager with no context.
// - Windows 11: a dummy ITextStoreACP with KEYBOARD_DISABLED and
//   EMPTYCONTEXT compartments, TS_SD_READONLY, and denied RequestLock.
// Leaving an editor always SetFocuses the empty document so OS SIP
// heuristics do not keep treating the HWND as an editor.
class TsfBridge {
 public:
  virtual ~TsfBridge() = default;

  // True if TSF and COM initialized successfully.
  virtual bool available() const = 0;

  // Focuses the editable text store on |hwnd|. No-op if TSF is unavailable
  // or |hwnd| is null.
  virtual void FocusEditable(HWND hwnd, TsfTextStoreDelegate* delegate) = 0;

  // Associates the non-editable document with |hwnd| and SetFocuses it.
  // No-op if TSF is unavailable. Null |hwnd| still SetFocuses the empty
  // document so OS SIP heuristics stop treating the HWND as an editor.
  virtual void FocusNonEditable(HWND hwnd) = 0;

  // Aborts any active TSF composition.
  virtual void AbortComposition() = 0;

  // Pushes Flutter-side text/selection/layout changes to TSF.
  virtual void NotifyTextChanged() = 0;
  virtual void NotifySelectionChanged() = 0;
  virtual void NotifyLayoutChanged() = 0;
};

// Default TSF bridge. Resolves TF_CreateThreadMgr from msctf.dll. Fails
// soft if COM is MTA or uninitialized (CO_E_NOTINITIALIZED).
class TsfBridgeWin : public TsfBridge {
 public:
  TsfBridgeWin();
  ~TsfBridgeWin() override;

  // |TsfBridge|
  bool available() const override;
  void FocusEditable(HWND hwnd, TsfTextStoreDelegate* delegate) override;
  void FocusNonEditable(HWND hwnd) override;
  void AbortComposition() override;
  void NotifyTextChanged() override;
  void NotifySelectionChanged() override;
  void NotifyLayoutChanged() override;

  // Exposed for tests that construct a store without the thread manager.
  TsfTextStore* text_store() const { return text_store_.Get(); }

 private:
  bool Initialize();
  void MaybeInitializeEmptyTextStore();
  HRESULT InitializeDisabledContext(ITfContext* context);

  Microsoft::WRL::ComPtr<ITfThreadMgr> thread_mgr_;
  Microsoft::WRL::ComPtr<ITfDocumentMgr> empty_document_mgr_;
  Microsoft::WRL::ComPtr<ITfContext> empty_context_;
  Microsoft::WRL::ComPtr<TsfTextStore> empty_text_store_;
  Microsoft::WRL::ComPtr<ITfDocumentMgr> editable_document_mgr_;
  Microsoft::WRL::ComPtr<ITfContext> editable_context_;
  Microsoft::WRL::ComPtr<TsfTextStore> text_store_;
  TfClientId client_id_ = TF_CLIENTID_NULL;
  TfEditCookie edit_cookie_ = TF_INVALID_EDIT_COOKIE;
  TfEditCookie empty_edit_cookie_ = TF_INVALID_EDIT_COOKIE;
  HWND associated_hwnd_ = nullptr;
  bool available_ = false;

  FML_DISALLOW_COPY_AND_ASSIGN(TsfBridgeWin);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_TSF_BRIDGE_H_
