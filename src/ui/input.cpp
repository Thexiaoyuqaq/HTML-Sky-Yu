#include <windows.h>
#include <atomic>
#include <mutex>
#include "imgui.h"

#include "htinternal.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
  HWND hWnd,
  UINT msg,
  WPARAM wParam,
  LPARAM lParam);

// Original window process of the game.
static WNDPROC gWndProcOrigin = nullptr;

// ----------------------------------------------------------------------------
// [SECTION] Deferred window message queue.
//
// ImGui is strictly single-threaded. io.AddMouse*Event() / io.AddKeyEvent()
// append to g.InputEventsQueue, which ImGui::NewFrame() drains and clears. Our
// window process runs on the game's message thread, while ImGui::NewFrame() runs
// on the render thread inside the present hook, so calling into ImGui from the
// window process races with that drain - the vector can be reallocated (or
// freed) while the render thread is walking it.
//
// The window process therefore only records the raw message here, and the render
// thread replays it through imgui_impl_win32 right before the frame starts.
// Replaying the original message keeps all of ImGui's own handling - the
// TrackMouseEvent bookkeeping behind MouseTrackedArea, the mouse source, cursor
// shapes, key mapping - instead of duplicating a subset of it, which is what the
// previous hand-written mouse handler did (and it forgot to update
// bd->MouseTrackedArea, so imgui_impl_win32 fell back to GetCursorPos() +
// ScreenToClient() on every single frame).
//
// Coalescing also bounds the per-frame work: consecutive motion messages
// collapse on the window thread, so a 1000 Hz mouse delivers at most one motion
// message per rendered frame instead of a thousand.
// ----------------------------------------------------------------------------

#define HT_INPUT_QUEUE_CAPACITY 256

struct HTInputMsg {
  UINT msg;
  WPARAM wParam;
  LPARAM lParam;
};

static HTInputMsg gInputQueue[HT_INPUT_QUEUE_CAPACITY];
static u32 gInputQueueCount = 0;
static std::mutex gInputQueueMutex;
// Set once the ImGui context exists, i.e. by HTiInstallInputHook(). Also tells
// HTWndProc that the capture flags in gui.cpp are meaningful.
static std::atomic<bool> gInputReady{false};

static bool isMouseMotionMsg(UINT msg) {
  return msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE;
}

// Messages ImGui has to see. Anything else is irrelevant to it and is forwarded
// to the game untouched.
static bool isImGuiInputMsg(UINT msg) {
  switch (msg) {
    case WM_MOUSEMOVE: case WM_NCMOUSEMOVE:
    case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SYSCHAR:
    case WM_SETFOCUS: case WM_KILLFOCUS:
      return true;
    default:
      return false;
  }
}

// Motion carries no information beyond "latest state", so consecutive motion
// messages collapse into the newest sample. Messages in between are never
// dropped, so ordering against clicks is preserved - a click has to be applied
// with the cursor position that was current when it happened.
static void queueInputMsg(
  UINT msg,
  WPARAM wParam,
  LPARAM lParam
) {
  if (!gInputReady.load(std::memory_order_relaxed))
    return;
  if (!isImGuiInputMsg(msg))
    return;

  std::lock_guard<std::mutex> lock(gInputQueueMutex);

  if (gInputQueueCount && isMouseMotionMsg(msg)
      && isMouseMotionMsg(gInputQueue[gInputQueueCount - 1].msg))
    gInputQueueCount--;

  if (gInputQueueCount >= HT_INPUT_QUEUE_CAPACITY)
    // The queue is drained on every rendered frame, so this only happens while
    // the overlay is skipped for a long stretch. Dropping the tail keeps the
    // newest state, which is what the UI actually cares about.
    return;

  gInputQueue[gInputQueueCount].msg = msg;
  gInputQueue[gInputQueueCount].wParam = wParam;
  gInputQueue[gInputQueueCount].lParam = lParam;
  gInputQueueCount++;
}

void HTiPumpInput() {
  static HTInputMsg local[HT_INPUT_QUEUE_CAPACITY];
  u32 count;
  bool sawMotion = false;

  {
    std::lock_guard<std::mutex> lock(gInputQueueMutex);
    count = gInputQueueCount;
    for (u32 i = 0; i < count; i++)
      local[i] = gInputQueue[i];
    gInputQueueCount = 0;
  }

  HTiDiagCountPumped(count);

  for (u32 i = 0; i < count; i++) {
    if (isMouseMotionMsg(local[i].msg))
      sawMotion = true;
    (void)ImGui_ImplWin32_WndProcHandler(
      gGameStatus.window,
      local[i].msg,
      local[i].wParam,
      local[i].lParam);
  }

  // Cursor fallback.
  //
  // imgui_impl_win32 only refreshes the cursor from GetCursorPos() while it is
  // not tracking the mouse (bd->MouseTrackedArea == 0). Replaying the messages
  // through it keeps that bookkeeping up to date, which means that on a frame
  // with no motion message the position would be left wherever the previous
  // message put it. Feed the live position instead, so the overlay cursor tracks
  // the real one even when the game has the mouse captured or clipped and stops
  // producing motion messages.
  if (!sawMotion && gGameStatus.window) {
    POINT pos;
    if (::GetCursorPos(&pos) && ::ScreenToClient(gGameStatus.window, &pos))
      ImGui::GetIO().AddMousePosEvent((f32)pos.x, (f32)pos.y);
  }
}

static bool isVkDown(int vk) {
  return (GetKeyState(vk) & 0x8000) != 0;
}

/**
 * Modified from ImGui. Map VK_* to HTKeyCode_*.
 */
static HTKeyCode vkToInternalKey(
  WPARAM wParam,
  LPARAM lParam
) {
  if ((wParam == VK_RETURN) && (HIWORD(lParam) & KF_EXTENDED))
    return HTKey_KeypadEnter;

  switch (wParam) {
    case VK_TAB: return HTKey_Tab;
    case VK_LEFT: return HTKey_LeftArrow;
    case VK_RIGHT: return HTKey_RightArrow;
    case VK_UP: return HTKey_UpArrow;
    case VK_DOWN: return HTKey_DownArrow;
    case VK_PRIOR: return HTKey_PageUp;
    case VK_NEXT: return HTKey_PageDown;
    case VK_HOME: return HTKey_Home;
    case VK_END: return HTKey_End;
    case VK_INSERT: return HTKey_Insert;
    case VK_DELETE: return HTKey_Delete;
    case VK_BACK: return HTKey_Backspace;
    case VK_SPACE: return HTKey_Space;
    case VK_RETURN: return HTKey_Enter;
    case VK_ESCAPE: return HTKey_Escape;
    case VK_OEM_COMMA: return HTKey_Comma;
    case VK_OEM_PERIOD: return HTKey_Period;
    case VK_CAPITAL: return HTKey_CapsLock;
    case VK_SCROLL: return HTKey_ScrollLock;
    case VK_NUMLOCK: return HTKey_NumLock;
    case VK_SNAPSHOT: return HTKey_PrintScreen;
    case VK_PAUSE: return HTKey_Pause;
    case VK_NUMPAD0: return HTKey_Keypad0;
    case VK_NUMPAD1: return HTKey_Keypad1;
    case VK_NUMPAD2: return HTKey_Keypad2;
    case VK_NUMPAD3: return HTKey_Keypad3;
    case VK_NUMPAD4: return HTKey_Keypad4;
    case VK_NUMPAD5: return HTKey_Keypad5;
    case VK_NUMPAD6: return HTKey_Keypad6;
    case VK_NUMPAD7: return HTKey_Keypad7;
    case VK_NUMPAD8: return HTKey_Keypad8;
    case VK_NUMPAD9: return HTKey_Keypad9;
    case VK_DECIMAL: return HTKey_KeypadDecimal;
    case VK_DIVIDE: return HTKey_KeypadDivide;
    case VK_MULTIPLY: return HTKey_KeypadMultiply;
    case VK_SUBTRACT: return HTKey_KeypadSubtract;
    case VK_ADD: return HTKey_KeypadAdd;
    case VK_LSHIFT: return HTKey_LeftShift;
    case VK_LCONTROL: return HTKey_LeftCtrl;
    case VK_LMENU: return HTKey_LeftAlt;
    case VK_LWIN: return HTKey_LeftSuper;
    case VK_RSHIFT: return HTKey_RightShift;
    case VK_RCONTROL: return HTKey_RightCtrl;
    case VK_RMENU: return HTKey_RightAlt;
    case VK_RWIN: return HTKey_RightSuper;
    case VK_APPS: return HTKey_Menu;
    case '0': return HTKey_0;
    case '1': return HTKey_1;
    case '2': return HTKey_2;
    case '3': return HTKey_3;
    case '4': return HTKey_4;
    case '5': return HTKey_5;
    case '6': return HTKey_6;
    case '7': return HTKey_7;
    case '8': return HTKey_8;
    case '9': return HTKey_9;
    case 'A': return HTKey_A;
    case 'B': return HTKey_B;
    case 'C': return HTKey_C;
    case 'D': return HTKey_D;
    case 'E': return HTKey_E;
    case 'F': return HTKey_F;
    case 'G': return HTKey_G;
    case 'H': return HTKey_H;
    case 'I': return HTKey_I;
    case 'J': return HTKey_J;
    case 'K': return HTKey_K;
    case 'L': return HTKey_L;
    case 'M': return HTKey_M;
    case 'N': return HTKey_N;
    case 'O': return HTKey_O;
    case 'P': return HTKey_P;
    case 'Q': return HTKey_Q;
    case 'R': return HTKey_R;
    case 'S': return HTKey_S;
    case 'T': return HTKey_T;
    case 'U': return HTKey_U;
    case 'V': return HTKey_V;
    case 'W': return HTKey_W;
    case 'X': return HTKey_X;
    case 'Y': return HTKey_Y;
    case 'Z': return HTKey_Z;
    case VK_F1: return HTKey_F1;
    case VK_F2: return HTKey_F2;
    case VK_F3: return HTKey_F3;
    case VK_F4: return HTKey_F4;
    case VK_F5: return HTKey_F5;
    case VK_F6: return HTKey_F6;
    case VK_F7: return HTKey_F7;
    case VK_F8: return HTKey_F8;
    case VK_F9: return HTKey_F9;
    case VK_F10: return HTKey_F10;
    case VK_F11: return HTKey_F11;
    case VK_F12: return HTKey_F12;
    case VK_F13: return HTKey_F13;
    case VK_F14: return HTKey_F14;
    case VK_F15: return HTKey_F15;
    case VK_F16: return HTKey_F16;
    case VK_F17: return HTKey_F17;
    case VK_F18: return HTKey_F18;
    case VK_F19: return HTKey_F19;
    case VK_F20: return HTKey_F20;
    case VK_F21: return HTKey_F21;
    case VK_F22: return HTKey_F22;
    case VK_F23: return HTKey_F23;
    case VK_F24: return HTKey_F24;
    case VK_BROWSER_BACK: return HTKey_AppBack;
    case VK_BROWSER_FORWARD: return HTKey_AppForward;
    default: break;
  }

  i32 scanCode = (i32)LOBYTE(HIWORD(lParam));
  switch (scanCode) {
    case 41: return HTKey_GraveAccent;
    case 12: return HTKey_Minus;
    case 13: return HTKey_Equal;
    case 26: return HTKey_LeftBracket;
    case 27: return HTKey_RightBracket;
    case 86: return HTKey_Oem102;
    case 43: return HTKey_Backslash;
    case 39: return HTKey_Semicolon;
    case 40: return HTKey_Apostrophe;
    case 51: return HTKey_Comma;
    case 52: return HTKey_Period;
    case 53: return HTKey_Slash;
  }

  return HTKey_None;
}

#define HTHotkeyCheck(a, b) \
  ((void)((isVkDown(a) == isKeyDown) && (HTiHotkeyDispatch(b, down | repeat | blockedKey, blocked), 1)))

/**
 * Modified from ImGui. Dispatch key events to registered callbacks.
 */
static void HTHotKeyWndProc(
  HWND hWnd,
  UINT uMsg,
  WPARAM wParam,
  LPARAM lParam,
  u08 *blocked
) {
  HTKeyEventFlags blockedKey = *blocked
    ? HTKeyEventFlags_Blocked
    : HTKeyEventFlags_None;

  switch (uMsg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
      bool isKeyDown = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN);
      if (wParam >= 256)
        return;

      HTKeyCode key = vkToInternalKey(wParam, lParam);
      i32 vk = (i32)wParam;
      HTKeyEventFlags repeat = ((lParam & 0x40000000) && isKeyDown)
        ? HTKeyEventFlags_Repeat
        : HTKeyEventFlags_None;
      HTKeyEventFlags down = isKeyDown
        ? HTKeyEventFlags_Down
        : HTKeyEventFlags_Up;

      if (key == HTKey_PrintScreen && !isKeyDown)
        HTiHotkeyDispatch(key, HTKeyEventFlags_Down | repeat | blockedKey, blocked);
      else if (vk == VK_SHIFT) {
        HTHotkeyCheck(VK_LSHIFT, HTKey_LeftShift);
        HTHotkeyCheck(VK_RSHIFT, HTKey_RightShift);
      } else if (vk == VK_CONTROL) {
        HTHotkeyCheck(VK_LCONTROL, HTKey_LeftCtrl);
        HTHotkeyCheck(VK_RCONTROL, HTKey_RightCtrl);
      } else if (vk == VK_MENU) {
        HTHotkeyCheck(VK_LMENU, HTKey_LeftAlt);
        HTHotkeyCheck(VK_RMENU, HTKey_RightAlt);
      } else if (key != HTKey_None)
        HTiHotkeyDispatch(key, down | repeat | blockedKey, blocked);
      break;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK: {
      HTKeyCode button = HTKey_None;
      if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK)
        button = HTKey_MouseLeft;
      if (uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONDBLCLK)
        button = HTKey_MouseRight;
      if (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONDBLCLK)
        button = HTKey_MouseMiddle;
      if (uMsg == WM_XBUTTONDOWN || uMsg == WM_XBUTTONDBLCLK)
        button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
          ? HTKey_MouseX1
          : HTKey_MouseX2;
      HTiHotkeyDispatch(button, HTKeyEventFlags_Down | blockedKey, blocked);
      break;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      HTKeyCode button = HTKey_None;
      if (uMsg == WM_LBUTTONUP)
        button = HTKey_MouseLeft;
      if (uMsg == WM_RBUTTONUP)
        button = HTKey_MouseRight;
      if (uMsg == WM_MBUTTONUP)
        button = HTKey_MouseMiddle;
      if (uMsg == WM_XBUTTONUP)
        button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
          ? HTKey_MouseX1
          : HTKey_MouseX2;
      HTiHotkeyDispatch(button, HTKeyEventFlags_Up | blockedKey, blocked);
      break;
    }
    case WM_MOUSEWHEEL: {
      f32 mouseDelta = (f32)GET_WHEEL_DELTA_WPARAM(wParam) / (f32)WHEEL_DELTA;
      if (mouseDelta > 0)
        HTiHotkeyDispatch(HTKey_MouseWheelUp, HTKeyEventFlags_Down | blockedKey, blocked);
      else
        HTiHotkeyDispatch(HTKey_MouseWheelDown, HTKeyEventFlags_Down | blockedKey, blocked);
      break;
    }
    case WM_MOUSEHWHEEL: {
      f32 mouseDelta = (f32)GET_WHEEL_DELTA_WPARAM(wParam) / (f32)WHEEL_DELTA;
      if (mouseDelta > 0)
        HTiHotkeyDispatch(HTKey_MouseWheelRight, HTKeyEventFlags_Down | blockedKey, blocked);
      else
        HTiHotkeyDispatch(HTKey_MouseWheelLeft, HTKeyEventFlags_Down | blockedKey, blocked);
      break;
    }
  }
}

/**
 * The window process used to pass window messages to ImGui and block window
 * message delivery to the game.
 */
static LRESULT APIENTRY HTWndProc(
  HWND hWnd,
  UINT uMsg,
  WPARAM wParam,
  LPARAM lParam
) {
  u08 block = 0;
  i64 tStart, tDelegate, tQueue, tHotkey, tEnd;
  LRESULT result;

  tStart = HTiDiagTicks();
  HTiDiagCountMsgType(uMsg);

  // Decide what the game is allowed to see.
  //
  // While one of our windows is hovered, the overlay owns the mouse: motion,
  // buttons, the wheel, the cursor shape. Everything else goes to the game
  // untouched, so outside the overlay the game behaves exactly as it would
  // without the loader.
  //
  // What must NOT happen is setting the cursor from both sides - see the
  // WM_SETCURSOR case, which is where the measured stutter was.
  //
  // The flags are latched once per frame by HTiUpdateGUI(); reading ImGui state
  // here would race with the render thread.
  switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_NCMOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
    case WM_XBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      block = gImGuiWantsMouse.load(std::memory_order_relaxed);
      break;
    case WM_SETCURSOR:
      // Cursor shapes are applied by the thread that owns the window, so this
      // message stays synchronous instead of going through the queue.
      //
      // This is the one message where the loader must stay out of the way unless
      // it actually owns the cursor. imgui_impl_win32 answers WM_SETCURSOR by
      // calling ::SetCursor(::LoadCursor(NULL, IDC_ARROW)), and WM_SETCURSOR
      // arrives once per mouse move. Answering it while the cursor is over the
      // game puts the loader in a cursor fight with the game: we set the cursor,
      // then forwarding the message lets SDL's own WM_SETCURSOR handler set it
      // again, and the two keep invalidating each other. Measured on Sky: about
      // 9 ms per move in this function plus about 16 ms in the game's window
      // process, which is enough to turn every mouse move over the UI into a
      // visible hitch.
      //
      // So: only touch the cursor while one of our windows is hovered, and while
      // it is ours, swallow the message so the game cannot take it back.
      if (gImGuiWantsMouse.load(std::memory_order_relaxed)) {
        (void)ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
        block = 1;
      }
      break;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
    case WM_CHAR:
      block = gImGuiWantsKeyboard.load(std::memory_order_relaxed);
      break;
    default:
      break;
  }

  // Record the message for ImGui. The render thread replays it at the top of the
  // next frame - see the note on the input queue above.
  tDelegate = HTiDiagTicks();
  queueInputMsg(uMsg, wParam, lParam);
  tQueue = HTiDiagTicks();

  // Hotkeys belong to the game and must see every message, blocked or not.
  HTHotKeyWndProc(hWnd, uMsg, wParam, lParam, &block);
  HTiDiagCountWndProcMsg(block);
  tHotkey = HTiDiagTicks();

  HTiDiagAddWndProcSplit(tDelegate - tStart, tQueue - tDelegate, tHotkey - tQueue);

  if (block) {
    HTiDiagAddWndProcTicks(tHotkey - tStart, 0);
    HTiDiagWorstWndProcMsg(tHotkey - tStart, 0, uMsg);
    return 0;
  }

  // Pass the window message to the game.
  result = CallWindowProcW(gWndProcOrigin, hWnd, uMsg, wParam, lParam);
  tEnd = HTiDiagTicks();

  HTiDiagAddWndProcTicks(tHotkey - tStart, tEnd - tHotkey);
  HTiDiagWorstWndProcMsg(tHotkey - tStart, tEnd - tHotkey, uMsg);

  return result;
}

ImGuiKey HTKeyToImGuiKey(HTKeyCode key) {
  if (key >= HTKey_NamedKey_BEGIN && key <= HTKey_Oem102)
    return (ImGuiKey)key;
  if (key >= HTKey_Mouse_BEGIN && key <= HTKey_MouseX2)
    return (ImGuiKey)(key - HTKey_Mouse_BEGIN + ImGuiKey_MouseLeft);
  if (key == HTKey_MouseWheelUp || key == HTKey_MouseWheelDown)
    return ImGuiKey_MouseWheelY;
  if (key == HTKey_MouseWheelLeft || key == HTKey_MouseWheelRight)
    return ImGuiKey_MouseWheelX;
  return ImGuiKey_None;
}

/**
 * Hook the window process of the game.
 */
void HTiInstallInputHook() {
  if (!gGameStatus.window)
    return;

  // The ImGui context already exists here (HTiInitGUI() creates it before
  // calling this), so the queued messages are safe to replay.
  gInputReady.store(true, std::memory_order_relaxed);

  gWndProcOrigin = (WNDPROC)SetWindowLongPtrW(
    gGameStatus.window,
    GWLP_WNDPROC,
    (LONG_PTR)HTWndProc);
}

/**
 * Release the window callback hook of the game.
 */
void HTiUninstallInputHook() {
  gInputReady.store(false, std::memory_order_relaxed);

  if (gGameStatus.window && gWndProcOrigin)
    (void)SetWindowLongPtrW(
      gGameStatus.window,
      GWLP_WNDPROC,
      (LONG_PTR)gWndProcOrigin);
}


