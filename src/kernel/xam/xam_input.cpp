/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

}  // namespace xam
}  // namespace kernel
}  // namespace rex

// Report controllers 2-4 as additional signed-in local users so games that gate
// a second player on a signed-in profile (the "P2 press start to join" flow)
// see them. Gating still requires a real connected controller, so this is safe
// to leave on for single-player. Disable to force legacy single-user behavior.
REXCVAR_DEFINE_BOOL(local_coop, true, "Input",
                    "Report connected controllers 2-4 as additional signed-in "
                    "local users (enables local co-op).");

// Default-off diagnostic: log how the title routes input so we can see whether
// P2 (slot 1) is reaching the game. Logs any-user keystroke resolution + the
// reported acting controller, per-slot signin queries, and per-slot GetState
// polls. Enable with [Input] coop_diag=true (or --coop_diag=true).
REXCVAR_DEFINE_BOOL(coop_diag, false, "Input",
                    "Log local co-op input routing (any-user keystroke/state "
                    "resolution and per-slot signin) for diagnosing P2 join.");

namespace rex {
namespace kernel {
namespace xam {

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

bool xeXamUserIsLocallySignedIn(uint32_t user_index) {
  if (user_index == 0) {
    // P1 is always signed in (matches the single loaded user profile and the
    // NOP driver, which spoofs a connected pad for slot 0).
    return true;
  }
  if (user_index >= 4 || !REXCVAR_GET(local_coop)) {
    return false;
  }
  auto* is = input_system();
  if (!is) {
    return false;
  }
  // A co-op slot counts as a signed-in user only when a real controller is
  // present there. The NOP fallback driver only reports slot 0, so a single pad
  // never spuriously gains a second user.
  X_INPUT_CAPABILITIES caps = {};
  auto result = is->GetCapabilities(user_index, 0, &caps);
  bool signed_in = result != X_ERROR_DEVICE_NOT_CONNECTED;

  if (REXCVAR_GET(coop_diag)) {
    // Throttle: only log when a slot's signed-in state changes.
    static bool last_signed_in[4] = {false, false, false, false};
    if (signed_in != last_signed_in[user_index]) {
      last_signed_in[user_index] = signed_in;
      REXKRNL_INFO("[COOP] slot {} signed-in -> {} (GetCapabilities=0x{:X})", user_index,
                   signed_in, (uint32_t)result);
    }
  }
  return signed_in;
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0. (GetState has no driver-level any-user scan, unlike
    // GetKeystroke; games detect "any controller" through the keystroke path.)
    actual_user_index = 0;
  }

  auto* is = input_system();
  auto result = is->GetState(actual_user_index, input_state);
  if (REXCVAR_GET(coop_diag) && result == X_ERROR_SUCCESS && actual_user_index != 0) {
    // One line per co-op slot the first time the game reads its pad state, so we
    // can tell whether the title polls P2 directly (vs only the keystroke path).
    static uint32_t logged_mask = 0;
    uint32_t bit = 1u << (actual_user_index & 3);
    if (!(logged_mask & bit)) {
      logged_mask |= bit;
      REXKRNL_INFO("[COOP] GetState slot {} connected -- game is polling this pad",
                   actual_user_index);
    }
  }
  return result;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Any-user query. Single-user mode pins it to slot 0; with local co-op on we
    // forward "scan all pads" (0xFF) so the driver reports which controller
    // actually pressed the key. This is the mechanism a game uses to detect
    // "press Start on any controller" -- title-screen primary select and the P2
    // join prompt -- so pinning to 0 here silently swallows every P2 press.
    actual_user_index = REXCVAR_GET(local_coop) ? 0xFFu : 0u;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(actual_user_index, flags, keystroke);
  if (REXCVAR_GET(coop_diag) && result == X_ERROR_SUCCESS) {
    REXKRNL_INFO("[COOP] keystroke in=0x{:X} flags=0x{:X} -> acting user={} vk=0x{:X} kf=0x{:X}",
                 (uint32_t)user_index, (uint32_t)flags, (uint32_t)keystroke->user_index,
                 (uint32_t)keystroke->virtual_key, (uint32_t)keystroke->flags);
  }
  return result;
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Any-user query. With local co-op on, forward "scan all pads" (0xFF) so the
    // driver reports the acting controller below (written back via
    // user_index_ptr); single-user mode pins to slot 0. See the non-Ex variant.
    user_index = REXCVAR_GET(local_coop) ? 0xFFu : 0u;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
    if (REXCVAR_GET(coop_diag)) {
      REXKRNL_INFO("[COOP] keystrokeEx in=0x{:X} flags=0x{:X} -> acting user={} vk=0x{:X}",
                   (uint32_t)user_index, (uint32_t)flags, (uint32_t)keystroke->user_index,
                   (uint32_t)keystroke->virtual_key);
    }
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if ((user_index & 0xFF) == 0xFF || xeXamUserIsLocallySignedIn(user_index)) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
