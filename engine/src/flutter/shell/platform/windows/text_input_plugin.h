// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_TEXT_INPUT_PLUGIN_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_TEXT_INPUT_PLUGIN_H_

#include <windows.h>

#include <array>
#include <map>
#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/shell/geometry/geometry.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "flutter/shell/platform/common/json_method_codec.h"
#include "flutter/shell/platform/common/text_editing_delta.h"
#include "flutter/shell/platform/common/text_input_model.h"
#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/windows/keyboard_handler_base.h"
#include "flutter/shell/platform/windows/tsf_text_store.h"

namespace flutter {

class FlutterWindowsEngine;
class OnScreenKeyboard;
class TsfBridge;

// Implements a text input plugin.
//
// Specifically handles window events within windows.
class TextInputPlugin : public TsfTextStoreDelegate {
 public:
  // |on_screen_keyboard| and |tsf_bridge| may be null in tests. Ownership
  // remains with the engine.
  TextInputPlugin(flutter::BinaryMessenger* messenger,
                  FlutterWindowsEngine* engine,
                  OnScreenKeyboard* on_screen_keyboard = nullptr,
                  TsfBridge* tsf_bridge = nullptr);

  virtual ~TextInputPlugin();

  // Called when the Flutter engine receives a raw keyboard message.
  virtual void KeyboardHook(int key,
                            int scancode,
                            int action,
                            char32_t character,
                            bool extended,
                            bool was_down);

  // Called when the Flutter engine receives a keyboard character.
  virtual void TextHook(const std::u16string& text);

  // Called on an IME compose begin event.
  //
  // Triggered when the user begins editing composing text using a multi-step
  // input method such as in CJK text input.
  virtual void ComposeBeginHook();

  // Called on an IME compose commit event.
  //
  // Triggered when the user triggers a commit of the current composing text
  // while using a multi-step input method such as in CJK text input. Composing
  // continues with the next keypress.
  virtual void ComposeCommitHook();

  // Called on an IME compose end event.
  //
  // Triggered when the composing ends, for example when the user presses
  // ESC or when the user triggers a commit of the composing text while using a
  // multi-step input method such as in CJK text input.
  virtual void ComposeEndHook();

  // Called on an IME composing region change event.
  //
  // Triggered when the user edits the composing text while using a multi-step
  // input method such as in CJK text input.
  virtual void ComposeChangeHook(const std::u16string& text, int cursor_pos);

  // Called when a view is removed from the engine.
  //
  // If the removed view is the currently active view for text input, dismisses
  // the on-screen keyboard and resets the active model and view id to prevent
  // stale references. The implicit view is excluded from this reset.
  void OnViewRemoved(FlutterViewId view_id);

  // Records the device kind and location of the most recent pointer event.
  //
  // |TextInput.show| requests the on-screen keyboard only for touch or pen.
  // Does not clear InputPane display suppression; that requires a later
  // pointer on the active text field (not AppBar back or other controls).
  //
  // A pointer that misses the active field, with a client still attached,
  // is Chromium TEXT_INPUT_TYPE_NONE: AssociateFocus the HWND to the empty
  // TSF document so OS SIP heuristics stop. Flutter tap-outside does not
  // clearClient.
  void SetLastPointerKind(FlutterPointerDeviceKind device_kind,
                          double x = 0.0,
                          double y = 0.0);

  // Called when the InputPane hides. Does not change TSF: Chromium never
  // updates TSF from InputPane events, and SetFocus on hide re-shows the
  // SIP. The dismiss pointer does not count as a request to show.
  void OnOnScreenKeyboardHidden();

  FlutterPointerDeviceKind last_pointer_kind() const {
    return last_pointer_kind_;
  }

  // The on-screen keyboard, if one was injected.
  OnScreenKeyboard* on_screen_keyboard() const { return on_screen_keyboard_; }

  // |TsfTextStoreDelegate|
  std::u16string GetTsfText() const override;
  TextRange GetTsfSelection() const override;
  void SetTsfSelection(const TextRange& range) override;
  void ReplaceTsfText(const TextRange& range,
                      const std::u16string& text) override;
  void OnTsfComposeBegin() override;
  void OnTsfComposeUpdate(const std::u16string& text, int cursor_pos) override;
  void OnTsfComposeEnd() override;
  Rect GetTsfCaretRect() const override;
  HWND GetTsfWindowHandle() const override;

 private:
  // Allows modifying the TextInputPlugin in tests.
  friend class TextInputPluginModifier;
  friend class EngineModifier;

  // Sends the current state of the given model to the Flutter engine.
  void SendStateUpdate(const TextInputModel& model);

  // Sends the current state of the given model to the Flutter engine.
  void SendStateUpdateWithDelta(const TextInputModel& model,
                                const TextEditingDelta*);

  // Sends an action triggered by the Enter key to the Flutter engine.
  void EnterPressed(TextInputModel* model);

  // Called when a method is called on |channel_|;
  void HandleMethodCall(
      const flutter::MethodCall<rapidjson::Document>& method_call,
      std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result);

  // Returns the composing rect, or if IME composing mode is not active, the
  // cursor rect in the PipelineOwner root coordinate system.
  Rect GetCursorRect() const;

  // HWND of the active text-input view, or null if the view is missing.
  HWND GetClientWindowHandle() const;

  // Whether |hwnd| currently has Win32 focus.
  bool ClientWindowHasFocus(HWND hwnd) const;

  // Requests the on-screen keyboard if a client is attached, the last pointer
  // was touch or pen, and the client HWND has focus.
  void MaybeDisplayOnScreenKeyboard();

  // Dismisses the on-screen keyboard if no text client is attached.
  void MaybeDismissOnScreenKeyboard();

  // Dismisses the on-screen keyboard for the active client view, if any.
  void DismissOnScreenKeyboard();

  // Focuses the TSF editable or non-editable document for the active view.
  void FocusTsfEditable();
  void FocusTsfNonEditable();

  // Focuses the editable TSF document unless display is suppressed and the
  // last pointer was not a tap on the active text field (e.g. AppBar back).
  void FocusTsfEditableIfAllowed();

  bool DisplayIsSuppressed() const;
  void AcceptDisplayAfterGesture();
  bool ShouldUnsuppressForPointer() const;
  bool LastPointerHitsEditableField() const;

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<rapidjson::Document>> channel_;

  // The associated |FlutterWindowsEngine|.
  FlutterWindowsEngine* engine_;

  // The on-screen keyboard used to show and hide the Windows touch keyboard.
  //
  // May be null in tests.
  OnScreenKeyboard* on_screen_keyboard_ = nullptr;

  // TSF IME bridge. May be null in tests or when TSF is unavailable.
  TsfBridge* tsf_bridge_ = nullptr;

  // Device kind of the last pointer event. Mouse/unknown does not Display.
  FlutterPointerDeviceKind last_pointer_kind_ = kFlutterPointerDeviceKindMouse;

  // False after a user SIP dismiss until the next pointer down. Prevents the
  // dismiss tap and stale setClient/show from restoring an editable TSF
  // document or calling TryShow.
  bool pointer_since_dismiss_ = true;

  // Last pointer-down location, in physical view pixels.
  double last_pointer_x_ = 0.0;
  double last_pointer_y_ = 0.0;

  // Size of the active EditableText, in local logical pixels. Updated via
  // TextInput.setEditableSizeAndTransform.
  double editable_width_ = 0.0;
  double editable_height_ = 0.0;

  // When true, |ClientWindowHasFocus| uses |window_has_focus_override_|
  // instead of GetFocus(). Tests set this via TextInputPluginModifier.
  bool has_window_focus_override_ = false;
  bool window_has_focus_override_ = false;

  // The active client id.
  int client_id_;

  // The active view id.
  FlutterViewId view_id_ = 0;

  // The active model. nullptr if not set.
  std::unique_ptr<TextInputModel> active_model_;

  // Whether to enable that the engine sends text input updates to the framework
  // as TextEditingDeltas or as one TextEditingValue.
  // For more information on the delta model, see:
  // https://master-api.flutter.dev/flutter/services/TextInputConfiguration/enableDeltaModel.html
  bool enable_delta_model = false;

  // Keyboard type of the client. See available options:
  // https://api.flutter.dev/flutter/services/TextInputType-class.html
  std::string input_type_;

  // An action requested by the user on the input client. See available options:
  // https://api.flutter.dev/flutter/services/TextInputAction-class.html
  std::string input_action_;

  // The smallest rect, in local coordinates, of the text in the composing
  // range, or of the caret in the case where there is no current composing
  // range. This value is updated via `TextInput.setMarkedTextRect` messages
  // over the text input channel.
  Rect composing_rect_;

  // A 4x4 matrix that maps from `EditableText` local coordinates to the
  // coordinate system of `PipelineOwner.rootNode`.
  std::array<std::array<double, 4>, 4> editabletext_transform_ = {
      0.0, 0.0, 0.0, 0.0,  //
      0.0, 0.0, 0.0, 0.0,  //
      0.0, 0.0, 0.0, 0.0,  //
      0.0, 0.0, 0.0, 0.0};

  FML_DISALLOW_COPY_AND_ASSIGN(TextInputPlugin);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_TEXT_INPUT_PLUGIN_H_
