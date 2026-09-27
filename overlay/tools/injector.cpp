// test dll injector:  strata_overlay_inject.exe <process id | exe name> <path to dll>
// (LoadLibraryW via a remote thread; target bitness must match: x64.)
// only for programs you may modify: never online games with anti-cheat.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

DWORD find_process(const std::wstring& name_or_id)
{
    wchar_t* end = nullptr;
    const unsigned long id = std::wcstoul(name_or_id.c_str(), &end, 10);
    if (end != nullptr && *end == L'\0' && id != 0) { return static_cast<DWORD>(id); }
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return 0; }
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    DWORD found = 0;
    for (BOOL ok = ::Process32FirstW(snap, &e); ok != FALSE; ok = ::Process32NextW(snap, &e)) {
        if (::_wcsicmp(e.szExeFile, name_or_id.c_str()) == 0) { found = e.th32ProcessID; break; }
    }
    ::CloseHandle(snap);
    return found;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: strata_overlay_inject <process id | exe name> <dll>\n");
        return 2;
    }
    const DWORD pid = find_process(argv[1]);
    if (pid == 0) { std::fprintf(stderr, "process not found\n"); return 3; }

    wchar_t full[MAX_PATH]{};
    if (::GetFullPathNameW(argv[2], MAX_PATH, full, nullptr) == 0 || ::GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "dll not found\n");
        return 4;
    }
    HANDLE process = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) { std::fprintf(stderr, "cannot open the process (%lu): elevated? protected?\n", ::GetLastError()); return 5; }

    const SIZE_T bytes = (std::wcslen(full) + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int rc = 6;
    if (remote != nullptr && ::WriteProcessMemory(process, remote, full, bytes, nullptr)) {
        const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr)) {
            ::WaitForSingleObject(thread, 10000);
            DWORD module = 0;
            ::GetExitCodeThread(thread, &module);
            ::CloseHandle(thread);
            std::printf(module != 0 ? "loaded into process %lu\n" : "LoadLibrary failed in process %lu\n", pid);
            rc = module != 0 ? 0 : 7;
        }
    }
    if (remote != nullptr) { ::VirtualFreeEx(process, remote, 0, MEM_RELEASE); }
    ::CloseHandle(process);
    return rc;
}
