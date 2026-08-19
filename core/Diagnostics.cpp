// ---------------------------------------------------------------------------
// Writing the diagnostics log.  Appends are line-at-a-time JSON, guarded so a
// failed write can never turn into a failure of its own, and callable from any
// thread including the hook thread and the unhandled-exception filter.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#define NOMINMAX
#include "Diagnostics.h"

#include <shlobj.h>
#include <atomic>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

// This file's own dependencies, declared here rather than inherited from
// whatever else happens to be in the link: SHGetKnownFolderPath and
// CoTaskMemFree for the profile folder, the registry calls for the Windows
// build number.  A diagnostics module that only links when someone else pulls
// its libraries in is one reordered build script away from not linking at all.
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Storage format: one JSON object per line (JSONL), appended and never
// rewritten in place.  A line is written with a single WriteFile on a handle
// opened for FILE_APPEND_DATA, which is the one shape of write Windows will
// not interleave with another process's — so the core, the Settings app and a
// second copy of either can all append without a lock between them, and a
// reader always sees whole lines.
//
// JSONL rather than one JSON document for exactly that reason: appending to a
// document means rewriting it, and rewriting is where a diagnostics system
// gets to destroy the evidence it exists to keep.
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kMaxBytes = 512 * 1024;   // trim threshold
constexpr size_t kKeepLines = 400;         // lines kept when trimming

std::mutex& StateLock()
{
    static std::mutex m;
    return m;
}

std::wstring ProfileDir()
{
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::wstring path(appData);
        CoTaskMemFree(appData);
        path += L"\\CKFlip3D";
        CreateDirectoryW(path.c_str(), nullptr);
        return path;
    }
    return L".";
}

// Both paths, resolved once and then never again.
//
// Resolving them per entry meant a COM allocation (SHGetKnownFolderPath), a
// std::wstring and a CreateDirectory on every single line written — wasteful
// on the ordinary path and genuinely unsafe on the one that matters most: the
// crash filter runs after something has already gone wrong, quite possibly the
// heap, and asking the shell for a folder at that moment is asking to die
// twice.  Plain arrays rather than std::wstring for the same reason.
wchar_t g_logPath[MAX_PATH]    = {};
wchar_t g_markerPath[MAX_PATH] = {};

// The marker's own first line, as ArmSession wrote it.  NoteState rewrites the
// file from this plus the current state, so the identity of the session (start
// time, pid) survives every update — that identity is the whole value of the
// line when the next launch reads it back as CK0001's detail.
wchar_t g_sessionLine[192] = {};

void ResolvePaths()
{
    if (g_logPath[0])
        return;
    const std::wstring dir = ProfileDir();
    _snwprintf_s(g_logPath, _countof(g_logPath), _TRUNCATE,
                 L"%s\\diagnostics.jsonl", dir.c_str());
    _snwprintf_s(g_markerPath, _countof(g_markerPath), _TRUNCATE,
                 L"%s\\session.lock", dir.c_str());
}

const wchar_t* MarkerPath()
{
    ResolvePaths();
    return g_markerPath;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

/// JSON string escaping.  Control characters are dropped rather than escaped:
/// they only ever arrive here from a window title, and a title is not worth a
/// malformed line.
std::string JsonEscape(const std::wstring& value)
{
    std::string s = Utf8(value);
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': break;
            case '\t': out += " ";    break;
            default:
                if (c >= 0x20) out += static_cast<char>(c);
                break;
        }
    }
    return out;
}

/// Strictly increasing id, and the timestamp, from one clock read.  100 ns
/// FILETIME ticks: unique in practice across processes, and ordered, which is
/// all the Settings app's "cleared up to here" bookkeeping needs.
unsigned long long NextId()
{
    static std::atomic<unsigned long long> last{ 0 };
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    unsigned long long now =
        (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    for (;;) {
        unsigned long long prev = last.load(std::memory_order_relaxed);
        unsigned long long next = (now > prev) ? now : prev + 1;
        if (last.compare_exchange_weak(prev, next, std::memory_order_relaxed))
            return next;
    }
}

std::wstring LocalStamp()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

/// The only write in this file.  Takes raw bytes so the crash path can reach
/// it without a std::string.
void AppendBytes(const char* data, size_t size)
{
    ResolvePaths();
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;                       // diagnostics never become the problem
    DWORD written = 0;
    WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(h);
}

void AppendLine(const std::string& line)
{
    AppendBytes(line.data(), line.size());
}

/// Keep the tail when the file grows past kMaxBytes.  Runs once per session,
/// from BeginSession, where a rewrite races nothing that matters: the worst
/// case is an entry written in the same millisecond being dropped, and the
/// alternative is a file that grows without bound for years.
void TrimIfLarge()
{
    ResolvePaths();
    const std::wstring path = g_logPath;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return;
    unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (size <= kMaxBytes)
        return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return;
    std::vector<std::string> lines;
    std::string cur;
    char buf[4096];
    size_t got = 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < got; ++i) {
            if (buf[i] == '\n') { lines.push_back(cur); cur.clear(); }
            else if (buf[i] != '\r') cur += buf[i];
        }
    }
    fclose(f);
    if (!cur.empty()) lines.push_back(cur);
    if (lines.size() <= kKeepLines)
        return;

    const std::wstring tmp = path + L".tmp";
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f)
        return;
    for (size_t i = lines.size() - kKeepLines; i < lines.size(); ++i) {
        fwrite(lines[i].data(), 1, lines[i].size(), f);
        fputc('\n', f);
    }
    fclose(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring DescribeStatus(DWORD code)
{
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring out;
    if (n > 0 && text) {
        out.assign(text, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (text) LocalFree(text);
    return out;
}

// --- Crash filter ----------------------------------------------------------

/// Which module an address belongs to, into a caller-owned buffer — no
/// allocation, so the crash filter can use it.
void ModuleAtInto(void* address, wchar_t* out, size_t cap)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &mod) || !mod)
        return;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(mod, path, MAX_PATH))
        return;
    const wchar_t* slash = wcsrchr(path, L'\\');
    wcsncpy_s(out, cap, slash ? slash + 1 : path, _TRUNCATE);
}

/// Widen ASCII into the byte buffer without touching the CRT's locale
/// machinery.  Everything written from the crash path is ASCII by
/// construction — hex, addresses, a module file name.
size_t AsciiCopy(char* dst, size_t cap, size_t at, const wchar_t* src)
{
    for (; *src && at + 1 < cap; ++src) {
        const wchar_t c = *src;
        dst[at++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return at;
}

/// Nothing that records a fatality may run twice, and the four paths that can
/// (the exception filter and the three CRT handlers below) share the latch: a
/// fault inside one of them must not be picked up by another, or a crash that
/// was about to be recorded becomes a hang that never is.
volatile LONG g_fatalEntered = 0;

bool ClaimFatalPath()
{
    return InterlockedExchange(&g_fatalEntered, 1) == 0;
}

/// One JSON line for a death, written the way the crash filter writes: a stack
/// buffer, no allocation, no lock, one WriteFile.  `detail` is ASCII by
/// construction — every caller passes a literal.
void RecordFatal(const char* detail)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[512];
    const int n = _snprintf_s(line, _countof(line), _TRUNCATE,
        "{\"id\":%llu,\"sev\":2,\"code\":\"%S\","
        "\"t\":\"%04u-%02u-%02uT%02u:%02u:%02u\","
        "\"m\":\"CKFlip3D stopped unexpectedly\",\"d\":\"%s\"}\n",
        NextId(), Diag::Code::SessionCrash,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        detail);
    if (n > 0)
        AppendBytes(line, static_cast<size_t>(n));

    // On the record now, so the marker goes: leaving it would report the same
    // death a second time next launch as a vaguer CK0001.
    DeleteFileW(MarkerPath());
}

// ---------------------------------------------------------------------------
// The deaths the exception filter cannot see.
//
// SetUnhandledExceptionFilter covers exceptions.  It does not cover the CRT's
// own fatal exits: a secure-CRT function handed an argument it rejects, an
// uncaught exception reaching std::terminate, a pure virtual call.  Each of
// those ends the process through __fastfail, which bypasses the unhandled-
// exception filter BY DESIGN — no filter runs, nothing is written, and the only
// trace left is the session marker, which the next launch reports as CK0001
// "ended without shutting down" with no idea why.
//
// That is exactly the shape of report that cannot be acted on, and the shape
// this file exists to eliminate.  Each handler below turns one of those silent
// deaths into the CK0002 it always was.  They report and then end the process
// deliberately: returning would hand control back to the very fastfail path
// they exist to pre-empt.
// ---------------------------------------------------------------------------
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                        unsigned, uintptr_t)
{
    if (ClaimFatalPath())
        RecordFatal("the program was stopped by a failed internal check "
                    "(a Windows library call was given an argument it "
                    "rejected). This is a defect in CKFlip3D, not a "
                    "configuration problem");
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnTerminate()
{
    if (ClaimFatalPath())
        RecordFatal("the program was stopped by an error that nothing was "
                    "waiting to handle (std::terminate). This is a defect in "
                    "CKFlip3D, not a configuration problem");
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnPureCall()
{
    if (ClaimFatalPath())
        RecordFatal("the program was stopped by a call into an object that was "
                    "being destroyed (pure virtual call). This is a defect in "
                    "CKFlip3D, not a configuration problem");
    TerminateProcess(GetCurrentProcess(), 3);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    // See ClaimFatalPath.
    if (!ClaimFatalPath())
        return EXCEPTION_EXECUTE_HANDLER;

    // Everything from here is deliberately primitive: a stack buffer, no
    // std::string, no allocation, and NO LOCK.  Diag::Report would take the
    // rate-limiter mutex, and if this thread is the one that crashed while
    // holding it — or another thread holds it and will never run again — the
    // process would deadlock inside its own crash reporting instead of dying
    // and being reported next launch.
    if (ep && ep->ExceptionRecord) {
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        void* addr = ep->ExceptionRecord->ExceptionAddress;

        wchar_t module[MAX_PATH] = L"unknown";
        ModuleAtInto(addr, module, _countof(module));

        const wchar_t* kind = L"";
        if (code == EXCEPTION_ACCESS_VIOLATION)
            kind = (ep->ExceptionRecord->NumberParameters >= 2
                    && ep->ExceptionRecord->ExceptionInformation[0] != 0)
                 ? L" (write to invalid address)"
                 : L" (read from invalid address)";

        SYSTEMTIME st{};
        GetLocalTime(&st);

        char line[1024];
        int n = _snprintf_s(line, _countof(line), _TRUNCATE,
            "{\"id\":%llu,\"sev\":2,\"code\":\"%S\","
            "\"t\":\"%04u-%02u-%02uT%02u:%02u:%02u\","
            "\"m\":\"CKFlip3D stopped unexpectedly\","
            "\"d\":\"exception 0x%08X at 0x%p in ",
            NextId(), Diag::Code::SessionCrash,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            code, addr);
        if (n > 0) {
            size_t at = static_cast<size_t>(n);
            at = AsciiCopy(line, sizeof(line), at, module);
            at = AsciiCopy(line, sizeof(line), at, kind);
            // NOT through AsciiCopy: it maps everything outside printable
            // ASCII to '?', which is right for a module name and wrong for the
            // line terminator — a crash entry that ended in '?' instead of a
            // newline was unreadable AND swallowed the next entry appended
            // after it.
            if (at + 3 <= sizeof(line)) {
                line[at++] = '"';
                line[at++] = '}';
                line[at++] = '\n';
            }
            AppendBytes(line, at);
        }
    }

    // The crash is now on the record, so the marker must go: leaving it would
    // report the same event a second time on the next launch, as a vaguer
    // "ended unexpectedly" with none of the detail above.
    DeleteFileW(MarkerPath());
    return EXCEPTION_EXECUTE_HANDLER;
}

std::set<std::wstring>& OnceSeen()
{
    static std::set<std::wstring> s;
    return s;
}

} // namespace

std::wstring Diag::LogPath()
{
    ResolvePaths();
    return g_logPath;
}

void Diag::Report(const wchar_t* code, Sev sev, const wchar_t* message,
                  const wchar_t* detail, bool sticky)
{
    if (!code || !message)
        return;

    // Rate limit per code.  A failure inside a per-frame or per-window path
    // would otherwise write thousands of identical lines and bury everything
    // else in the file — the one outcome that would make the log useless.
    {
        static std::mutex rateLock;
        static std::map<std::wstring, ULONGLONG> lastAt;
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> guard(rateLock);
        auto it = lastAt.find(code);
        if (it != lastAt.end() && now - it->second < 2000)
            return;
        lastAt[code] = now;
    }

    std::string line;
    line.reserve(320);
    char head[64];
    sprintf_s(head, "{\"id\":%llu,\"sev\":%d,", NextId(), static_cast<int>(sev));
    line += head;
    line += "\"code\":\"" + JsonEscape(code) + "\",";
    line += "\"t\":\"" + JsonEscape(LocalStamp()) + "\",";
    line += "\"m\":\"" + JsonEscape(message) + "\"";
    if (detail && *detail)
        line += ",\"d\":\"" + JsonEscape(detail) + "\"";
    if (sticky)
        line += ",\"k\":1";
    line += "}\n";

    AppendLine(line);
}

void Diag::ReportOnce(const wchar_t* code, Sev sev, const wchar_t* message,
                      const wchar_t* detail, bool sticky)
{
    if (!code)
        return;
    {
        std::lock_guard<std::mutex> guard(StateLock());
        if (!OnceSeen().insert(code).second)
            return;
    }
    Report(code, sev, message, detail, sticky);
}

void Diag::ReportHr(const wchar_t* code, Sev sev, const wchar_t* message,
                    HRESULT hr, const wchar_t* detail)
{
    // _snwprintf_s with _TRUNCATE, never swprintf_s: the secure form treats a
    // buffer that is too small as a PROGRAMMING error and hands it to the
    // invalid-parameter handler, which by default kills the process.  Half of
    // what gets formatted here is outside this program's control — a driver's
    // adapter name, a string out of config.json, whatever text Windows
    // attaches to an error code — so "too long" is an input, not a bug, and a
    // diagnostic that could take the program down with it would be worse than
    // no diagnostic at all.
    wchar_t buf[640];
    const std::wstring text = DescribeStatus(static_cast<DWORD>(hr));
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s%s0x%08X%s%s",
                 detail ? detail : L"", detail ? L" — " : L"",
                 static_cast<unsigned>(hr),
                 text.empty() ? L"" : L" ", text.c_str());
    Report(code, sev, message, buf);
}

void Diag::ReportLastError(const wchar_t* code, Sev sev, const wchar_t* message,
                           const wchar_t* detail)
{
    const DWORD err = GetLastError();
    wchar_t buf[640];
    const std::wstring text = DescribeStatus(err);
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s%serror %lu%s%s",
                 detail ? detail : L"", detail ? L" — " : L"",
                 err, text.empty() ? L"" : L": ", text.c_str());
    Report(code, sev, message, buf);
}

void Diag::BeginSession()
{
    TrimIfLarge();
    SetUnhandledExceptionFilter(CrashFilter);
    // The other three ways this process can die (see the handlers).
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    std::set_terminate(OnTerminate);

    // Did the last session say goodbye?  The marker is written now and removed
    // by EndSession; finding one here means the process before this did not
    // get that far — killed, crashed in a way the filter could not catch, or
    // taken down with the machine.
    const std::wstring marker = MarkerPath();
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, marker.c_str(), L"r, ccs=UTF-8") == 0 && f) {
            wchar_t prev[256] = {};
            if (!fgetws(prev, 255, f)) prev[0] = L'\0';
            fclose(f);
            // Trailing newline off the recorded line.
            for (wchar_t* p = prev; *p; ++p)
                if (*p == L'\n' || *p == L'\r') { *p = L'\0'; break; }
            // What is left when this fires has narrowed, so the entry says so.
            // Every fault this process can suffer now records itself as CK0002
            // and clears the marker on the way out (the exception filter and
            // the three CRT handlers above), which means a bare CK0001 is no
            // longer "something went wrong, unknown what" — it is a session
            // that was stopped from outside, or a machine that stopped.
            wchar_t detail[512];
            _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                L"%s — nothing was recorded for a fault, and CKFlip3D records "
                L"its own crashes as CK0002, so this session was most likely "
                L"ended from outside: Task Manager, a taskkill, an installer or "
                L"a rebuild replacing the program, or the machine losing power",
                prev[0] ? prev : L"no session detail recorded");
            Report(Code::SessionUnexpectedEnd, Sev::Critical,
                   L"The previous CKFlip3D session ended without shutting down",
                   detail);
        }
    }
    ArmSession();

    // Windows 11 is the target: the overlay's taskbar handling, the capture
    // shapes it expects and the compositor behaviour it relies on are all
    // Win11.  Build 22000 is the line.
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t build[64] = {}, display[64] = {};
            DWORD cb = sizeof(build);
            RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(build), &cb);
            cb = sizeof(display);
            RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(display), &cb);
            RegCloseKey(key);

            const long buildNo = build[0] ? wcstol(build, nullptr, 10) : 0;
            if (buildNo > 0 && buildNo < 22000) {
                wchar_t detail[256];
                _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                           L"Windows build %s%s%s%s — CKFlip3D targets Windows 11 "
                           L"(build 22000 or newer)",
                           build,
                           display[0] ? L" (" : L"", display[0] ? display : L"",
                           display[0] ? L")" : L"");
                Report(Code::OsUnsupported, Sev::Warning,
                       L"This version of Windows is not supported",
                       detail, /*sticky=*/true);
            }
        }
    }
}

void Diag::ArmSession()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, MarkerPath(), L"w, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        _snwprintf_s(g_sessionLine, _countof(g_sessionLine), _TRUNCATE,
                     L"started %04u-%02u-%02u %02u:%02u:%02u, pid %lu",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, GetCurrentProcessId());
        fwprintf(f, L"%s\n", g_sessionLine);
        fclose(f);
    } else {
        ReportOnce(Code::ProfileNotWritable, Sev::Warning,
                   L"CKFlip3D cannot write to its settings folder",
                   L"%APPDATA%\\CKFlip3D is not writable; settings and "
                   L"diagnostics will not survive a restart");
    }
}

void Diag::NoteState(const wchar_t* state)
{
    if (!state || !*state || !g_sessionLine[0])
        return;   // no armed session to annotate

    SYSTEMTIME st{};
    GetLocalTime(&st);

    FILE* f = nullptr;
    if (_wfopen_s(&f, MarkerPath(), L"w, ccs=UTF-8") != 0 || !f)
        return;   // a diagnostic that cannot be written is not a failure
    fwprintf(f, L"%s, %s since %02u:%02u:%02u\n",
             g_sessionLine, state, st.wHour, st.wMinute, st.wSecond);
    fclose(f);
}

void Diag::EndSession()
{
    DeleteFileW(MarkerPath());
}
