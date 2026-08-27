// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/tsf_text_store.h"

#include <wrl/client.h>
#include <wrl/implements.h>

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {

class FakeTsfDelegate : public TsfTextStoreDelegate {
 public:
  std::u16string GetTsfText() const override { return text_; }
  TextRange GetTsfSelection() const override { return selection_; }
  void SetTsfSelection(const TextRange& range) override { selection_ = range; }
  void ReplaceTsfText(const TextRange& range,
                      const std::u16string& text) override {
    text_ = text;
    selection_ = TextRange(text.size());
  }
  void OnTsfComposeBegin() override { composing_ = true; }
  void OnTsfComposeUpdate(const std::u16string& text, int cursor_pos) override {
    composing_text_ = text;
    cursor_pos_ = cursor_pos;
  }
  void OnTsfComposeEnd() override { composing_ = false; }
  Rect GetTsfCaretRect() const override { return Rect({0, 0}, Size(1, 1)); }
  HWND GetTsfWindowHandle() const override { return nullptr; }

  std::u16string text_;
  TextRange selection_{0};
  bool composing_ = false;
  std::u16string composing_text_;
  int cursor_pos_ = 0;
};

}  // namespace

TEST(TsfTextStoreTest, GetStatusSetsManualInputPaneFlag) {
  FakeTsfDelegate delegate;
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr =
      Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, &delegate);
  ASSERT_EQ(hr, S_OK);
  ASSERT_TRUE(store);

  TS_STATUS status{};
  EXPECT_EQ(store->GetStatus(&status), S_OK);
  EXPECT_NE(status.dwDynamicFlags & TS_SD_INPUTPANEMANUALDISPLAYENABLE, 0u);
  EXPECT_NE(status.dwStaticFlags & TS_SS_NOHIDDENTEXT, 0u);
  EXPECT_NE(status.dwStaticFlags & TS_SS_TRANSITORY, 0u);
}

TEST(TsfTextStoreTest, GetStatusManualFlagIsDefault) {
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, nullptr);
  ASSERT_EQ(hr, S_OK);

  TS_STATUS status{};
  EXPECT_EQ(store->GetStatus(&status), S_OK);
  EXPECT_EQ(status.dwDynamicFlags,
            static_cast<DWORD>(TS_SD_INPUTPANEMANUALDISPLAYENABLE));
}

TEST(TsfTextStoreTest, GetStatusRejectsNull) {
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, nullptr);
  ASSERT_EQ(hr, S_OK);
  EXPECT_EQ(store->GetStatus(nullptr), E_INVALIDARG);
}

TEST(TsfTextStoreTest, EmptyStoreGetStatusSetsReadonly) {
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, nullptr);
  ASSERT_EQ(hr, S_OK);
  store->UseEmptyTextStore(true);

  TS_STATUS status{};
  EXPECT_EQ(store->GetStatus(&status), S_OK);
  EXPECT_NE(status.dwDynamicFlags & TS_SD_INPUTPANEMANUALDISPLAYENABLE, 0u);
  EXPECT_NE(status.dwDynamicFlags & TS_SD_READONLY, 0u);
  EXPECT_NE(status.dwStaticFlags & TS_SS_NOHIDDENTEXT, 0u);
  EXPECT_NE(status.dwStaticFlags & TS_SS_TRANSITORY, 0u);
}

TEST(TsfTextStoreTest, EmptyStoreRequestLockFails) {
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, nullptr);
  ASSERT_EQ(hr, S_OK);
  store->UseEmptyTextStore(true);

  HRESULT session = S_OK;
  EXPECT_EQ(store->RequestLock(TS_LF_READ, &session), E_FAIL);
  EXPECT_EQ(session, E_FAIL);
}

TEST(TsfTextStoreTest, RequestLockWithoutDelegateFails) {
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, nullptr);
  ASSERT_EQ(hr, S_OK);

  HRESULT session = S_OK;
  EXPECT_EQ(store->RequestLock(TS_LF_READ, &session), E_UNEXPECTED);
  EXPECT_EQ(session, E_FAIL);
}

TEST(TsfTextStoreTest, GetWndWithoutDelegateIsNull) {
  Microsoft::WRL::ComPtr<TsfTextStore> store;
  HRESULT hr = Microsoft::WRL::MakeAndInitialize<TsfTextStore>(&store, nullptr);
  ASSERT_EQ(hr, S_OK);
  HWND hwnd = reinterpret_cast<HWND>(1);
  TsViewCookie view = 0;
  EXPECT_EQ(store->GetActiveView(&view), S_OK);
  EXPECT_EQ(store->GetWnd(view, &hwnd), S_OK);
  EXPECT_EQ(hwnd, nullptr);
}

}  // namespace testing
}  // namespace flutter
