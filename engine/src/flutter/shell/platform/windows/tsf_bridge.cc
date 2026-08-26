// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/tsf_bridge.h"

#include "flutter/fml/logging.h"

namespace flutter {

namespace {

using TF_CreateThreadMgrFn = HRESULT(WINAPI*)(ITfThreadMgr**);

TF_CreateThreadMgrFn LoadCreateThreadMgr() {
  HMODULE module = LoadLibraryW(L"msctf.dll");
  if (!module) {
    return nullptr;
  }
  return reinterpret_cast<TF_CreateThreadMgrFn>(
      GetProcAddress(module, "TF_CreateThreadMgr"));
}

void LogTsfFailure(const char* api, HRESULT hr) {
  FML_LOG(WARNING) << "TSF " << api << " failed: 0x" << std::hex
                   << static_cast<unsigned long>(hr);
}

}  // namespace

TsfBridgeWin::TsfBridgeWin() {
  Initialize();
}

TsfBridgeWin::~TsfBridgeWin() {
  DestroyCaretIfNeeded();
  if (editable_document_mgr_) {
    editable_document_mgr_->Pop(TF_POPF_ALL);
  }
  if (thread_mgr_) {
    thread_mgr_->Deactivate();
  }
}

bool TsfBridgeWin::available() const {
  return available_;
}

bool TsfBridgeWin::Initialize() {
  TF_CreateThreadMgrFn create_thread_mgr = LoadCreateThreadMgr();
  if (!create_thread_mgr) {
    LogTsfFailure("LoadLibrary(msctf.dll)",
                  HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
    return false;
  }

  HRESULT hr = create_thread_mgr(&thread_mgr_);
  if (FAILED(hr) || !thread_mgr_) {
    LogTsfFailure("TF_CreateThreadMgr", hr);
    thread_mgr_.Reset();
    return false;
  }

  hr = thread_mgr_->Activate(&client_id_);
  if (FAILED(hr)) {
    LogTsfFailure("ITfThreadMgr::Activate", hr);
    thread_mgr_.Reset();
    return false;
  }

  hr = thread_mgr_->CreateDocumentMgr(&empty_document_mgr_);
  if (FAILED(hr) || !empty_document_mgr_) {
    LogTsfFailure("CreateDocumentMgr(empty)", hr);
    thread_mgr_->Deactivate();
    thread_mgr_.Reset();
    return false;
  }

  hr = thread_mgr_->CreateDocumentMgr(&editable_document_mgr_);
  if (FAILED(hr) || !editable_document_mgr_) {
    LogTsfFailure("CreateDocumentMgr(editable)", hr);
    thread_mgr_->Deactivate();
    thread_mgr_.Reset();
    return false;
  }

  hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&text_store_, nullptr);
  if (FAILED(hr) || !text_store_) {
    LogTsfFailure("TsfTextStore", hr);
    thread_mgr_->Deactivate();
    thread_mgr_.Reset();
    return false;
  }

  hr = editable_document_mgr_->CreateContext(
      client_id_, 0, static_cast<ITextStoreACP*>(text_store_.Get()),
      &editable_context_, &edit_cookie_);
  if (FAILED(hr) || !editable_context_) {
    LogTsfFailure("CreateContext", hr);
    thread_mgr_->Deactivate();
    thread_mgr_.Reset();
    return false;
  }

  hr = editable_document_mgr_->Push(editable_context_.Get());
  if (FAILED(hr)) {
    LogTsfFailure("Push", hr);
    thread_mgr_->Deactivate();
    thread_mgr_.Reset();
    return false;
  }

  available_ = true;
  return true;
}

void TsfBridgeWin::CreateCaretIfNeeded(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) {
    return;
  }
  ::CreateCaret(hwnd, nullptr, 1, 1);
  caret_created_ = true;
}

void TsfBridgeWin::DestroyCaretIfNeeded() {
  if (caret_created_) {
    ::DestroyCaret();
    caret_created_ = false;
  }
}

void TsfBridgeWin::FocusEditable(HWND hwnd, TsfTextStoreDelegate* delegate) {
  if (!available_ || hwnd == nullptr) {
    return;
  }
  if (text_store_) {
    text_store_->SetDelegate(delegate);
  }
  Microsoft::WRL::ComPtr<ITfDocumentMgr> previous;
  HRESULT hr = thread_mgr_->AssociateFocus(hwnd, editable_document_mgr_.Get(),
                                           &previous);
  if (FAILED(hr)) {
    LogTsfFailure("AssociateFocus(editable)", hr);
  }
  hr = thread_mgr_->SetFocus(editable_document_mgr_.Get());
  if (FAILED(hr)) {
    LogTsfFailure("SetFocus(editable)", hr);
  }
  associated_hwnd_ = hwnd;
  CreateCaretIfNeeded(hwnd);
}

void TsfBridgeWin::FocusNonEditable(HWND hwnd) {
  if (!available_) {
    return;
  }
  if (text_store_) {
    text_store_->SetDelegate(nullptr);
  }
  DestroyCaretIfNeeded();
  if (hwnd == nullptr) {
    associated_hwnd_ = nullptr;
    return;
  }
  Microsoft::WRL::ComPtr<ITfDocumentMgr> previous;
  // Win10 path: associate a document manager with no context.
  HRESULT hr =
      thread_mgr_->AssociateFocus(hwnd, empty_document_mgr_.Get(), &previous);
  if (FAILED(hr)) {
    LogTsfFailure("AssociateFocus(empty)", hr);
  }
  associated_hwnd_ = hwnd;
}

void TsfBridgeWin::AbortComposition() {
  if (!available_ || !editable_context_) {
    return;
  }
  Microsoft::WRL::ComPtr<ITfContextOwnerCompositionServices> composition;
  HRESULT hr = editable_context_.As(&composition);
  if (FAILED(hr) || !composition) {
    return;
  }
  composition->TerminateComposition(nullptr);
}

void TsfBridgeWin::NotifyTextChanged() {
  if (text_store_) {
    text_store_->NotifyTextChanged();
  }
}

void TsfBridgeWin::NotifySelectionChanged() {
  if (text_store_) {
    text_store_->NotifySelectionChanged();
  }
}

void TsfBridgeWin::NotifyLayoutChanged() {
  if (text_store_) {
    text_store_->NotifyLayoutChanged();
  }
}

}  // namespace flutter
