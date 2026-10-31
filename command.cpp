#include "command.hpp"
#include <windows.h>
namespace usb {
void WriteConsoleLine(const std::wstring& message) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD ignored = 0, mode = 0;
    std::wstring line = message + L"\r\n";
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &ignored, nullptr);
    } else {
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string utf8(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), n, nullptr, nullptr);
            WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &ignored, nullptr);
        }
    }
}
CommandResult RunOnSource(policy::Action action, Progress progress) {
    if (action == policy::Action::Invalid) return {2, L"无效参数。使用 --protect、--unprotect、--toggle 或 --status。"};
    std::wstring error;
    if (!Discover(error)) return {3, error};
    Disk* source = SourceDisk();
    if (!source || source->volumes.empty())
        return {3, L"无法确定程序所在 U 盘的可识别基本数据卷，拒绝操作。"};
    std::wstring result;
    const bool ok = ChangeDisk(*source, action, result, progress);
    return {ok ? 0 : 4, std::wstring(L"目标：") + DiskLabel(*source) + L"。" + result};
}
}
