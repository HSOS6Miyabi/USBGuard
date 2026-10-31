#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "command.hpp"
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <iterator>
#include <string>

// Notification-area balloons need no Start-menu identity or per-user registration.
// Windows 10/11 may render these notifications as system banners.
namespace {
constexpr int MAIN_ICON = 101;

// Windows 7 uses the old system notification-area balloon. An icon has to exist
// for Shell_NotifyIcon; it is deleted before the program exits.
struct Balloon {
    HWND host = nullptr;
    NOTIFYICONDATAW data{};
    bool active = false;
    bool Start() {
        host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"USB Guard Notification",
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!host) return false;
        data.cbSize = sizeof(data); data.hWnd = host; data.uID = 118;
        data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        data.uCallbackMessage = WM_APP + 118;
        data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(MAIN_ICON));
        wcscpy_s(data.szTip, L"USB Guard");
        active = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        return active;
    }
    void Tell(const std::wstring& title, const std::wstring& message, bool failed = false) {
        if (!active) return;
        data.uFlags = NIF_INFO;
        wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
        data.dwInfoFlags = failed ? NIIF_ERROR : NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    ~Balloon() {
        if (active) Shell_NotifyIconW(NIM_DELETE, &data);
        if (host) DestroyWindow(host);
    }
};
}

// One short-lived elevated worker performs disk I/O while the normal user
// process owns the Toast. Both exit at the end of the operation.
namespace {
struct Packet {
    DWORD kind = 0;   // 1: progress, 2: completed
    DWORD done = 0;
    DWORD total = 0;
    DWORD exitCode = 0;
    wchar_t text[480]{};
};
void CopyPacketText(Packet& p, const std::wstring& text) {
    wcsncpy_s(p.text, text.c_str(), _TRUNCATE);
}
bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &bytes) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}
int Worker(policy::Action action, const std::wstring& pipeName) {
    if (!Elevated() || action == policy::Action::Invalid ||
        pipeName.rfind(L"\\\\.\\pipe\\USBGuard-", 0) != 0) return 4;
    HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 4;
    auto send = [&](const Packet& packet) {
        DWORD written = 0;
        return WriteFile(h, &packet, sizeof(packet), &written, nullptr) && written == sizeof(packet);
    };
    auto result = usb::RunOnSource(action, [&](size_t done, size_t total, const std::wstring& stage) {
        Packet p{}; p.kind = 1;
        p.done = static_cast<DWORD>(std::min<size_t>(done, MAXDWORD));
        p.total = static_cast<DWORD>(std::min<size_t>(total, MAXDWORD));
        CopyPacketText(p, stage); send(p);
    });
    Packet last{}; last.kind = 2; last.exitCode = static_cast<DWORD>(result.exitCode);
    CopyPacketText(last, result.message); send(last);
    CloseHandle(h);
    return result.exitCode;
}
usb::CommandResult LaunchWorker(policy::Action action, const usb::Progress& report) {
    if (action == policy::Action::Invalid) return {2, L"参数无效，支持 --protect、--unprotect、--toggle、--status。"};
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return {4, L"无法生成进程通信标识。"};
    wchar_t guid[64]{}; StringFromGUID2(id, guid, 64);
    const std::wstring name = std::wstring(L"\\\\.\\pipe\\USBGuard-") + guid;
    // The unpredictable pipe name is further checked against the launched worker PID.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr))
        return {4, L"无法设置通信管道访问权限。"};
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1,
        sizeof(Packet) * 2, sizeof(Packet) * 2, 0, &security);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) return {4, L"创建通信管道失败。"};
    wchar_t executable[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, executable, 32768);
    if (!n || n >= 32768) { CloseHandle(pipe); return {4, L"无法获取程序路径。"}; }
    const wchar_t* flag = action == policy::Action::Protect ? L"--protect" :
                          action == policy::Action::Unprotect ? L"--unprotect" :
                          action == policy::Action::Status ? L"--status" : L"--toggle";
    const std::wstring arguments = std::wstring(L"--worker ") + flag + L" \"" + name + L"\"";
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas";
    launch.lpFile = executable;
    launch.lpParameters = arguments.c_str();
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch) || !launch.hProcess) {
        const DWORD error = GetLastError();
        CloseHandle(pipe);
        return {error == ERROR_CANCELLED ? 3 : 4,
                error == ERROR_CANCELLED ? L"管理员权限请求已取消。" : L"无法启动管理员权限工作进程：" + usb::ErrorText(error)};
    }
    OVERLAPPED pending{};
    pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (pending.hEvent) {
        BOOL ok = ConnectNamedPipe(pipe, &pending);
        if (ok || GetLastError() == ERROR_PIPE_CONNECTED) connected = true;
        else if (GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[] = {pending.hEvent, launch.hProcess};
            DWORD hit = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (hit == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe, &pending, &ignored, FALSE) != FALSE;
            }
        }
    }
    usb::CommandResult result{4, L"无法接收磁盘操作进度。"};
    if (connected) {
        DWORD clientPid = 0;
        if (GetNamedPipeClientProcessId(pipe, &clientPid) &&
            clientPid == GetProcessId(launch.hProcess)) {
            OVERLAPPED reading{};
            reading.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (reading.hEvent) {
                for (;;) {
                    ResetEvent(reading.hEvent);
                    Packet p{}; DWORD bytesRead = 0;
                    BOOL ok = ReadFile(pipe, &p, sizeof(p), nullptr, &reading);
                    if (!ok && GetLastError() == ERROR_IO_PENDING) {
                        WaitForSingleObject(reading.hEvent, INFINITE);
                        ok = GetOverlappedResult(pipe, &reading, &bytesRead, FALSE);
                    } else if (ok) {
                        ok = GetOverlappedResult(pipe, &reading, &bytesRead, TRUE);
                    }
                    if (!ok || bytesRead != sizeof(p)) break;
                    p.text[std::size(p.text) - 1] = 0;
                    if (p.kind == 1) {
                        if (report) report(p.done, p.total, p.text);
                    } else if (p.kind == 2) {
                        result.exitCode = static_cast<int>(p.exitCode);
                        result.message = p.text;
                        break;
                    }
                }
                CloseHandle(reading.hEvent);
            }
        } else result = {4, L"进程身份验证失败，拒绝接收通信数据。"};
    }
    // Always close the handle before returning; the notification process then exits immediately.
    if (pending.hEvent) {
        if (!connected) CancelIoEx(pipe, &pending);
        CloseHandle(pending.hEvent);
    }
    CloseHandle(pipe);
    WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD exitStatus = 4;
    GetExitCodeProcess(launch.hProcess, &exitStatus);
    CloseHandle(launch.hProcess);
    if (result.message == L"无法接收磁盘操作进度。" && exitStatus != 0)
        result.message = L"管理员权限工作进程提前退出，未能读取完整结果。";
    return result;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    policy::Action action = policy::Action::Toggle;
    // Opening an already displayed notification must never reverse protection.
    if (argc == 2 && wcscmp(argv[1], L"--notification-open") == 0) {
        if (argv) LocalFree(argv);
        return 0;
    }
    if (argc >= 2) {
        if (wcscmp(argv[1], L"--worker") == 0) {
            const policy::Action workerAction = argc == 4 ? policy::ParseAction(argv[2]) : policy::Action::Invalid;
            const std::wstring pipe = argc == 4 ? argv[3] : L"";
            if (argv) LocalFree(argv);
            return Worker(workerAction, pipe);
        }
        action = argc == 2 ? policy::ParseAction(argv[1]) : policy::Action::Invalid;
    }
    if (argv) LocalFree(argv);
    struct ComScope {
        bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        ~ComScope() { if (initialized) CoUninitialize(); }
    } com;
    Balloon balloon;
    balloon.Start();
    balloon.Tell(L"USB Guard 正在处理", L"正在识别 U 盘并检查持久只读属性");
    size_t previous = static_cast<size_t>(-1);
    auto result = LaunchWorker(action, [&](size_t done, size_t total, const std::wstring& stage) {
        if (done == previous) return;
        previous = done;
        const unsigned percent = total ? static_cast<unsigned>(100 * done / total) : 0;
        balloon.Tell(L"USB Guard 正在处理", std::to_wstring(percent) + L"% · " + stage);
    });
    const bool success = result.exitCode == 0;
    const std::wstring title = success ? L"USB Guard 操作完成" : L"USB Guard 操作失败";
    balloon.Tell(title, result.message, !success);
    usb::WriteConsoleLine(result.message);
    return result.exitCode;
}
