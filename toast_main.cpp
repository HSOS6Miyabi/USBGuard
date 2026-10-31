#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "command.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propsys.h>
#include <wrl/client.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.ui.notifications.h>
#include <windows.foundation.collections.h>
#include <windows.data.xml.dom.h>
#include <commctrl.h>
#include <algorithm>
#include <iterator>
#include <sddl.h>
#include <string>
#include <vector>

// Windows Runtime APIs are loaded at execution time so the same executable starts on Windows 7.
namespace {
using Microsoft::WRL::ComPtr;
namespace N = ABI::Windows::UI::Notifications;
namespace X = ABI::Windows::Data::Xml::Dom;
using Map = ABI::Windows::Foundation::Collections::IMap<HSTRING, HSTRING>;
constexpr wchar_t NOTIFICATION_TAG[] = L"diskstate";
constexpr int MAIN_ICON = 101;

struct Rt {
    HMODULE dll = nullptr;
    decltype(&RoInitialize) initialize = nullptr;
    decltype(&RoUninitialize) uninitialize = nullptr;
    decltype(&RoGetActivationFactory) factory = nullptr;
    decltype(&RoActivateInstance) activate = nullptr;
    decltype(&WindowsCreateString) createString = nullptr;
    decltype(&WindowsDeleteString) deleteString = nullptr;
    bool initialized = false;
    bool Load() {
        dll = LoadLibraryW(L"combase.dll");
        if (!dll) return false;
        initialize = reinterpret_cast<decltype(initialize)>(GetProcAddress(dll, "RoInitialize"));
        uninitialize = reinterpret_cast<decltype(uninitialize)>(GetProcAddress(dll, "RoUninitialize"));
        factory = reinterpret_cast<decltype(factory)>(GetProcAddress(dll, "RoGetActivationFactory"));
        activate = reinterpret_cast<decltype(activate)>(GetProcAddress(dll, "RoActivateInstance"));
        createString = reinterpret_cast<decltype(createString)>(GetProcAddress(dll, "WindowsCreateString"));
        deleteString = reinterpret_cast<decltype(deleteString)>(GetProcAddress(dll, "WindowsDeleteString"));
        if (!initialize || !uninitialize || !factory || !activate || !createString || !deleteString)
            return false;
        const HRESULT hr = initialize(RO_INIT_SINGLETHREADED);
        initialized = SUCCEEDED(hr);
        return initialized;
    }
    ~Rt() {
        if (initialized) uninitialize();
        if (dll) FreeLibrary(dll);
    }
};
struct HStr {
    Rt& rt;
    HSTRING raw = nullptr;
    HStr(Rt& r, const std::wstring& text) : rt(r) {
        if (!text.empty()) rt.createString(text.c_str(), static_cast<UINT32>(text.size()), &raw);
    }
    HStr(Rt& r, const wchar_t* text) : HStr(r, std::wstring(text)) {}
    HStr(const HStr&) = delete;
    HStr& operator=(const HStr&) = delete;
    ~HStr() { if (raw) rt.deleteString(raw); }
};
DWORD WindowsBuild() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto function = reinterpret_cast<VersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof(v);
    if (!function || function(&v) != 0) return 0;
    return v.dwMajorVersion >= 10 ? v.dwBuildNumber : (v.dwMajorVersion * 100 + v.dwMinorVersion);
}
std::wstring XmlEscape(const std::wstring& input) {
    std::wstring result; result.reserve(input.size());
    for (wchar_t c : input) {
        switch (c) {
            case L'&': result += L"&amp;"; break;
            case L'<': result += L"&lt;"; break;
            case L'>': result += L"&gt;"; break;
            case L'\'': result += L"&apos;"; break;
            case L'\"': result += L"&quot;"; break;
            default: if (c >= 0x20 || c == L'\n' || c == L'\t') result += c;
        }
    }
    return result;
}

// A unique shortcut exists only while this process publishes notifications.
// Keeping ownership inside the object also removes the file when setup fails.
struct TemporaryNotificationIdentity {
    std::wstring shortcutPath;
    std::wstring appId;

    bool Create() {
        wchar_t exe[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (!length || length >= std::size(exe)) return false;
        GUID uuid{};
        if (FAILED(CoCreateGuid(&uuid))) return false;
        wchar_t guid[40]{};
        if (!StringFromGUID2(uuid, guid, static_cast<int>(std::size(guid)))) return false;
        std::wstring token;
        for (wchar_t c : guid) {
            if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') ||
                (c >= L'a' && c <= L'f')) token += c;
        }
        if (token.size() != 32) return false;
        appId = L"PortableUSBGuard.Notifications";
        PWSTR folder = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &folder))) return false;
        shortcutPath = folder;
        CoTaskMemFree(folder);
        shortcutPath += L"\\USBGuard-Session-" + token + L".lnk";
        // Never overwrite an existing file, even if the generated identity collides.
        if (GetFileAttributesW(shortcutPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            shortcutPath.clear();
            return false;
        }
        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&link))) || FAILED(link->SetPath(exe)) ||
            FAILED(link->SetDescription(L"USB Guard 临时进度通知"))) return false;
        ComPtr<IPropertyStore> store;
        if (FAILED(link.As(&store))) return false;
        PROPVARIANT value{};
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<PWSTR>(appId.c_str());
        if (FAILED(store->SetValue(PKEY_AppUserModel_ID, value)) ||
            FAILED(store->Commit())) return false;
        ComPtr<IPersistFile> file;
        if (FAILED(link.As(&file)) || FAILED(file->Save(shortcutPath.c_str(), TRUE))) return false;
        return SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(appId.c_str()));
    }
    ~TemporaryNotificationIdentity() {
        if (!shortcutPath.empty()) DeleteFileW(shortcutPath.c_str());
    }
};

struct NativeToast {
    TemporaryNotificationIdentity identity;
    Rt runtime;
    ComPtr<N::IToastNotifier> notifier;
    ComPtr<N::IToastNotificationHistory> history;
    bool available = false;
    bool progressStarted = false;
    unsigned sequence = 1;

    bool Setup(DWORD build) {
        // NotificationData / Toast progress needs Windows 10 version 1703.
        if (build < 15063 || !runtime.Load() || !identity.Create()) return false;
        HStr className(runtime, RuntimeClass_Windows_UI_Notifications_ToastNotificationManager);
        HStr appId(runtime, identity.appId);
        ComPtr<N::IToastNotificationManagerStatics> manager;
        if (FAILED(runtime.factory(className.raw, IID_PPV_ARGS(&manager))) ||
            FAILED(manager->CreateToastNotifierWithId(appId.raw, &notifier))) return false;
        // Stable AppUserModelID lets us remove earlier USBGuard notifications.
        ComPtr<N::IToastNotificationManagerStatics2> manager2;
        if (SUCCEEDED(manager.As(&manager2)) && manager2)
            manager2->get_History(&history);
        if (history) history->ClearWithId(appId.raw);
        available = true;
        return true;
    }

    bool CreateData(double fraction, const std::wstring& heading,
                    const std::wstring& details, const std::wstring& status,
                    ComPtr<N::INotificationData>& output) {
        HStr name(runtime, RuntimeClass_Windows_UI_Notifications_NotificationData);
        ComPtr<IInspectable> instance;
        if (FAILED(runtime.activate(name.raw, instance.GetAddressOf())) ||
            FAILED(instance.As(&output))) return false;
        ComPtr<Map> values;
        if (FAILED(output->get_Values(&values))) return false;
        const std::pair<const wchar_t*, std::wstring> parts[] = {
            {L"heading", heading},
            {L"details", details},
            {L"progressValue", std::to_wstring((std::max)(0.0, (std::min)(fraction, 1.0)))},
            {L"progressStatus", status}
        };
        for (const auto& item : parts) {
            HStr key(runtime, item.first), value(runtime, item.second);
            boolean replaced = false;
            if (FAILED(values->Insert(key.raw, value.raw, &replaced))) return false;
        }
        return SUCCEEDED(output->put_SequenceNumber(sequence++));
    }

    bool Show(const std::wstring& title, const std::wstring& details, bool showProgress) {
        if (!available) return false;
        std::wstring xml = L"<toast duration='long' launch='--notification-open'><visual><binding template='ToastGeneric'>";
        if (showProgress) {
            xml += L"<text>{heading}</text><text>{details}</text>"
                   L"<progress title='U 盘写保护操作' value='{progressValue}' status='{progressStatus}'/>";
        } else {
            xml += L"<text>" + XmlEscape(title) + L"</text><text>" + XmlEscape(details) + L"</text>";
        }
        xml += L"</binding></visual></toast>";
        HStr xmlClass(runtime, RuntimeClass_Windows_Data_Xml_Dom_XmlDocument), xmlText(runtime, xml);
        ComPtr<IInspectable> instance;
        if (FAILED(runtime.activate(xmlClass.raw, instance.GetAddressOf()))) return false;
        ComPtr<X::IXmlDocument> doc;
        ComPtr<X::IXmlDocumentIO> docIo;
        if (FAILED(instance.As(&doc)) || FAILED(instance.As(&docIo)) ||
            FAILED(docIo->LoadXml(xmlText.raw))) return false;
        HStr toastClass(runtime, RuntimeClass_Windows_UI_Notifications_ToastNotification);
        ComPtr<N::IToastNotificationFactory> factory;
        if (FAILED(runtime.factory(toastClass.raw, IID_PPV_ARGS(&factory)))) return false;
        ComPtr<N::IToastNotification> item;
        if (FAILED(factory->CreateToastNotification(doc.Get(), &item))) return false;
        ComPtr<N::IToastNotification2> tagged;
        HStr tag(runtime, NOTIFICATION_TAG);
        if (FAILED(item.As(&tagged)) || FAILED(tagged->put_Tag(tag.raw))) return false;
        if (showProgress) {
            ComPtr<N::IToastNotification4> dataToast;
            ComPtr<N::INotificationData> data;
            if (FAILED(item.As(&dataToast)) ||
                !CreateData(0.0, title, details, L"等待管理员授权", data) ||
                FAILED(dataToast->put_Data(data.Get()))) return false;
        }
        const bool shown = SUCCEEDED(notifier->Show(item.Get()));
        if (shown && showProgress) progressStarted = true;
        return shown;
    }

    bool Update(size_t done, size_t total, const std::wstring& stage) {
        if (!available || !progressStarted) return false;
        ComPtr<N::IToastNotifier2> updater;
        if (FAILED(notifier.As(&updater))) return false;
        ComPtr<N::INotificationData> data;
        if (!CreateData(total ? static_cast<double>(done) / total : 0.0,
                        L"USB Guard 正在处理", stage, stage, data)) return false;
        HStr tag(runtime, NOTIFICATION_TAG);
        N::NotificationUpdateResult result{};
        return SUCCEEDED(updater->UpdateWithTag(data.Get(), tag.raw, &result)) &&
               static_cast<int>(result) == 0; // Succeeded, not NotificationNotFound.
    }

    bool Finish(const std::wstring& title, const std::wstring& details) {
        if (!available) return false;
        // Remove the progress item; do not leave a stale progress card in history.
        if (history) {
            HStr appId(runtime, identity.appId), tag(runtime, NOTIFICATION_TAG);
            history->RemoveGroupedTagWithId(tag.raw, nullptr, appId.raw);
        }
        progressStarted = false;
        // Same tag and app identity: a single completion entry remains in history.
        return Show(title, details, false);
    }
};

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
    const DWORD build = WindowsBuild();
    NativeToast toast;
    const bool modern = toast.Setup(build);
    Balloon balloon;
    // Shell balloons are exclusively for Windows releases without WinRT progress.
    if (!modern && build < 15063) balloon.Start();
    const std::wstring consent = L"请在 Windows 用户账户控制对话框中点击“是”，允许修改 U 盘写保护设置。";
    if (modern) {
        toast.Show(L"USB Guard 需要管理员权限", consent, true);
    } else if (build < 15063) {
        balloon.Tell(L"USB Guard 需要管理员权限", consent);
    }
    // UAC is the sole confirmation; no additional dialog or artificial delay.
    auto result = LaunchWorker(action, [&](size_t done, size_t total, const std::wstring& stage) {
        if (modern) toast.Update(done, total, stage);
    });
    const bool success = result.exitCode == 0;
    const std::wstring title = success ? L"USB Guard 操作完成" : L"USB Guard 操作失败";
    if (modern) {
        toast.Finish(title, result.message);
    } else if (build < 15063) {
        balloon.Tell(title, result.message, !success);
    }
    usb::WriteConsoleLine(result.message);
    return result.exitCode;
}
