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

TEST(OnScreenKeyboardTest, ComputeBottomInsetEmptyIntersection) {
  RECT client{0, 0, 800, 600};
  RECT occluded{900, 0, 1100, 200};
  EXPECT_EQ(OnScreenKeyboardWin::ComputeBottomInset(client, occluded), 0.0);
}

TEST(OnScreenKeyboardTest, ComputeBottomInsetKeyboardFromBottom) {
  RECT client{0, 0, 800, 600};
  RECT occluded{0, 400, 800, 600};
  EXPECT_EQ(OnScreenKeyboardWin::ComputeBottomInset(client, occluded), 200.0);
}

TEST(OnScreenKeyboardTest, ComputeBottomInsetClampedToClientHeight) {
  RECT client{0, 0, 800, 600};
  RECT occluded{0, -100, 800, 700};
  EXPECT_EQ(OnScreenKeyboardWin::ComputeBottomInset(client, occluded), 600.0);
}

TEST(OnScreenKeyboardTest, OccludedDipAtScale1IsUnchangedPhysical) {
  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  POINT origin{0, 0};
  RECT screen = OnScreenKeyboardWin::OccludedDipToPhysicalScreenRect(
      occluded, 1.0, origin);
  EXPECT_EQ(screen.left, 0);
  EXPECT_EQ(screen.top, 400);
  EXPECT_EQ(screen.right, 800);
  EXPECT_EQ(screen.bottom, 600);
}

TEST(OnScreenKeyboardTest, OccludedDipAtScale15IsPhysicalPixels) {
  // 200 DIP occlusion at 150% scale is 300 physical px, not 200.
  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  POINT origin{0, 0};
  RECT screen = OnScreenKeyboardWin::OccludedDipToPhysicalScreenRect(
      occluded, 1.5, origin);
  EXPECT_EQ(screen.left, 0);
  EXPECT_EQ(screen.top, 600);
  EXPECT_EQ(screen.right, 1200);
  EXPECT_EQ(screen.bottom, 900);
}

TEST(OnScreenKeyboardTest, OccludedDipAtScale2IsPhysicalPixels) {
  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  POINT origin{0, 0};
  RECT screen = OnScreenKeyboardWin::OccludedDipToPhysicalScreenRect(
      occluded, 2.0, origin);
  EXPECT_EQ(screen.left, 0);
  EXPECT_EQ(screen.top, 800);
  EXPECT_EQ(screen.right, 1600);
  EXPECT_EQ(screen.bottom, 1200);
}

TEST(OnScreenKeyboardTest, BottomInsetAtScale1) {
  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  POINT origin{0, 0};
  RECT view_client{0, 0, 800, 600};
  EXPECT_EQ(OnScreenKeyboardWin::ComputePhysicalBottomInset(
                occluded, 1.0, origin, view_client),
            200.0);
}

TEST(OnScreenKeyboardTest, BottomInsetAtScale15) {
  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  POINT origin{100, 50};
  RECT view_client{100, 50, 1300, 950};
  EXPECT_EQ(OnScreenKeyboardWin::ComputePhysicalBottomInset(
                occluded, 1.5, origin, view_client),
            300.0);
}

TEST(OnScreenKeyboardTest, ShowingUpdatesInsetWithoutTryHide) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);

  bool shown = false;
  double inset = -1.0;
  keyboard.SetVisibilityChangedCallback(
      [&shown, &inset](bool is_shown, double physical_bottom_inset) {
        shown = is_shown;
        inset = physical_bottom_inset;
      });

  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  RECT view_client{0, 0, 800, 600};
  keyboard.HandleVisibilityEvent(true, occluded, 1.0, POINT{0, 0}, view_client);

  EXPECT_TRUE(shown);
  EXPECT_EQ(inset, 200.0);
  EXPECT_TRUE(keyboard.shown());
  EXPECT_EQ(keyboard.physical_bottom_inset(), 200.0);
  EXPECT_TRUE(applies.empty());
}

TEST(OnScreenKeyboardTest, HidingClearsInset) {
  MockTaskRunner runner;
  std::vector<ApplyCall> applies;
  RecordingOnScreenKeyboard keyboard(&runner, &applies);

  OnScreenKeyboardWin::DipRect occluded{0, 400, 800, 200};
  RECT view_client{0, 0, 800, 600};
  keyboard.HandleVisibilityEvent(true, occluded, 1.0, POINT{0, 0}, view_client);
  keyboard.HandleVisibilityEvent(false, OnScreenKeyboardWin::DipRect{}, 1.0,
                                 POINT{0, 0}, RECT{});

  EXPECT_FALSE(keyboard.shown());
  EXPECT_EQ(keyboard.physical_bottom_inset(), 0.0);
  EXPECT_TRUE(applies.empty());
}

TEST(OnScreenKeyboardTest, InvalidHwndDoesNotCrash) {
  MockTaskRunner runner;
  OnScreenKeyboardWin keyboard(&runner);
  bool called = false;
  keyboard.SetVisibilityChangedCallback(
      [&called](bool, double) { called = true; });

  keyboard.Display(DummyHwnd());
  runner.AdvanceTime(OnScreenKeyboardWin::kDisplayDismissDebounce);
  runner.SimulateTimerAwake();

  EXPECT_FALSE(keyboard.shown());
  EXPECT_EQ(keyboard.physical_bottom_inset(), 0.0);
  EXPECT_FALSE(called);
}

}  // namespace testing
}  // namespace flutter
