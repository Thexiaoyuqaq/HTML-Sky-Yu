// ----------------------------------------------------------------------------
// Runtime diagnostics for HT's Mod Loader.
//
// Everything here is driven from html-config.json's "ht_mod_loader" section so a
// deployment can be re-configured without rebuilding:
//
//   "profile": true             log frame-time statistics to the loader log
//   "disable_overlay": true     never render the ImGui overlay
//   "disable_input_hook": true  leave the game's window process alone
//
// The last two exist to bisect a performance problem: they split "the loader
// renders an overlay" from "the loader touches the game's input", which are the
// only two ways HTML can affect a frame.
//
// The counters are relaxed atomics and are always maintained - a few nanoseconds
// per window message. The timing code only runs when "profile" is set.
// ----------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <atomic>
#include "htinternal.hpp"

bool gConfigProfile = false;
bool gConfigDisableOverlay = false;
bool gConfigDisableInputHook = false;

// ----------------------------------------------------------------------------
// [SECTION] Window message counters.
//
// Written on the game's message thread, read on the render thread. A statistic
// only needs a relaxed load/store, so no synchronization is required.
// ----------------------------------------------------------------------------

static std::atomic<u64> gDiagWndProcMsgs{0};
static std::atomic<u64> gDiagWndProcBlocked{0};
static std::atomic<u64> gDiagPumpedMsgs{0};

// Time spent inside HTWndProc, split into "our own code" and "the game's window
// process". The window process runs on the game's message thread, so anything it
// costs is added straight to the game's frame - it has to be measured separately
// from the present hook.
static std::atomic<u64> gDiagWndSelfTicks{0};
static std::atomic<u64> gDiagWndGameTicks{0};

void HTiDiagCountWndProcMsg(
  u08 blocked
) {
  gDiagWndProcMsgs.fetch_add(1, std::memory_order_relaxed);
  if (blocked)
    gDiagWndProcBlocked.fetch_add(1, std::memory_order_relaxed);
}

void HTiDiagAddWndProcTicks(
  i64 selfTicks,
  i64 gameTicks
) {
  gDiagWndSelfTicks.fetch_add((u64)selfTicks, std::memory_order_relaxed);
  gDiagWndGameTicks.fetch_add((u64)gameTicks, std::memory_order_relaxed);
}

// Breakdown of the loader's own share of a window message, so a slow message can
// be attributed to one step instead of the whole function.
static std::atomic<u64> gDiagWndDelegateTicks{0};   // handing the message to ImGui
static std::atomic<u64> gDiagWndQueueTicks{0};      // queueInputMsg()
static std::atomic<u64> gDiagWndHotkeyTicks{0};     // HTHotKeyWndProc()

void HTiDiagAddWndProcSplit(
  i64 delegateTicks,
  i64 queueTicks,
  i64 hotkeyTicks
) {
  gDiagWndDelegateTicks.fetch_add((u64)delegateTicks, std::memory_order_relaxed);
  gDiagWndQueueTicks.fetch_add((u64)queueTicks, std::memory_order_relaxed);
  gDiagWndHotkeyTicks.fetch_add((u64)hotkeyTicks, std::memory_order_relaxed);
}

// The single worst message seen in the current window, per side, with its
// message id. Averages hide a few catastrophic messages; this does not.
static std::atomic<u64> gDiagWorstOurTicks{0};
static std::atomic<u32> gDiagWorstOurMsg{0};
static std::atomic<u64> gDiagWorstGameTicks{0};
static std::atomic<u32> gDiagWorstGameMsg{0};

void HTiDiagWorstWndProcMsg(
  i64 ourTicks,
  i64 gameTicks,
  u32 msg
) {
  if ((u64)ourTicks > gDiagWorstOurTicks.load(std::memory_order_relaxed)) {
    gDiagWorstOurTicks.store((u64)ourTicks, std::memory_order_relaxed);
    gDiagWorstOurMsg.store(msg, std::memory_order_relaxed);
  }
  if ((u64)gameTicks > gDiagWorstGameTicks.load(std::memory_order_relaxed)) {
    gDiagWorstGameTicks.store((u64)gameTicks, std::memory_order_relaxed);
    gDiagWorstGameMsg.store(msg, std::memory_order_relaxed);
  }
}

// Per-message-id counts, to identify what the game's message stream is actually
// made of. 0x400 covers the whole documented WM_ range.
#define HT_DIAG_MSG_SLOTS 0x400
static std::atomic<u64> gDiagMsgHist[HT_DIAG_MSG_SLOTS];

void HTiDiagCountMsgType(
  u32 msg
) {
  if (msg < HT_DIAG_MSG_SLOTS)
    gDiagMsgHist[msg].fetch_add(1, std::memory_order_relaxed);
}

void HTiDiagCountPumped(
  u32 count
) {
  gDiagPumpedMsgs.fetch_add(count, std::memory_order_relaxed);
}

// Time the game spent blocked in vkAcquireNextImageKHR. This is where a game
// stalls when the swapchain has no free image, which is exactly what the
// overlay's extra submit + present on the graphics queue can cause.
static std::atomic<u64> gDiagAcquireTicks{0};
static std::atomic<u64> gDiagAcquireCount{0};

void HTiDiagAddAcquireTicks(
  i64 ticks
) {
  gDiagAcquireTicks.fetch_add((u64)ticks, std::memory_order_relaxed);
  gDiagAcquireCount.fetch_add(1, std::memory_order_relaxed);
}

// Overlay frames: how many presents actually built an ImGui frame, and how many
// were skipped because the previous overlay submit had not completed yet. A high
// skip count with a healthy game means the overlay itself is being starved.
static std::atomic<u64> gDiagOverlayRendered{0};
static std::atomic<u64> gDiagOverlaySkipped{0};

void HTiDiagCountOverlayFrame(
  u08 skipped
) {
  if (skipped)
    gDiagOverlaySkipped.fetch_add(1, std::memory_order_relaxed);
  else
    gDiagOverlayRendered.fetch_add(1, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------
// [SECTION] Frame timing.
//
// The loader's own cost and the game's frame pace are different things, and only
// the second one explains a visible stutter, so both are reported:
//
//   present - interval between two vkQueuePresentKHR entries, i.e. the game's
//             frame time, including whatever the overlay added to it
//   overlay - time spent inside the present hook, i.e. what the loader added
//
// A frame counts as a spike when it takes at least HT_DIAG_SPIKE_FACTOR times the
// average of the current window, above a floor so that a healthy game does not
// report noise. Spikes are logged individually, together with the message counts
// since the previous frame, so a spike can be attributed either to the loader or
// to a burst of input.
// ----------------------------------------------------------------------------

#define HT_DIAG_WINDOW_MS 2000.0
#define HT_DIAG_SPIKE_FACTOR 3.0
#define HT_DIAG_SPIKE_FLOOR_MS 25.0

static i64 gDiagFreq = 0;
static i64 gDiagFrameStart = 0;
static i64 gDiagPrevFrame = 0;
static i64 gDiagWindowStart = 0;
static f64 gDiagCurInterval = 0.0;
static u64 gDiagFrames = 0;
static f64 gDiagSumPresent = 0.0
  , gDiagMaxPresent = 0.0
  , gDiagSumOverlay = 0.0
  , gDiagMaxOverlay = 0.0;
static u64 gDiagSpikes = 0;
static u64 gDiagLastMsgs = 0
  , gDiagLastPumped = 0;
static u64 gDiagPrevFrameMsgs = 0
  , gDiagPrevFramePumped = 0;
// Cumulative counters as of the end of the previous reporting window, so each
// line reports its own window rather than a running total.
static u64 gDiagRepMsgs = 0
  , gDiagRepBlocked = 0
  , gDiagRepPumped = 0
  , gDiagRepSelf = 0
  , gDiagRepGame = 0
  , gDiagRepAcquireTicks = 0
  , gDiagRepAcquireCount = 0
  , gDiagRepDelegate = 0
  , gDiagRepQueue = 0
  , gDiagRepHotkey = 0
  , gDiagRepRendered = 0
  , gDiagRepSkipped = 0;
static u64 gDiagRepMsgHist[HT_DIAG_MSG_SLOTS];

i64 HTiDiagTicks() {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

static f64 diagMs(
  i64 ticks
) {
  if (!gDiagFreq)
    return 0.0;
  return (f64)ticks * 1000.0 / (f64)gDiagFreq;
}

/**
 * Call at the very top of the present hook, before any work is done.
 */
void HTiDiagFrameBegin() {
  if (!gConfigProfile)
    return;

  LARGE_INTEGER freq;
  i64 now;

  if (!gDiagFreq) {
    if (!QueryPerformanceFrequency(&freq))
      return;
    gDiagFreq = freq.QuadPart;
    gDiagWindowStart = HTiDiagTicks();
    gDiagPrevFrame = gDiagWindowStart;
  }

  now = HTiDiagTicks();
  gDiagFrameStart = now;

  if (gDiagPrevFrame) {
    gDiagCurInterval = diagMs(now - gDiagPrevFrame);
    // Message counts since the previous present, so a spike can be matched
    // against what the window thread was doing at the time.
    gDiagPrevFrameMsgs = gDiagWndProcMsgs.load(std::memory_order_relaxed) - gDiagLastMsgs;
    gDiagPrevFramePumped = gDiagPumpedMsgs.load(std::memory_order_relaxed) - gDiagLastPumped;
    gDiagLastMsgs += gDiagPrevFrameMsgs;
    gDiagLastPumped += gDiagPrevFramePumped;
  }
  gDiagPrevFrame = now;
}

/**
 * Call before every return of the present hook.
 */
void HTiDiagFrameEnd() {
  f64 overlay;
  f64 average;
  u64 msgs
    , blocked
    , pumped
    , selfTicks
    , gameTicks
    , count
    , acquireTicks
    , acquireCount
    , delegateTicks
    , queueTicks
    , hotkeyTicks;
  f64 elapsed;
  u32 top[4] = {0, 0, 0, 0};
  u64 topCount[4] = {0, 0, 0, 0};
  char topText[128];
  u32 i, j;
  size_t used;

  if (!gConfigProfile || !gDiagFreq)
    return;

  overlay = diagMs(HTiDiagTicks() - gDiagFrameStart);

  gDiagFrames++;
  gDiagSumPresent += gDiagCurInterval;
  gDiagSumOverlay += overlay;
  if (gDiagCurInterval > gDiagMaxPresent)
    gDiagMaxPresent = gDiagCurInterval;
  if (overlay > gDiagMaxOverlay)
    gDiagMaxOverlay = overlay;

  average = gDiagSumPresent / (f64)gDiagFrames;
  if (gDiagCurInterval >= HT_DIAG_SPIKE_FLOOR_MS
      && gDiagCurInterval >= average * HT_DIAG_SPIKE_FACTOR) {
    gDiagSpikes++;
    LOGI(
      "[PROF] SPIKE present=%.1fms overlay=%.2fms avg=%.1fms msgs=%llu pumped=%llu\n",
      gDiagCurInterval, overlay, average,
      (unsigned long long)gDiagPrevFrameMsgs,
      (unsigned long long)gDiagPrevFramePumped);
  }

  elapsed = diagMs(HTiDiagTicks() - gDiagWindowStart);
  if (elapsed < HT_DIAG_WINDOW_MS)
    return;

  msgs = gDiagWndProcMsgs.load(std::memory_order_relaxed);
  blocked = gDiagWndProcBlocked.load(std::memory_order_relaxed);
  pumped = gDiagPumpedMsgs.load(std::memory_order_relaxed);
  selfTicks = gDiagWndSelfTicks.load(std::memory_order_relaxed);
  gameTicks = gDiagWndGameTicks.load(std::memory_order_relaxed);
  acquireTicks = gDiagAcquireTicks.load(std::memory_order_relaxed);
  acquireCount = gDiagAcquireCount.load(std::memory_order_relaxed);
  delegateTicks = gDiagWndDelegateTicks.load(std::memory_order_relaxed);
  queueTicks = gDiagWndQueueTicks.load(std::memory_order_relaxed);
  hotkeyTicks = gDiagWndHotkeyTicks.load(std::memory_order_relaxed);
  count = msgs - gDiagRepMsgs;

  // Top message ids of this window, so the message stream can be identified.
  for (i = 0; i < HT_DIAG_MSG_SLOTS; i++) {
    u64 now = gDiagMsgHist[i].load(std::memory_order_relaxed);
    u64 delta = now - gDiagRepMsgHist[i];
    gDiagRepMsgHist[i] = now;
    if (!delta)
      continue;
    for (j = 0; j < 4; j++) {
      if (delta > topCount[j]) {
        for (u32 k = 3; k > j; k--) {
          topCount[k] = topCount[k - 1];
          top[k] = top[k - 1];
        }
        topCount[j] = delta;
        top[j] = i;
        break;
      }
    }
  }
  used = 0;
  topText[0] = 0;
  for (j = 0; j < 4; j++) {
    int written;
    if (!topCount[j])
      break;
    written = snprintf(
      topText + used, sizeof(topText) - used,
      "%s%04X:%llu", used ? " " : "", top[j],
      (unsigned long long)topCount[j]);
    if (written <= 0 || (size_t)written >= sizeof(topText) - used)
      break;
    used += (size_t)written;
  }

  LOGI(
    "[PROF] %.1fs frames=%llu fps=%.1f present avg=%.1f max=%.1f | "
    "overlay avg=%.2f max=%.2f | spikes=%llu | "
    "wmsg=%llu blocked=%llu pumped=%llu | our=%.1fms game=%.1fms | "
    "split delegate=%.1fms queue=%.1fms hotkey=%.1fms | "
    "worst our=%.1fms(0x%03X) game=%.1fms(0x%03X) | top[%s] | "
    "gui=%llu/%llu | "
    "acquire n=%llu avg=%.2fms tot=%.1fms\n",
    elapsed / 1000.0,
    (unsigned long long)gDiagFrames,
    gDiagFrames * 1000.0 / elapsed,
    gDiagSumPresent / (f64)gDiagFrames, gDiagMaxPresent,
    gDiagSumOverlay / (f64)gDiagFrames, gDiagMaxOverlay,
    (unsigned long long)gDiagSpikes,
    (unsigned long long)count,
    (unsigned long long)(blocked - gDiagRepBlocked),
    (unsigned long long)(pumped - gDiagRepPumped),
    diagMs((i64)(selfTicks - gDiagRepSelf)),
    diagMs((i64)(gameTicks - gDiagRepGame)),
    diagMs((i64)(delegateTicks - gDiagRepDelegate)),
    diagMs((i64)(queueTicks - gDiagRepQueue)),
    diagMs((i64)(hotkeyTicks - gDiagRepHotkey)),
    diagMs((i64)gDiagWorstOurTicks.load(std::memory_order_relaxed)),
    (unsigned)gDiagWorstOurMsg.load(std::memory_order_relaxed),
    diagMs((i64)gDiagWorstGameTicks.load(std::memory_order_relaxed)),
    (unsigned)gDiagWorstGameMsg.load(std::memory_order_relaxed),
    topText ? topText : "",
    (unsigned long long)(gDiagOverlayRendered.load(std::memory_order_relaxed) - gDiagRepRendered),
    (unsigned long long)(gDiagOverlaySkipped.load(std::memory_order_relaxed) - gDiagRepSkipped),
    (unsigned long long)(acquireCount - gDiagRepAcquireCount),
    (acquireCount - gDiagRepAcquireCount)
      ? diagMs((i64)(acquireTicks - gDiagRepAcquireTicks))
        / (f64)(acquireCount - gDiagRepAcquireCount)
      : 0.0,
    diagMs((i64)(acquireTicks - gDiagRepAcquireTicks)));

  gDiagRepMsgs = msgs;
  gDiagRepBlocked = blocked;
  gDiagRepPumped = pumped;
  gDiagRepSelf = selfTicks;
  gDiagRepGame = gameTicks;
  gDiagRepAcquireTicks = acquireTicks;
  gDiagRepAcquireCount = acquireCount;
  gDiagRepDelegate = delegateTicks;
  gDiagRepQueue = queueTicks;
  gDiagRepHotkey = hotkeyTicks;
  gDiagRepRendered = gDiagOverlayRendered.load(std::memory_order_relaxed);
  gDiagRepSkipped = gDiagOverlaySkipped.load(std::memory_order_relaxed);
  gDiagWorstOurTicks.store(0, std::memory_order_relaxed);
  gDiagWorstOurMsg.store(0, std::memory_order_relaxed);
  gDiagWorstGameTicks.store(0, std::memory_order_relaxed);
  gDiagWorstGameMsg.store(0, std::memory_order_relaxed);

  gDiagFrames = 0;
  gDiagSumPresent = gDiagMaxPresent = 0.0;
  gDiagSumOverlay = gDiagMaxOverlay = 0.0;
  gDiagSpikes = 0;
  gDiagWindowStart = HTiDiagTicks();
}
