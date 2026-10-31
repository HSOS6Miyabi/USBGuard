#include "engine.hpp"
#include <windows.h>
#include <cwchar>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

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
bool RejectWriteWhileProtected(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const char payload[] = "This must not be written.";
        DWORD count = 0;
        const BOOL wrote = WriteFile(file, payload, sizeof(payload), &count, nullptr);
        const DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        DeleteFileW(path.c_str());
        std::wcerr << L"Protected volume unexpectedly allowed file creation. WriteFile="
                   << wrote << L" writeError=" << writeError << std::endl;
        return false;
    }
    const DWORD error = GetLastError();
    std::wcout << L"Protected CreateFile rejected with Win32 error " << error << std::endl;
    return error == ERROR_ACCESS_DENIED || error == ERROR_WRITE_PROTECT;
}
void Trace(const wchar_t* description, const std::wstring& detail) {
    std::wcout << description << L": " << detail << std::endl;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) {
        std::wcerr << L"Usage: storage_integration <disk-number> <drive-letter> <MBR|GPT> [second-GPT-drive]\n";
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
    const std::wstring initialPath = root + L"usbguard-initial.tmp";
    const std::wstring blockedPath = root + L"usbguard-protected.tmp";
    const std::wstring restoredPath = root + L"usbguard-restored.tmp";
    const PARTITION_STYLE style = _wcsicmp(argv[3], L"GPT") == 0
        ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
    const bool multiple = argc == 5 && style == PARTITION_STYLE_GPT;
    if (argc == 5 && !multiple) return 2;
    if (multiple && (wcslen(argv[4]) != 1 ||
        !((argv[4][0] >= L'A' && argv[4][0] <= L'Z') ||
          (argv[4][0] >= L'a' && argv[4][0] <= L'z')))) return 2;
    const std::wstring secondRoot = multiple ? std::wstring(1, argv[4][0]) + L":\\" : L"";
    if (multiple && _wcsicmp(secondRoot.c_str(), root.c_str()) == 0) return 2;

    // Never run this destructive test against a real USB or system disk.
    const std::wstring physical = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
    HANDLE diskHandle = CreateFileW(physical.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DEVICE_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    const bool disposable = diskHandle != INVALID_HANDLE_VALUE &&
        DeviceIoControl(diskHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &descriptor, sizeof(descriptor), &returned, nullptr) &&
        (descriptor.BusType == BusTypeFileBackedVirtual || descriptor.BusType == BusTypeVirtual);
    if (diskHandle != INVALID_HANDLE_VALUE) CloseHandle(diskHandle);
    if (!disposable) {
        std::wcerr << L"Refusing to modify a nonvirtual disk\\n";
        return 9;
    }
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
    bool foundSecondRoot = !multiple;
    for (auto& volume : target->volumes) {
        const bool first = volume.mountPoints.find(root) != std::wstring::npos;
        const bool second = multiple && volume.mountPoints.find(secondRoot) != std::wstring::npos;
        volume.selected = first || second;
        foundExpectedRoot = foundExpectedRoot || first;
        foundSecondRoot = foundSecondRoot || second;
    }
    if (!foundExpectedRoot || !foundSecondRoot ||
        (style == PARTITION_STYLE_MBR && target->volumes.size() != 1)) {
        std::wcerr << L"Required test volume is missing or disk layout unexpected\n";
        return 5;
    }
    // Unprotect even when an earlier assertion fails, so cleanup can detach VHD.
    std::wstring initial;
    if (!usb::ChangeDisk(*target, policy::Action::Unprotect, initial)) {
        Trace(L"Initial writable state could not be established", initial);
        return 6;
    }
    if (!ProbeWrite(initialPath) ||
        (multiple && !ProbeWrite(secondRoot + L"usbguard-initial-2.tmp"))) {
        std::wcerr << L"Initial write and reopen verification failed\n";
        return 7;
    }
    bool firstOnlyOk = true;
    if (multiple) {
        // Establish an intentionally mixed GPT state: first volume readonly,
        // second volume writable. Toggle must then protect *both* volumes.
        for (auto& volume : target->volumes)
            volume.selected = volume.mountPoints.find(root) != std::wstring::npos;
        std::wstring partial;
        firstOnlyOk = usb::ChangeDisk(*target, policy::Action::Protect, partial);
        Trace(L"Partial protection", partial);
        firstOnlyOk = firstOnlyOk && ProbeWrite(secondRoot + L"usbguard-mixed-writable.tmp");
        for (auto& volume : target->volumes)
            volume.selected = volume.mountPoints.find(root) != std::wstring::npos ||
                              volume.mountPoints.find(secondRoot) != std::wstring::npos;
    }
    std::vector<std::wstring> phases;
    bool monotonic = true;
    size_t previousDone = 0;
    auto progress = [&](size_t done, size_t total, const std::wstring& stage) {
        if (total == 0 || done > total || done < previousDone) monotonic = false;
        previousDone = done;
        phases.push_back(stage);
        std::wcout << L"PROGRESS " << done << L"/" << total << L" " << stage << std::endl;
    };
    std::wstring protectedResult;
    bool protectedOk = usb::ChangeDisk(*target,
        multiple ? policy::Action::Toggle : policy::Action::Protect,
        protectedResult, progress);
    Trace(L"Protect", protectedResult);
    bool actualReadOnly = false;
    bool statusKnown = false;
    statusKnown = true;
    actualReadOnly = true;
    for (const auto& volume : target->volumes) if (volume.selected) {
        bool readonly = false;
        const bool readable = usb::ReadMountedReadOnly(volume.devicePath, readonly);
        statusKnown = statusKnown && readable;
        actualReadOnly = actualReadOnly && readonly;
    }
    const bool correctlyBlocked = RejectWriteWhileProtected(blockedPath) &&
        (!multiple || RejectWriteWhileProtected(secondRoot + L"usbguard-protected-2.tmp"));
    std::wstring restoredResult;
    bool restoredOk = usb::ChangeDisk(*target, policy::Action::Unprotect, restoredResult);
    Trace(L"Unprotect", restoredResult);
    bool writableAgain = restoredOk && ProbeWrite(restoredPath) &&
        (!multiple || ProbeWrite(secondRoot + L"usbguard-restored-2.tmp"));
    const auto saw = [&](const wchar_t* stage) {
        return std::any_of(phases.begin(), phases.end(), [stage](const std::wstring& item) {
            return item.find(stage) != std::wstring::npos;
        });
    };
    const bool phaseCoverage = saw(L"正在写入") &&
                               saw(L"正在强制卸载卷") &&
                               saw(L"正在重新挂载卷") &&
                               saw(L"正在验证卷只读状态");
    if (!firstOnlyOk || !protectedOk || !statusKnown || !actualReadOnly ||
        !correctlyBlocked || !writableAgain || !monotonic || !phaseCoverage) {
        std::wcerr << L"Read-only enforcement failed. protect=" << protectedOk
                   << L" mounted=" << statusKnown << L" readonly=" << actualReadOnly
                   << L" blockedWrite=" << correctlyBlocked
                   << L" unprotect=" << restoredOk << L" writableAgain=" << writableAgain
                   << L" monotonic=" << monotonic << L" phases=" << phaseCoverage << std::endl;
        return 8;
    }
    std::wcout << L"Disposable VHD write-protection cycle PASSED (" << argv[3] << L")\n";
    return 0;
}
