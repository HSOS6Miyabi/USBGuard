#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "command.hpp"
#include <windows.h>
#if defined(_WIN32_WINNT) && _WIN32_WINNT <= 0x0501
// Windows 7.1A SDK headers predate the current C++ parser. These stable
// Ole32 exports exist on XP and need no COM interface declarations.
extern "C" __declspec(dllimport) HRESULT WINAPI CoCreateGuid(GUID*);
extern "C" __declspec(dllimport) int WINAPI StringFromGUID2(REFGUID, LPWSTR, int);
extern "C" __declspec(dllimport) HRESULT WINAPI CoInitializeEx(LPVOID, DWORD);
extern "C" __declspec(dllimport) void WINAPI CoUninitialize();
#ifndef COINIT_APARTMENTTHREADED
#define COINIT_APARTMENTTHREADED 0x2
#endif
#else
#include <objbase.h>
#endif
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// A transient elevated process performs storage operations and reports progress
// over a verified named pipe. The main UI process never requires elevation.
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
// CheckTokenMembership is available on XP and distinguishes an elevated
// administrator token from a filtered token on newer Windows.
bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    // TokenElevation (20) is supported on Vista and newer. XP's SDK does not
    // define this information class, so use the numeric ABI value dynamically.
    struct ElevationInfo { DWORD tokenIsElevated; } elevation{};
    DWORD returned = 0;
    const BOOL modern = GetTokenInformation(
        token, static_cast<TOKEN_INFORMATION_CLASS>(20),
        &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    if (modern) return elevation.tokenIsElevated != 0;

    // XP fallback: the caller's enabled Administrators group membership.
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &adminSid)) return false;
    BOOL member = FALSE;
    const BOOL checked = CheckTokenMembership(nullptr, adminSid, &member);
    FreeSid(adminSid);
    return checked && member;
}

bool VerifyNamedPipePeer(HANDLE pipe, HANDLE worker) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using ClientPidFn = BOOL (WINAPI*)(HANDLE, PULONG);
    auto getClientPid = kernel ? reinterpret_cast<ClientPidFn>(
        GetProcAddress(kernel, "GetNamedPipeClientProcessId")) : nullptr;
    if (getClientPid) {
        using GetProcessIdFn = DWORD (WINAPI*)(HANDLE);
        auto getProcessId = kernel ? reinterpret_cast<GetProcessIdFn>(
            GetProcAddress(kernel, "GetProcessId")) : nullptr;
        ULONG clientPid = 0;
        return getProcessId && getClientPid(pipe, &clientPid) &&
               clientPid == getProcessId(worker);
    }
    // Windows XP cannot query the peer PID. The pipe is a single-client
    // endpoint with a cryptographically unpredictable GUID known only to the
    // launched worker. Do not require the worker to still be running here:
    // it may complete before the UI observes the connection.
    return worker != nullptr && worker != INVALID_HANDLE_VALUE;
}

void CancelPipeOperation(HANDLE pipe, OVERLAPPED* pending) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using CancelFn = BOOL (WINAPI*)(HANDLE, LPOVERLAPPED);
    auto cancelEx = kernel ? reinterpret_cast<CancelFn>(
        GetProcAddress(kernel, "CancelIoEx")) : nullptr;
    if (cancelEx) cancelEx(pipe, pending);
    else CancelIo(pipe);
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
    launch.lpVerb = Elevated() ? L"open" : L"runas";
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
        if (VerifyNamedPipePeer(pipe, launch.hProcess)) {
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
        if (!connected) CancelPipeOperation(pipe, &pending);
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


// Everything below runs in the unelevated UI process. No notification service,
// registry writes, installed components or persistent system settings are used.
namespace {
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
ULONGLONG CompatTickCount() {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using TickFn = ULONGLONG (WINAPI*)();
    auto tick64 = kernel ? reinterpret_cast<TickFn>(
        GetProcAddress(kernel, "GetTickCount64")) : nullptr;
    return tick64 ? tick64() : static_cast<ULONGLONG>(GetTickCount());
}
constexpr UINT kProgressMessage = WM_APP + 201;
constexpr UINT kFinishedMessage = WM_APP + 202;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kCloseTimer = 2;
constexpr DWORD kFinishDurationMs = 4200;
constexpr int kIconResource = 101;

struct MessageData {
    int exitCode = 0;
    unsigned percent = 0;
    std::wstring text;
};

struct MiniState {
    std::wstring heading = L"需要管理员权限";
    std::wstring details = L"请在 Windows 用户账户控制对话框中点击“是”。";
    unsigned percent = 0;
    int exitCode = 4;
    bool receivedProgress = false;
    bool finished = false;
    bool succeeded = false;
    bool dismissed = false;
    bool closeHovered = false;
    bool closePressed = false;
    bool trackingMouse = false;
    bool smokeTest = false;
    bool closeTest = false;
    bool openedDirectory = false;
    bool restoreExplorerPending = false;
    ULONGLONG lastExplorerAttempt = 0;
    policy::Action requestedAction = policy::Action::Invalid;
    std::wstring executableDirectory;
    ULONGLONG finishTick = 0;
    unsigned animation = 0;
    UINT dpi = 96;
    int width = 460;
    int height = 240;
    HFONT fontTitle = nullptr;
    HFONT fontHeading = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HICON icon = nullptr;
    std::thread worker;
};

MiniState g_ui;

// Capture the EXE directory before any storage-volume dismount occurs.
std::wstring ExecutableDirectory() {
    std::vector<wchar_t> path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return L"";
    const std::wstring full(path.data(), length);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return full.substr(0, slash + 1);
}

std::wstring ExplorerArguments(std::wstring directory) {
    // A trailing backslash immediately before a closing quote can be
    // interpreted as escaping that quote. Use an unquoted root drive and
    // remove the final separator from ordinary directory names.
    const bool driveRoot = directory.size() == 3 && directory[1] == L':' &&
                           directory[2] == L'\\';
    if (driveRoot) return L"/n," + directory;
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/'))
        directory.pop_back();
    return L"/n,\"" + directory + L"\"";
}
bool RestoreExplorerWindow(HWND owner) {
    const std::wstring& directory = g_ui.executableDirectory;
    if (directory.empty()) return false;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    // Launch Explorer itself with /n to request a fresh folder window.
    // ShellExecute("open", directory) may hand the request to an existing
    // Explorer instance whose window was closed during the volume dismount.
    wchar_t windowsDirectory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::wstring explorer(windowsDirectory);
    if (explorer.back() != L'\\') explorer += L'\\';
    explorer += L"explorer.exe";
    const std::wstring parameters = ExplorerArguments(directory);
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    launch.hwnd = owner;
    launch.lpVerb = L"open";
    launch.lpFile = explorer.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_SHOWNORMAL;
    const bool started = ShellExecuteExW(&launch) != FALSE;
    if (launch.hProcess) CloseHandle(launch.hProcess);
    return started;
}

void TryRestoreExplorerWindow(HWND owner) {
    if (!g_ui.restoreExplorerPending || g_ui.openedDirectory) return;
    const ULONGLONG now = CompatTickCount();
    // Retry only during the existing completion countdown. Windows can take
    // some time to expose the same drive letter after FSCTL_DISMOUNT_VOLUME.
    if (g_ui.lastExplorerAttempt && now - g_ui.lastExplorerAttempt < 400) return;
    g_ui.lastExplorerAttempt = now;
    if (RestoreExplorerWindow(owner)) {
        g_ui.openedDirectory = true;
        g_ui.restoreExplorerPending = false;
    }
}

int Scale(int logical) {
    return MulDiv(logical, static_cast<int>(g_ui.dpi), 96);
}
COLORREF Color(BYTE r, BYTE g, BYTE b) { return RGB(r, g, b); }

RECT CloseButtonRect() {
    return RECT{g_ui.width - Scale(48), Scale(14),
                g_ui.width - Scale(14), Scale(48)};
}
bool InCloseButton(POINT point) {
    const RECT bounds = CloseButtonRect();
    return PtInRect(&bounds, point) != FALSE;
}

HFONT MakeFont(int pointSize, int weight) {
    return CreateFontW(-MulDiv(pointSize, static_cast<int>(g_ui.dpi), 72),
                       0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}
void CreateFonts() {
    if (g_ui.fontTitle) DeleteObject(g_ui.fontTitle);
    if (g_ui.fontHeading) DeleteObject(g_ui.fontHeading);
    if (g_ui.fontBody) DeleteObject(g_ui.fontBody);
    if (g_ui.fontSmall) DeleteObject(g_ui.fontSmall);
    g_ui.fontTitle = MakeFont(14, FW_BOLD);
    g_ui.fontHeading = MakeFont(16, FW_SEMIBOLD);
    g_ui.fontBody = MakeFont(10, FW_NORMAL);
    g_ui.fontSmall = MakeFont(9, FW_NORMAL);
}

void PaintRounded(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF outline) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ previousBrush = SelectObject(dc, brush);
    HGDIOBJ previousPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, previousPen);
    SelectObject(dc, previousBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void Text(HDC dc, HFONT font, COLORREF color,
          const std::wstring& message, RECT rect, UINT flags) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ previousFont = SelectObject(dc, font);
    DrawTextW(dc, message.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, previousFont);
}

void DrawMiniWindow(HDC dc) {
    const int w = g_ui.width, h = g_ui.height;
    const COLORREF paper = Color(250, 252, 255);
    const COLORREF navy = Color(18, 46, 73);
    const COLORREF muted = Color(93, 111, 132);
    const COLORREF accent = g_ui.finished ?
        (g_ui.succeeded ? Color(12, 153, 118) : Color(214, 81, 77)) :
        Color(21, 157, 197);
    RECT background{0, 0, w, h};
    HBRUSH base = CreateSolidBrush(paper);
    FillRect(dc, &background, base);
    DeleteObject(base);

    RECT top{0, 0, w, Scale(82)};
    HBRUSH navyBrush = CreateSolidBrush(navy);
    FillRect(dc, &top, navyBrush);
    DeleteObject(navyBrush);

    if (g_ui.icon)
        DrawIconEx(dc, Scale(23), Scale(20), g_ui.icon,
                   Scale(42), Scale(42), 0, nullptr, DI_NORMAL);
    Text(dc, g_ui.fontTitle, Color(255,255,255), L"USB GUARD",
         RECT{Scale(78), Scale(19), w-Scale(60), Scale(47)},
         DT_SINGLELINE | DT_VCENTER);
    Text(dc, g_ui.fontSmall, Color(173, 207, 224), L"便携式 U 盘写保护",
         RECT{Scale(78), Scale(47), w-Scale(60), Scale(69)},
         DT_SINGLELINE | DT_VCENTER);

    const RECT close = CloseButtonRect();
    if (g_ui.closeHovered) {
        PaintRounded(dc, close, Scale(12), Color(45, 77, 106), Color(45, 77, 106));
    }
    HPEN closePen = CreatePen(PS_SOLID, (std::max)(1, Scale(2)), Color(235, 244, 250));
    HGDIOBJ previousPen = SelectObject(dc, closePen);
    const int cx = (close.left + close.right) / 2;
    const int cy = (close.top + close.bottom) / 2;
    const int arm = Scale(6);
    MoveToEx(dc, cx - arm, cy - arm, nullptr);
    LineTo(dc, cx + arm, cy + arm);
    MoveToEx(dc, cx + arm, cy - arm, nullptr);
    LineTo(dc, cx - arm, cy + arm);
    SelectObject(dc, previousPen);
    DeleteObject(closePen);

    Text(dc, g_ui.fontHeading, navy, g_ui.heading,
         RECT{Scale(23), Scale(94), w-Scale(96), Scale(122)},
         DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    const std::wstring indicator = g_ui.finished ?
        (g_ui.succeeded ? L"已完成" : L"失败") :
        (g_ui.receivedProgress ? std::to_wstring(g_ui.percent) + L"%" : L"等待授权");
    Text(dc, g_ui.fontSmall, accent, indicator,
         RECT{w-Scale(125), Scale(97), w-Scale(24), Scale(120)},
         DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

    Text(dc, g_ui.fontBody, muted, g_ui.details,
         RECT{Scale(24), Scale(130), w-Scale(24), Scale(180)},
         DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);

    const int bx=Scale(24), by=Scale(185), bw=w-Scale(48), bh=Scale(9);
    PaintRounded(dc, RECT{bx,by,bx+bw,by+bh},bh,
                 Color(226,235,243),Color(226,235,243));
    if (g_ui.finished || g_ui.receivedProgress) {
        const unsigned value = g_ui.finished ? (g_ui.succeeded ? 100u : g_ui.percent) : g_ui.percent;
        const int fill = MulDiv(bw, static_cast<int>((std::min)(value,100u)), 100);
        if (fill > 0)
            PaintRounded(dc, RECT{bx,by,bx+(std::max)(bh,fill),by+bh},
                         bh,accent,accent);
    } else {
        const int sweepWidth=Scale(74);
        const int span=(std::max)(1,bw-sweepWidth);
        const int from = bx + static_cast<int>(g_ui.animation % static_cast<unsigned>(span));
        PaintRounded(dc, RECT{from,by,from+sweepWidth,by+bh},bh,accent,accent);
    }
    std::wstring footer;
    if (!g_ui.finished) footer = L"请等待操作结束，期间不要拔出 U 盘";
    else {
        ULONGLONG elapsed = CompatTickCount() - g_ui.finishTick;
        unsigned seconds = elapsed < kFinishDurationMs
            ? static_cast<unsigned>((kFinishDurationMs - elapsed + 999) / 1000) : 0;
        footer = std::to_wstring(seconds) + L" 秒后自动关闭";
    }
    Text(dc,g_ui.fontSmall,muted,footer,
         RECT{Scale(24),Scale(205),w-Scale(24),Scale(231)},
         DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // Minimal hairline between the header and the body.
    HBRUSH separator=CreateSolidBrush(Color(223,234,243));
    RECT line{0,Scale(81),w,Scale(82)};
    FillRect(dc,&line,separator);
    DeleteObject(separator);
}

void LayoutWindow(HWND window) {
    g_ui.width=Scale(460);
    g_ui.height=Scale(240);
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor,&info)) {
        SystemParametersInfoW(SPI_GETWORKAREA,0,&info.rcWork,0);
    }
    const int margin=Scale(20);
    SetWindowPos(window,HWND_TOPMOST,
        info.rcWork.right-g_ui.width-margin,
        info.rcWork.bottom-g_ui.height-margin,
        g_ui.width,g_ui.height,SWP_NOACTIVATE);
    HRGN region=CreateRoundRectRgn(0,0,g_ui.width+1,g_ui.height+1,Scale(22),Scale(22));
    if (region) {
        if (!SetWindowRgn(window,region,TRUE)) DeleteObject(region);
    }
}
void DismissMiniWindow(HWND window) {
    g_ui.dismissed = true;
    g_ui.closeHovered = false;
    ShowWindow(window, SW_HIDE);
}
void PostUpdate(HWND window, size_t done, size_t total, const std::wstring& stage) {
    auto payload = std::make_unique<MessageData>();
    payload->percent = total ? static_cast<unsigned>((std::min)(done*100/total,size_t{100})) : 0u;
    payload->text = stage;
    if (PostMessageW(window,kProgressMessage,0,reinterpret_cast<LPARAM>(payload.get())))
        payload.release();
}
void PostResult(HWND window, usb::CommandResult result) {
    auto payload=std::make_unique<MessageData>();
    payload->exitCode = result.exitCode;
    payload->text = std::move(result.message);
    if (PostMessageW(window,kFinishedMessage,0,reinterpret_cast<LPARAM>(payload.get())))
        payload.release();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        const POINT point{static_cast<short>(LOWORD(lParam)),
                          static_cast<short>(HIWORD(lParam))};
        const bool hovered = InCloseButton(point);
        if (hovered != g_ui.closeHovered) {
            g_ui.closeHovered = hovered;
            const RECT bounds = CloseButtonRect();
            InvalidateRect(window, &bounds, FALSE);
        }
        if (!g_ui.trackingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            g_ui.trackingMouse = TrackMouseEvent(&tracking) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_ui.trackingMouse = false;
        if (g_ui.closeHovered) {
            g_ui.closeHovered = false;
            const RECT bounds = CloseButtonRect();
            InvalidateRect(window, &bounds, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_ui.closeHovered) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_LBUTTONDOWN: {
        const POINT point{static_cast<short>(LOWORD(lParam)),
                          static_cast<short>(HIWORD(lParam))};
        if (InCloseButton(point)) {
            g_ui.closePressed = true;
            SetCapture(window);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_LBUTTONUP:
        if (g_ui.closePressed) {
            const POINT point{static_cast<short>(LOWORD(lParam)),
                              static_cast<short>(HIWORD(lParam))};
            const bool dismiss = InCloseButton(point);
            g_ui.closePressed = false;
            if (GetCapture() == window) ReleaseCapture();
            if (dismiss) DismissMiniWindow(window);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_CAPTURECHANGED:
        g_ui.closePressed = false;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC target=BeginPaint(window,&paint);
        if (!target) return 0;
        HDC buffer=CreateCompatibleDC(target);
        HBITMAP bitmap=buffer?CreateCompatibleBitmap(target,g_ui.width,g_ui.height):nullptr;
        if (buffer && bitmap) {
            HGDIOBJ previous=SelectObject(buffer,bitmap);
            DrawMiniWindow(buffer);
            BitBlt(target,0,0,g_ui.width,g_ui.height,buffer,0,0,SRCCOPY);
            SelectObject(buffer,previous);
            DeleteObject(bitmap);
        } else DrawMiniWindow(target);
        if (buffer) DeleteDC(buffer);
        EndPaint(window,&paint);
        return 0;
    }
    case kProgressMessage: {
        std::unique_ptr<MessageData> update(reinterpret_cast<MessageData*>(lParam));
        if (!g_ui.finished && update) {
            g_ui.receivedProgress = true;
            g_ui.percent = update->percent;
            g_ui.heading = L"正在切换写保护";
            g_ui.details = std::move(update->text);
            InvalidateRect(window,nullptr,FALSE);
        }
        return 0;
    }
    case kFinishedMessage: {
        std::unique_ptr<MessageData> result(reinterpret_cast<MessageData*>(lParam));
        if (!result || g_ui.finished) return 0;
        g_ui.finished=true;
        g_ui.exitCode=result->exitCode;
        g_ui.succeeded=result->exitCode == 0;
        g_ui.heading=g_ui.succeeded ? L"操作已完成" : L"操作未完成";
        g_ui.details=std::move(result->text);
        g_ui.finishTick=CompatTickCount();
        // Keep Explorer launching in the unelevated UI process. A newly
        // mounted volume may not be available at the instant the worker exits.
        g_ui.restoreExplorerPending = g_ui.succeeded && !g_ui.smokeTest &&
            g_ui.requestedAction != policy::Action::Status;
        if (g_ui.restoreExplorerPending) TryRestoreExplorerWindow(window);
        KillTimer(window,kAnimationTimer);
        SetTimer(window,kCloseTimer,100,nullptr);
        if (!g_ui.dismissed)
            SetWindowPos(window,HWND_TOPMOST,0,0,0,0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (g_ui.closeTest && IsWindowVisible(window)) g_ui.exitCode = 8;
        InvalidateRect(window,nullptr,FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            if (!g_ui.receivedProgress && !g_ui.finished)
                g_ui.animation+=static_cast<unsigned>(Scale(8));
            InvalidateRect(window,nullptr,FALSE);
        } else if (wParam == kCloseTimer) {
            const ULONGLONG elapsed = CompatTickCount() - g_ui.finishTick;
            if (g_ui.restoreExplorerPending && elapsed < 3200) {
                TryRestoreExplorerWindow(window);
            } else if (g_ui.restoreExplorerPending) {
                g_ui.restoreExplorerPending = false;
                g_ui.details += L"\n未能自动打开程序目录，请手动在资源管理器中打开。";
            }
            if (elapsed >= (g_ui.smokeTest ? 500u : kFinishDurationMs)) {
                KillTimer(window,kCloseTimer);
                DestroyWindow(window);
            } else InvalidateRect(window,nullptr,FALSE);
        }
        return 0;
    case WM_DPICHANGED: {
        const UINT newDpi=HIWORD(wParam);
        if (newDpi && newDpi!=g_ui.dpi) {
            g_ui.dpi=newDpi;
            CreateFonts();
            LayoutWindow(window);
            InvalidateRect(window,nullptr,FALSE);
        }
        return 0;
    }
    case WM_CLOSE:
        DismissMiniWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window,kAnimationTimer);
        KillTimer(window,kCloseTimer);
        if (g_ui.worker.joinable()) g_ui.worker.join();
        PostQuitMessage(g_ui.exitCode);
        return 0;
    default: return DefWindowProcW(window,message,wParam,lParam);
    }
}

int RunMiniWindow(HINSTANCE instance,policy::Action action,bool smokeTest,bool closeTest) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using DpiContextFn = BOOL (WINAPI*)(HANDLE);
    auto dpiFn = user32 ? reinterpret_cast<DpiContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(
        static_cast<INT_PTR>(-4));
    if (!dpiFn || !dpiFn(perMonitorV2)) {
        using DpiAwareFn = BOOL (WINAPI*)();
        auto dpiAware = user32 ? reinterpret_cast<DpiAwareFn>(
            GetProcAddress(user32, "SetProcessDPIAware")) : nullptr;
        if (dpiAware) dpiAware();
    }

    HDC screen=GetDC(nullptr);
    if (screen) {
        const int xdpi=GetDeviceCaps(screen,LOGPIXELSX);
        if (xdpi>0) g_ui.dpi=static_cast<UINT>(xdpi);
        ReleaseDC(nullptr,screen);
    }
    g_ui.smokeTest=smokeTest;
    g_ui.closeTest=closeTest;
    g_ui.requestedAction=action;
    g_ui.executableDirectory=ExecutableDirectory();
    CreateFonts();
    g_ui.icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(kIconResource),
                    IMAGE_ICON,Scale(42),Scale(42),LR_DEFAULTCOLOR));
    WNDCLASSEXW klass{};
    klass.cbSize=sizeof(klass);
    klass.lpfnWndProc=WindowProc;
    klass.hInstance=instance;
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    klass.lpszClassName=L"USBGuardMiniWindow";
    klass.hIcon=g_ui.icon;
    klass.hbrBackground=nullptr;
    if (!RegisterClassExW(&klass)) return 5;
    HWND window=CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
               klass.lpszClassName,L"USB Guard",WS_POPUP,
               0,0,Scale(460),Scale(240),nullptr,nullptr,instance,nullptr);
    if (!window) return 5;
    LayoutWindow(window);
    ShowWindow(window,SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    SetTimer(window,kAnimationTimer,80,nullptr);

    if (closeTest) {
        const RECT bounds = CloseButtonRect();
        const LPARAM click = MAKELPARAM((bounds.left + bounds.right) / 2,
                                       (bounds.top + bounds.bottom) / 2);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, click);
        SendMessageW(window, WM_LBUTTONUP, 0, click);
        if (!g_ui.dismissed || IsWindowVisible(window)) {
            DestroyWindow(window);
            return 8;
        }
    }
    if (smokeTest) {
        if (ExplorerArguments(L"E:\\") != L"/n,E:\\" ||
            ExplorerArguments(L"E:\\Example Folder\\") != L"/n,\"E:\\Example Folder\"") {
            DestroyWindow(window);
            return 8;
        }
        PostUpdate(window,1,2,L"正在测试窗口绘制和进度更新");
        PostResult(window,{0,L"界面绘制、进度和结束状态检查完成。"});
    } else {
        g_ui.worker = std::thread([window,action] {
            // ShellExecuteExW is invoked on a COM-initialized STA worker thread.
            const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            auto result = LaunchWorker(action,[window](size_t done,size_t total,const std::wstring& stage){
                PostUpdate(window,done,total,stage);
            });
            if (SUCCEEDED(initialized)) CoUninitialize();
            usb::WriteConsoleLine(result.message);
            PostResult(window,std::move(result));
        });
    }
    MSG msg{};
    while (GetMessageW(&msg,nullptr,0,0)>0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_ui.icon) DestroyIcon(g_ui.icon);
    if (g_ui.fontTitle) DeleteObject(g_ui.fontTitle);
    if (g_ui.fontHeading) DeleteObject(g_ui.fontHeading);
    if (g_ui.fontBody) DeleteObject(g_ui.fontBody);
    if (g_ui.fontSmall) DeleteObject(g_ui.fontSmall);
    return static_cast<int>(msg.wParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    policy::Action action=policy::Action::Toggle;
    bool smokeTest=false;
    bool closeTest=false;
    if (argv && argc>=2) {
        if (wcscmp(argv[1],L"--worker")==0) {
            const policy::Action workerAction=argc==4?policy::ParseAction(argv[2]):policy::Action::Invalid;
            const std::wstring pipe=argc==4?argv[3]:L"";
            LocalFree(argv);
            return Worker(workerAction,pipe);
        }
        closeTest = argc==2 && wcscmp(argv[1],L"--ui-close-test")==0;
        smokeTest = closeTest || (argc==2 && wcscmp(argv[1],L"--ui-self-test")==0);
        if (!smokeTest)
            action=argc==2?policy::ParseAction(argv[1]):policy::Action::Invalid;
    }
    if (argv) LocalFree(argv);

    // No persistent state. Multiple concurrent attempts must not toggle the same disk.
    HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\USBGuardMini.SingleOperation");
    if (!mutex) return 4;
    if (GetLastError()==ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    const int code=RunMiniWindow(instance,action,smokeTest,closeTest);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
