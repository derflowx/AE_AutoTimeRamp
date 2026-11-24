#include "Guard.h"

#ifdef _WIN32
    #include <Windows.h>

    #include <cstdio>
    #include <cstdlib>

thread_local const char* g_atrStep = "none";

    #include <cstdarg>
    #include <mutex>

void AtrDebugLog(const char* fmt, ...)
{
    char dir[MAX_PATH] = {};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH - 40)
        return;
    char flag[MAX_PATH], path[MAX_PATH];
    sprintf_s(flag, "%sAutoTimeRamp_debug.flag", dir);
    if (GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES)
        return;
    sprintf_s(path, "%sAutoTimeRamp_debug.log", dir);
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f)
        return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

int AtrCrashFilter(EXCEPTION_POINTERS* ep, const char* where)
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&AtrCrashFilter),
                       &self);
    const unsigned long long base = reinterpret_cast<unsigned long long>(self);
    const unsigned long long addr =
        ep && ep->ExceptionRecord ? reinterpret_cast<unsigned long long>(ep->ExceptionRecord->ExceptionAddress) : 0;
    const unsigned long code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;

    char path[MAX_PATH] = {};
    const DWORD n = GetTempPathA(MAX_PATH, path);
    if (n > 0 && n < MAX_PATH - 32)
    {
        strcat_s(path, "AutoTimeRamp_crash.log");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") == 0 && f)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            const bool inModule = addr >= base && addr < base + 0x10000000ULL;
            std::fprintf(f,
                         "%04d-%02d-%02d %02d:%02d:%02d entry=%s step=%s code=0x%08lX %s=0x%llX\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         where, g_atrStep ? g_atrStep : "?", code,
                         inModule ? "rva" : "addr", inModule ? addr - base : addr);
            std::fclose(f);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
thread_local const char* g_atrStep = "none";
void AtrDebugLog(const char*, ...) {}
#endif
