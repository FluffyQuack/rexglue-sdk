/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include "platform_win.h"

// dbghelp must follow windows.h (pulled in by platform_win.h).
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdint>
#include <cstdio>
#include <mutex>

#include <rex/dbg.h>
#include <rex/logging/api.h>
#include <rex/logging/macros.h>
#include <rex/string/buffer.h>

namespace rex::debug {

bool IsDebuggerAttached() {
  return IsDebuggerPresent() ? true : false;
}

void Break() {
  __debugbreak();
}

namespace detail {
void DebugPrint(const char* s) {
  OutputDebugStringA(s);
}
}  // namespace detail

namespace {

// DbgHelp is single-threaded; serialize all Sym*/StackWalk64 use here. The crash
// handler (exit_common) may have already called SymInitialize -- that's fine,
// our SymInitialize just returns FALSE and we proceed.
std::mutex g_sym_mutex;
bool g_sym_ready = false;
uintptr_t g_module_base = 0;

void EnsureSymInitialized() {
  if (g_sym_ready) {
    return;
  }
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME |
                SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  g_module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  g_sym_ready = true;
}

void SymbolizeFrameToLog(int index, DWORD64 addr) {
  // RVA relative to the exe image -- matches the recompiled .map / config TOML
  // math (guest functions live in the exe; this is how a frame is mapped back
  // to a guest address to name it).
  char rva[64] = "";
  if (g_module_base && addr >= g_module_base) {
    std::snprintf(rva, sizeof(rva), "  exe+0x%llX",
                  static_cast<unsigned long long>(addr - g_module_base));
  }

  alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 512];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;

  DWORD64 disp = 0;
  if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
    IMAGEHLP_LINE64 li = {};
    li.SizeOfStruct = sizeof(li);
    DWORD line_disp = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &line_disp, &li)) {
      REXLOG_WARN("  #{:02d} 0x{:016X}  {}+0x{:X}  ({}:{}){}", index,
                  static_cast<uint64_t>(addr), sym->Name, static_cast<uint64_t>(disp),
                  li.FileName, li.LineNumber, rva);
    } else {
      REXLOG_WARN("  #{:02d} 0x{:016X}  {}+0x{:X}{}", index, static_cast<uint64_t>(addr),
                  sym->Name, static_cast<uint64_t>(disp), rva);
    }
  } else {
    REXLOG_WARN("  #{:02d} 0x{:016X}  <no symbol>{}", index, static_cast<uint64_t>(addr),
                rva);
  }
}

}  // namespace

void DumpThreadBacktrace(void* native_thread_handle, const char* reason) {
  if (!native_thread_handle) {
    return;
  }
  HANDLE thread = reinterpret_cast<HANDLE>(native_thread_handle);

  std::lock_guard<std::mutex> lock(g_sym_mutex);
  EnsureSymInitialized();

  if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
    REXLOG_WARN("DumpThreadBacktrace: SuspendThread failed (error {})", GetLastError());
    return;
  }

  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(thread, &ctx)) {
    REXLOG_WARN("DumpThreadBacktrace: GetThreadContext failed (error {})", GetLastError());
    ResumeThread(thread);
    return;
  }

  REXLOG_WARN("==== thread backtrace: {} (innermost first) ====", reason ? reason : "");

  STACKFRAME64 frame = {};
#if defined(_M_X64)
  const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrStack.Offset = ctx.Rsp;
#else
  const DWORD machine = IMAGE_FILE_MACHINE_I386;
  frame.AddrPC.Offset = ctx.Eip;
  frame.AddrFrame.Offset = ctx.Ebp;
  frame.AddrStack.Offset = ctx.Esp;
#endif
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Mode = AddrModeFlat;

  HANDLE proc = GetCurrentProcess();
  constexpr int kMaxFrames = 64;
  for (int i = 0; i < kMaxFrames; ++i) {
    if (!StackWalk64(machine, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }
    SymbolizeFrameToLog(i, frame.AddrPC.Offset);
  }
  REXLOG_WARN("================================================");

  ResumeThread(thread);

  if (auto logger = ::rex::GetLogger()) {
    logger->flush();
  }
}

}  // namespace rex::debug
