#include "engine.hpp"
#include <windows.h>
#include <cwchar>
#include <iostream>
#include <string>

namespace {
bool ProbeWrite(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char payload[] = "USBGuard test VHD volume payload\n";
    DWORD written = 0;
    const bool ok = WriteFile(file, payload, static_cast<DWORD>(sizeof(payload)), &written, nullptr) != FALSE
                    && written == sizeof(payload) && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    HANDLE verify = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (verify == INVALID_HANDLE_VALUE) {
        DeleteFileW(path.c_str());
        return false;
    }
    char buffer[sizeof(payload)]{};
    DWORD read = 0;
    const bool equal = ReadFile(verify, buffer, sizeof(buffer), &read, nullptr) != FALSE
                       && read == sizeof(payload)
                       && memcmp(buffer, payload, sizeof(payload)) == 0;
    CloseHandle(verify);
    DeleteFileW(path.c_str());
    return ok && equal;
}
void Trace(const wchar_t* description, const std::wstring& detail) {
    std::wcout << description << L": " << detail << std::endl;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::wcerr << L"Usage: storage_integration <disk-number> <drive-letter> <MBR|GPT>\n";
        return 2;
    }
    wchar_t* end = nullptr;
    unsigned long parsed = wcstoul(argv[1], &end, 10);
    if (!argv[1][0] || *end != L'\0' || parsed == MAXDWORD) return 2;
    const DWORD number = static_cast<DWORD>(parsed);
    if (wcslen(argv[2]) != 1 ||
        !((argv[2][0] >= L'A' && argv[2][0] <= L'Z') ||
          (argv[2][0] >= L'a' && argv[2][0] <= L'z'))) return 2;
    const std::wstring root = std::wstring(1, argv[2][0]) + L":\\";
    const std::wstring probe = root + L"usbguard-disposable-probe.tmp";
    const PARTITION_STYLE style = _wcsicmp(argv[3], L"GPT") == 0
        ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;

    std::wstring problem;
    if (!usb::Discover(problem)) {
        Trace(L"Discovery failed", problem);
        return 3;
    }
    usb::Disk* target = nullptr;
    for (auto& disk : usb::g_disks) {
        if (disk.number == number) { target = &disk; break; }
    }
    if (!target || target->style != style || usb::g_windowsDisks.count(number) ||
        target->volumes.empty()) {
        std::wcerr << L"Disposable VHD target missing or unsafe. Disk " << number << std::endl;
        return 4;
    }
    bool foundExpectedRoot = false;
    for (auto& volume : target->volumes) {
        volume.selected = volume.mountPoints.find(root.substr(0, 2)) != std::wstring::npos;
        if (volume.selected) foundExpectedRoot = true;
    }
    if (!foundExpectedRoot || (style == PARTITION_STYLE_MBR &&
        target->volumes.size() != 1)) {
        std::wcerr << L"Required test volume is missing or disk layout unexpected\n";
        return 5;
    }
    // Unprotect even when an earlier assertion fails, so cleanup can detach VHD.
    std::wstring initial;
    if (!usb::ChangeDisk(*target, policy::Action::Unprotect, initial)) {
        Trace(L"Initial writable state could not be established", initial);
        return 6;
    }
    if (!ProbeWrite(probe)) {
        std::wcerr << L"Initial write and reopen verification failed\n";
        return 7;
    }
    std::wstring protectedResult;
    bool protectedOk = usb::ChangeDisk(*target, policy::Action::Protect, protectedResult);
    Trace(L"Protect", protectedResult);
    bool actualReadOnly = false;
    bool statusKnown = false;
    for (const auto& volume : target->volumes) if (volume.selected)
        statusKnown = usb::ReadMountedReadOnly(volume.devicePath, actualReadOnly);
    const bool acceptedUnexpectedWrite = ProbeWrite(probe);
    std::wstring restoredResult;
    bool restoredOk = usb::ChangeDisk(*target, policy::Action::Unprotect, restoredResult);
    Trace(L"Unprotect", restoredResult);
    bool writableAgain = restoredOk && ProbeWrite(probe);
    if (!protectedOk || !statusKnown || !actualReadOnly ||
        acceptedUnexpectedWrite || !writableAgain) {
        std::wcerr << L"Read-only enforcement failed. protect=" << protectedOk
                   << L" mounted=" << statusKnown << L" readonly=" << actualReadOnly
                   << L" wroteWhileProtected=" << acceptedUnexpectedWrite
                   << L" unprotect=" << restoredOk << L" writableAgain=" << writableAgain << std::endl;
        return 8;
    }
    std::wcout << L"Disposable VHD write-protection cycle PASSED (" << argv[3] << L")\n";
    return 0;
}
