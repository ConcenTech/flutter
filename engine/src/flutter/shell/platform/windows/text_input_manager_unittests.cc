// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/text_input_manager.h"

#include <windows.h>
#include <imm.h>

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {

class TextInputManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hwnd_ = CreateWindowEx(WS_EX_NOACTIVATE, L"STATIC", L"osk_caret_test",
                           WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, nullptr,
                           nullptr, GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd_, nullptr);
    manager_.SetWindowHandle(hwnd_);
    DestroyCaret();
  }

  void TearDown() override {
    DestroyCaret();
    if (hwnd_ != nullptr) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  HWND hwnd_ = nullptr;
  TextInputManager manager_;
};

}  // namespace

TEST_F(TextInputManagerTest, FocusDoesNotCreateCaretWithoutClient) {
  manager_.OnWindowFocusChanged(true);
  EXPECT_FALSE(manager_.caret_created());
}

TEST_F(TextInputManagerTest, CaretRectCreatesCaret) {
  manager_.UpdateCaretRect(Rect{Point(10, 20), Size(5, 15)});
  EXPECT_TRUE(manager_.text_client_attached());
  EXPECT_TRUE(manager_.caret_created());
}

TEST_F(TextInputManagerTest, AbortComposingDestroysCaret) {
  manager_.UpdateCaretRect(Rect{Point(10, 20), Size(5, 15)});
  ASSERT_TRUE(manager_.caret_created());

  manager_.AbortComposing();

  EXPECT_FALSE(manager_.text_client_attached());
  EXPECT_FALSE(manager_.ime_active());
  EXPECT_FALSE(manager_.caret_created());
}

TEST_F(TextInputManagerTest, EndCompositionKeepsCaretWhileClientAttached) {
  manager_.UpdateCaretRect(Rect{Point(10, 20), Size(5, 15)});
  manager_.CreateImeWindow();
  ASSERT_TRUE(manager_.ime_active());
  ASSERT_TRUE(manager_.caret_created());

  manager_.DestroyImeWindow();

  EXPECT_FALSE(manager_.ime_active());
  EXPECT_TRUE(manager_.text_client_attached());
  EXPECT_TRUE(manager_.caret_created());
}

TEST_F(TextInputManagerTest, EndCompositionDestroysCaretWithoutClient) {
  manager_.CreateImeWindow();
  ASSERT_TRUE(manager_.ime_active());
  ASSERT_TRUE(manager_.caret_created());

  manager_.DestroyImeWindow();

  EXPECT_FALSE(manager_.ime_active());
  EXPECT_FALSE(manager_.caret_created());
}

TEST_F(TextInputManagerTest, UnfocusDestroysCaretAndFocusRestoresIfClient) {
  manager_.UpdateCaretRect(Rect{Point(10, 20), Size(5, 15)});
  ASSERT_TRUE(manager_.caret_created());

  manager_.OnWindowFocusChanged(false);
  EXPECT_FALSE(manager_.caret_created());
  EXPECT_TRUE(manager_.text_client_attached());

  manager_.OnWindowFocusChanged(true);
  EXPECT_TRUE(manager_.caret_created());
}

TEST_F(TextInputManagerTest, AbortComposingDetachesImeContext) {
  manager_.UpdateCaretRect(Rect{Point(10, 20), Size(5, 15)});
  {
    HIMC before = ImmGetContext(hwnd_);
    EXPECT_NE(before, nullptr);
    if (before) {
      ImmReleaseContext(hwnd_, before);
    }
  }

  manager_.AbortComposing();

  HIMC after = ImmGetContext(hwnd_);
  EXPECT_EQ(after, nullptr);
  if (after) {
    ImmReleaseContext(hwnd_, after);
  }
}

TEST_F(TextInputManagerTest, CaretRectRestoresImeContext) {
  manager_.AbortComposing();
  manager_.UpdateCaretRect(Rect{Point(10, 20), Size(5, 15)});

  HIMC context = ImmGetContext(hwnd_);
  EXPECT_NE(context, nullptr);
  if (context) {
    ImmReleaseContext(hwnd_, context);
  }
}

}  // namespace testing
}  // namespace flutter
