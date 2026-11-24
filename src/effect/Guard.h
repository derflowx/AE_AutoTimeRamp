// Crash guard: structured-exception wrappers around every entry point AE calls, so a fault inside
// this plug-in is logged and turned into an error instead of taking down After Effects.
//
// Log: %TEMP%\AutoTimeRamp_crash.log (time, entry point, last step, exception code, address as an
// offset into AutoTimeRamp.aex; look the offset up in build\<Config>\AutoTimeRamp.map).
#pragma once

// Last step reached on this thread (a string literal); shown in the crash log.
extern thread_local const char* g_atrStep;
#define ATR_STEP(s) (g_atrStep = (s))

// Opt-in render debug log: active while %TEMP%\AutoTimeRamp_debug.flag exists; appends to
// %TEMP%\AutoTimeRamp_debug.log. printf-style.
void AtrDebugLog(const char* fmt, ...);

#ifdef _WIN32
struct _EXCEPTION_POINTERS;
int AtrCrashFilter(struct _EXCEPTION_POINTERS* ep, const char* where);
#endif
