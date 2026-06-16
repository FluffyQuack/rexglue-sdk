#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

// Local co-op support. Returns true if guest user slot `user_index` should be
// reported as a signed-in local user. Slot 0 is always signed in (P1, matching
// the single loaded user profile). Slots 1..3 are signed in only when the
// `local_coop` cvar is enabled AND a controller is actually connected at that
// slot, so single-player (one pad) never spuriously gains extra users.
// Defined in xam_input.cpp (it needs the input system).
bool xeXamUserIsLocallySignedIn(uint32_t user_index);

rex::runtime::Export* RegisterExport_xam(rex::runtime::Export* export_entry);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                       \
  void Register##n##Exports(rex::runtime::ExportResolver* export_resolver, \
                            system::KernelState* kernel_state);
#include "module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace rex
