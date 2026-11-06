#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#include "renderer.h"

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace tbd {

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

namespace {

std::mutex& LogLock() { static std::mutex m; return m; }

// Seconds since the first log line, so a log shows how long each stage took and how close to the crash a line was.
double Seconds() {
    static const ULONGLONG start = GetTickCount64();
    return (double)(GetTickCount64() - start) / 1000.0;
}

// The last lines, kept in memory so a crash report can repeat them even if the file write was the thing that failed.
constexpr int kRing = 64;
char g_ring[kRing][256];
int g_ringNext = 0;

// Identical lines in a row (a per-frame message) are folded into one "repeated N times" line.
char g_lastLine[1024];
int g_repeat = 0;

FILE* OpenLog(const wchar_t* mode) {
    FILE* f = nullptr;
    const std::wstring path = ExeDirectory() + L"\\engine.log";
    return _wfopen_s(&f, path.c_str(), mode) == 0 ? f : nullptr;
}

void WriteLine(FILE* f, const char* line) {
    fprintf(f, "[%8.3f t%05lu] %s\n", Seconds(), GetCurrentThreadId(), line);
}

void FlushRepeat(FILE* f) {
    if (g_repeat > 0) {
        char note[64];
        snprintf(note, sizeof(note), "(last line repeated %d more times)", g_repeat);
        WriteLine(f, note);
        g_repeat = 0;
    }
}

void Rotate() { // first line of a run: keep the previous run as engine.log.old
    const std::wstring base = ExeDirectory() + L"\\engine.log";
    MoveFileExW(base.c_str(), (base + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[48];
    swprintf(buf, 48, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN / fail-fast";
    case 0xE06D7363: return "C++ exception (uncaught)";
    default: return "?";
    }
}

LONG g_inCrash = 0;
bool g_cleanExit = false;

void PrintFrames(FILE* f, HANDLE process, HANDLE thread, CONTEXT c, int maxFrames) {
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = c.Rip; frame.AddrFrame.Offset = c.Rbp; frame.AddrStack.Offset = c.Rsp;
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    for (int k = 0; k < maxFrames && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &c, nullptr,
                                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++k) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 d64 = 0;
        DWORD d32 = 0;
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        const DWORD64 pc = frame.AddrPC.Offset;
        const bool named = SymFromAddr(process, pc, &d64, sym);
        const bool lined = SymGetLineFromAddr64(process, pc, &d32, &line);
        IMAGEHLP_MODULE64 mod = {};
        mod.SizeOfStruct = sizeof(mod);
        const bool hasMod = SymGetModuleInfo64(process, pc, &mod);
        fprintf(f, "  #%02d 0x%016llx %s!%s+0x%llx  %s:%lu\n", k, (unsigned long long)pc, hasMod ? mod.ModuleName : "?",
                named ? sym->Name : "?", (unsigned long long)d64, lined ? line.FileName : "?", lined ? line.LineNumber : 0);
        if (pc == 0) break;
    }
}

// Writes crash_<time>.log (everything below) and crash_<time>.dmp (open in Visual Studio: full memory, threads and handles).
void WriteCrashReport(EXCEPTION_POINTERS* info, const char* why) {
    if (InterlockedExchange(&g_inCrash, 1)) return; // a crash inside the crash handler: give up rather than loop
    const std::wstring dir = ExeDirectory();
    const std::wstring stamp = Stamp();

    // Minidump first: it is the most useful artifact and the least likely to be broken by a damaged heap.
    {
        HANDLE file = CreateFileW((dir + L"\\crash_" + stamp + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), info, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                              MiniDumpWithIndirectlyReferencedMemory),
                              info ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(file);
        }
    }

    FILE* f = nullptr;
    const std::wstring logPath = dir + L"\\crash_" + stamp + L".log";
    if (_wfopen_s(&f, logPath.c_str(), L"w") != 0 || !f) return;
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    fprintf(f, "CRASH (%s) at %.3f s into the run, crashing thread %lu\n", why, Seconds(), GetCurrentThreadId());
    if (info && info->ExceptionRecord) {
        const EXCEPTION_RECORD* r = info->ExceptionRecord;
        fprintf(f, "exception %08lx %s at 0x%p\n", r->ExceptionCode, ExceptionName(r->ExceptionCode), r->ExceptionAddress);
        if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && r->NumberParameters >= 2)
            fprintf(f, "  %s address 0x%llx%s\n", r->ExceptionInformation[0] == 0 ? "read of" : r->ExceptionInformation[0] == 1 ? "write to" : "execute of",
                    (unsigned long long)r->ExceptionInformation[1], r->ExceptionInformation[1] < 0x10000 ? "  (null pointer + small offset)" : "");
        if (r->ExceptionCode == 0xC0000409 && r->NumberParameters >= 1) fprintf(f, "  fail-fast code %llu\n", (unsigned long long)r->ExceptionInformation[0]);
    }

    MEMORYSTATUSEX ms = {sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms))
        fprintf(f, "system memory: %llu MB free of %llu MB physical; commit %llu MB free\n", ms.ullAvailPhys >> 20, ms.ullTotalPhys >> 20, ms.ullAvailPageFile >> 20);
    PROCESS_MEMORY_COUNTERS pmc = {sizeof(pmc)};
    if (GetProcessMemoryInfo(process, &pmc, sizeof(pmc)))
        fprintf(f, "process memory: %llu MB working set, peak %llu MB, %llu MB private\n", (unsigned long long)(pmc.WorkingSetSize >> 20),
                (unsigned long long)(pmc.PeakWorkingSetSize >> 20), (unsigned long long)(pmc.PagefileUsage >> 20));

    if (info && info->ContextRecord) {
        const CONTEXT& c = *info->ContextRecord;
        fprintf(f, "registers: rip=%016llx rsp=%016llx rbp=%016llx\n  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx\n",
                c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9);
        fprintf(f, "\ncrashing thread stack:\n");
        PrintFrames(f, process, GetCurrentThread(), *info->ContextRecord, 48);
    } else {
        CONTEXT c = {};
        RtlCaptureContext(&c);
        fprintf(f, "\ncrashing thread stack:\n");
        PrintFrames(f, process, GetCurrentThread(), c, 48);
    }

    fprintf(f, "\nloaded modules:\n");
    HMODULE mods[256];
    DWORD needed = 0;
    if (EnumProcessModules(process, mods, sizeof(mods), &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 256; ++i) {
            wchar_t name[MAX_PATH];
            MODULEINFO mi = {};
            GetModuleFileNameExW(process, mods[i], name, MAX_PATH);
            GetModuleInformation(process, mods[i], &mi, sizeof(mi));
            fprintf(f, "  %p %8lu KB  %ls\n", mi.lpBaseOfDll, mi.SizeOfImage / 1024, name);
        }
    }

    fprintf(f, "\nlast log lines before the crash:\n");
    for (int i = 0; i < kRing; ++i) {
        const char* line = g_ring[(g_ringNext + i) % kRing];
        if (line[0]) fprintf(f, "  %s\n", line);
    }
    fclose(f);

    // Also leave a pointer in the main log, which is what people read first.
    if (FILE* lf = OpenLog(L"ab")) {
        fprintf(lf, "[%8.3f t%05lu] CRASH (%s) - report: crash_%ls.log, dump: crash_%ls.dmp\n", Seconds(), GetCurrentThreadId(), why, stamp.c_str(), stamp.c_str());
        fclose(lf);
    }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
    WriteCrashReport(info, "unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH; // lets Windows Error Reporting / a debugger still see it
}

// Sees every exception before any handler does. Only the ones that are never routine are reported here: a stack overflow
// (its own filter has no stack left to run on) and heap corruption. Others are left to UnhandledFilter.
LONG WINAPI FirstChance(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374) {
        Log("FATAL first-chance %s", ExceptionName(code));
        WriteCrashReport(info, ExceptionName(code));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
    const char* what = "std::terminate (uncaught C++ exception or noexcept violation)";
    try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
    catch (const std::exception& ex) { Log("FATAL uncaught exception: %s", ex.what()); }
    catch (...) { Log("FATAL uncaught non-standard exception"); }
    WriteCrashReport(nullptr, what);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnAbort(int) {
    WriteCrashReport(nullptr, "abort() / failed assert");
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnInvalidParameter(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t) {
    Log("FATAL invalid CRT parameter in %ls (%ls:%u): %ls", func ? func : L"?", file ? file : L"?", line, expr ? expr : L"?");
    WriteCrashReport(nullptr, "invalid CRT parameter");
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnPureCall() {
    WriteCrashReport(nullptr, "pure virtual call");
    TerminateProcess(GetCurrentProcess(), 3);
}

void AtExit() {
    if (FILE* f = OpenLog(L"ab")) {
        FlushRepeat(f);
        WriteLine(f, g_cleanExit ? "process exiting normally" : "process exited through exit() without MarkCleanExit");
        fclose(f);
    }
}

} // namespace

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    std::lock_guard<std::mutex> guard(LogLock());
    static bool first = true;
    if (first) { first = false; Rotate(); }

    snprintf(g_ring[g_ringNext], sizeof(g_ring[0]), "[%8.3f t%05lu] %s", Seconds(), GetCurrentThreadId(), line);
    g_ringNext = (g_ringNext + 1) % kRing;

    FILE* f = OpenLog(L"ab");
    if (!f) return;
    if (g_lastLine[0] && strcmp(g_lastLine, line) == 0) { ++g_repeat; fclose(f); return; }
    FlushRepeat(f);
    WriteLine(f, line);
    snprintf(g_lastLine, sizeof(g_lastLine), "%s", line);
    fclose(f); // closed every time: nothing is lost if the process dies on the next instruction
}

void InstallCrashHandlers() {
    SetUnhandledExceptionFilter(UnhandledFilter);
    AddVectoredExceptionHandler(1, FirstChance);
    std::set_terminate(OnTerminate);
    signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    atexit(AtExit);

    Log("--- engine start, pid %lu, built %s %s ---", GetCurrentProcessId(), __DATE__, __TIME__);
    MEMORYSTATUSEX ms = {sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) Log("system: %llu MB RAM (%llu MB free)", ms.ullTotalPhys >> 20, ms.ullAvailPhys >> 20);
}

void MarkCleanExit() { g_cleanExit = true; }

} // namespace tbd

namespace tbd {
void Renderer::LogCounts(const char* api) const {
    Log("%s: draws: scene %u (%.2f M triangles), shadow %u (%.2f M triangles), material binds %u", api, lastCounts_.draws,
        (double)lastCounts_.triangles / 1e6, lastCounts_.shadowDraws, (double)lastCounts_.shadowTriangles / 1e6, lastCounts_.materialBinds);
}

void Renderer::UploadSteps::Done(const char* api, const char* what, uint64_t bytes, double slowMs) {
    const double total = TraceNowMs() - start;
    if (total < slowMs) return;
    char line[512];
    int pos = snprintf(line, sizeof(line), "%s: slow %s (%llu KB) %.1f ms:", api, what, (unsigned long long)(bytes >> 10), total);
    double prev = start;
    for (int i = 0; i < n && pos > 0 && pos < (int)sizeof(line); ++i) {
        pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %s %.1f", name[i], at[i] - prev);
        prev = at[i];
    }
    Log("%s", line);
}
} // namespace tbd
