// exit_common - shared, game-agnostic ReXGlue glue
//
// See exit_common/crash.h for rationale. This is the game-neutral symbolizing
// crash filter; SCIV's TEMPORARY type-registry bring-up diagnostics are not
// carried here (they belong in a game's own Phase-7 glue).

#include "exit_common/crash.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// dbghelp must follow windows.h
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/logging/api.h>
#include <rex/logging/macros.h>

#pragma comment(lib, "dbghelp.lib")

namespace exit_common {

namespace {

// Recompiled functions are named `sub_<guestaddr>` (alias of `__imp__sub_...`),
// so a symbolized frame embeds the guest address directly -- that string is the
// whole point of this handler.
constexpr int kMaxFrames = 48;

// Module base of the main exe, captured at install. Frames inside the exe are
// also reported as base-relative RVAs so a frame can be cross-referenced even
// when DbgHelp can't name it.
uintptr_t g_module_base = 0;

// Both the rex logger and stderr -- the logger lands in logs/, stderr in the
// console the user is watching. The logger flush in the caller guarantees the
// line survives the imminent termination.
void EmitLine(const char* line) {
  REXLOG_CRITICAL("{}", line);
  std::fprintf(stderr, "%s\n", line);
}

void SymbolizeFrame(int index, DWORD64 addr) {
  char line[1024];

  // RVA relative to the exe image (matches the recompiled .map / IDA math).
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
      std::snprintf(line, sizeof(line), "  #%02d 0x%016llX  %s+0x%llX  (%s:%lu)%s",
                    index, static_cast<unsigned long long>(addr), sym->Name,
                    static_cast<unsigned long long>(disp), li.FileName, li.LineNumber, rva);
    } else {
      std::snprintf(line, sizeof(line), "  #%02d 0x%016llX  %s+0x%llX%s", index,
                    static_cast<unsigned long long>(addr), sym->Name,
                    static_cast<unsigned long long>(disp), rva);
    }
  } else {
    std::snprintf(line, sizeof(line), "  #%02d 0x%016llX  <no symbol>%s", index,
                  static_cast<unsigned long long>(addr), rva);
  }
  EmitLine(line);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  // Re-entrancy guard: if symbolization itself faults, don't recurse.
  static volatile LONG in_handler = 0;
  if (InterlockedExchange(&in_handler, 1) != 0) {
    return EXCEPTION_EXECUTE_HANDLER;
  }

  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  char header[512];

  const char* kind = "exception";
  switch (rec->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: kind = "access violation"; break;
    case EXCEPTION_IN_PAGE_ERROR: kind = "in-page error"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: kind = "illegal instruction"; break;
    case EXCEPTION_STACK_OVERFLOW: kind = "stack overflow"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: kind = "integer divide-by-zero"; break;
    case EXCEPTION_PRIV_INSTRUCTION: kind = "privileged instruction"; break;
    default: break;
  }

  EmitLine("==================== NATIVE CRASH ====================");
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
      rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
    const char* op = rec->ExceptionInformation[0] == 1   ? "write"
                     : rec->ExceptionInformation[0] == 8 ? "execute"
                                                         : "read";
    std::snprintf(header, sizeof(header),
                  "%s (code 0x%08lX): %s of address 0x%016llX  on thread %lu",
                  kind, rec->ExceptionCode, op,
                  static_cast<unsigned long long>(rec->ExceptionInformation[1]),
                  GetCurrentThreadId());
  } else {
    std::snprintf(header, sizeof(header), "%s (code 0x%08lX)  on thread %lu", kind,
                  rec->ExceptionCode, GetCurrentThreadId());
  }
  EmitLine(header);

  // Faulting instruction first -- this is the recompiled function that crashed.
  EmitLine("backtrace (innermost first):");
  SymbolizeFrame(0, reinterpret_cast<DWORD64>(rec->ExceptionAddress));

  // Walk the rest of the stack. StackWalk64 mutates the CONTEXT, so copy it.
  CONTEXT ctx = *info->ContextRecord;
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
  HANDLE thread = GetCurrentThread();
  for (int i = 1; i < kMaxFrames; ++i) {
    if (!StackWalk64(machine, proc, thread, &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }
    SymbolizeFrame(i, frame.AddrPC.Offset);
  }
  EmitLine("======================================================");

  if (auto logger = ::rex::GetLogger()) {
    logger->flush();
  }
  std::fflush(stderr);

  // Terminate -- the process state is unrecoverable.
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void InstallCrashHandler(const std::string& app_name) {
  g_module_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME |
                SYMOPT_FAIL_CRITICAL_ERRORS);
  // Prime symbols now, off the crash path: the PDB load and DbgHelp init can
  // allocate, which is risky to do for the first time inside a faulted process.
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);

  SetUnhandledExceptionFilter(CrashFilter);
  REXLOG_INFO("{} crash handler installed (symbolized backtraces on native fault)",
              app_name);
}

}  // namespace exit_common

#else  // !_WIN32

namespace exit_common {
void InstallCrashHandler(const std::string& /*app_name*/) {}
}  // namespace exit_common

#endif
