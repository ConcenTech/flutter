// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/on_screen_keyboard.h"

#include <chrono>
#include <memory>
#include <vector>

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/windows/task_runner.h"
#include "flutter/shell/platform/windows/testing/mock_on_screen_keyboard.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {

HWND DummyHwnd() {
  return reinterpret_cast<HWND>(1);
}

class MockTaskRunner : public TaskRunner {
 public:
  MockTaskRunner()
      : TaskRunner([]() -> uint64_t { return 10000; },
                   [](const FlutterTask*) {}) {}

  void SimulateTimerAwake() { ProcessTasks(); }

  void AdvanceTime(std::chrono::milliseconds delay) { current_time_ += delay; }

 protected:
  void WakeUp() override {}

  TaskTimePoint GetCurrentTimeForTask() const override { return current_time_; }

 private:
  TaskTimePoint current_time_ = TaskTimePoint(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::nanoseconds(10000)));

  FML_DISALLOW_COPY_AND_ASSIGN(MockTaskRunner);
};

struct ApplyCall {
  HWND hwnd;
  bool show;
};

class RecordingOnScreenKeyboard : public OnScreenKeyboardWin {
 public:
  RecordingOnScreenKeyboard(TaskRunner* task_runner,
                            std::vector<ApplyCall>* applies)
      : OnScreenKeyboardWin(task_runner), applies_(applies) {}

 protected:
  void ApplyVisibility(HWND hwnd, bool show) override {
    applies_->push_back(ApplyCall{hwnd, show});
  }

 private:
  std::vector<ApplyCall>* applies_;

  FML_DISALLOW_COPY_AND_ASSIGN(RecordingOnScreenKeyboard);
};

}  // namespace

TEST(OnScreenKeyboardTest, NullHwndIsNoOp) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);

  keyboard.Display(nullptr);
  keyboard.Dismiss(nullptr);
  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  EXPECT_TRUE(applies.empty());
}

TEST(OnScreenKeyboardTest, DisplayAppliesAfterDebounce) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);
  HWND hwnd = DummyHwnd();

  keyboard.Display(hwnd);
  runner.SimulateTimerAwake();
  EXPECT_TRUE(applies.empty());

  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  ASSERT_EQ(applies.size(), 1u);
  EXPECT_EQ(applies[0].hwnd, hwnd);
  EXPECT_TRUE(applies[0].show);
}

TEST(OnScreenKeyboardTest, DisplayThenDismissCoalescesToHide) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);
  HWND hwnd = DummyHwnd();

  keyboard.Display(hwnd);
  keyboard.Dismiss(hwnd);
  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  ASSERT_EQ(applies.size(), 1u);
  EXPECT_EQ(applies[0].hwnd, hwnd);
  EXPECT_FALSE(applies[0].show);
}

TEST(OnScreenKeyboardTest, DismissThenDisplayCoalescesToShow) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);
  HWND hwnd = DummyHwnd();

  keyboard.Dismiss(hwnd);
  keyboard.Display(hwnd);
  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  ASSERT_EQ(applies.size(), 1u);
  EXPECT_EQ(applies[0].hwnd, hwnd);
  EXPECT_TRUE(applies[0].show);
}

TEST(OnScreenKeyboardTest, DestroyBeforeDebounceDoesNotApply) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  HWND hwnd = DummyHwnd();

  {
    auto keyboard =
        std::make_unique<RecordingOnScreenKeyboard>(&runner, &applies);
    keyboard->Display(hwnd);
  }

  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  EXPECT_TRUE(applies.empty());
}

TEST(OnScreenKeyboardTest, StubReportsHidden) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);

  EXPECT_FALSE(keyboard.shown());
  EXPECT_EQ(keyboard.physical_bottom_inset(), 0.0);

  bool called = false;
  keyboard.SetVisibilityChangedCallback(
      [&called](bool, double) { called = true; });
  keyboard.Display(DummyHwnd());
  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  EXPECT_FALSE(called);
}

TEST(OnScreenKeyboardTest, MockCanBeConstructed) {
  MockOnScreenKeyboard keyboard;
  EXPECT_CALL(keyboard, shown()).WillOnce(::testing::Return(false));
  EXPECT_FALSE(keyboard.shown());
}

}  // namespace testing
}  // namespace flutter
