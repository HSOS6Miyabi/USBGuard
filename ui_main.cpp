#include "engine.hpp"
#include "command.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace {
constexpr int ICON_MAIN = 101;
constexpr int C_DISK = 201, C_VOLS = 202, C_REFRESH = 203, C_ENABLE = 204,
              C_DISABLE = 205, C_TOGGLE = 206, C_STATUS = 207, C_PROGRESS = 208;
constexpr COLORREF NAVY = RGB(18, 45, 77), BLUE = RGB(41, 105, 175), CYAN = RGB(21, 154, 158),
                   TEXT = RGB(28, 48, 70), SUB = RGB(99, 113, 128), BG = RGB(244, 247, 251),
                   WHITE = RGB(255, 255, 255), BORDER = RGB(222, 230, 238);
HWND window = nullptr, combo = nullptr, list = nullptr, status = nullptr, progress = nullptr;
HFONT titleFont = nullptr, headingFont = nullptr, normalFont = nullptr, smallFont = nullptr;
HBRUSH whiteBrush = nullptr, bgBrush = nullptr, navyBrush = nullptr;
std::vector<HWND> actionButtons;
bool filling = false, busy = false;
int g_dpi = 96;
int S(int n) { return MulDiv(n, g_dpi, 96); }
void Font(HWND h, HFONT f) { if (h && f) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE); }
HFONT NewFont(int size, int weight) {
    return CreateFontW(-S(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}
HWND Control(const wchar_t* cls, const wchar_t* caption, DWORD styles, int x, int y,
             int w, int h, int id, HFONT f = nullptr, DWORD ex = 0) {
    auto result = CreateWindowExW(ex, cls, caption, WS_CHILD | WS_VISIBLE | styles,
        S(x), S(y), S(w), S(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    Font(result, f ? f : normalFont);
    return result;
}
usb::Disk* SelectedDisk() {
    const int i = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (i < 0 || static_cast<size_t>(i) >= usb::g_disks.size()) return nullptr;
    return &usb::g_disks[i];
}
void Status(const std::wstring& value) { SetWindowTextW(status, value.c_str()); }
void SetBusy(bool yes) {
    busy = yes;
    for (auto h : actionButtons) EnableWindow(h, !yes);
    EnableWindow(combo, !yes);
    EnableWindow(list, !yes);
}
void Populate() {
    filling = true;
    ListView_DeleteAllItems(list);
    usb::Disk* disk = SelectedDisk();
    if (disk) {
        for (size_t i = 0; i < disk->volumes.size(); ++i) {
            const auto& v = disk->volumes[i];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(v.mountPoints.c_str());
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, static_cast<int>(i), 1, const_cast<wchar_t*>(v.fileSystem.c_str()));
            ListView_SetItemText(list, static_cast<int>(i), 2, const_cast<wchar_t*>(v.readonly ? L"已保护" : L"可写入"));
            ListView_SetCheckState(list, static_cast<int>(i), v.selected ? TRUE : FALSE);
        }
        Status(disk->style == PARTITION_STYLE_MBR
            ? L"MBR：单次设置整个磁盘的持久属性，影响该盘全部分区。"
            : L"GPT：设置勾选分区的持久只读属性，未识别的分区保持原状。");
    } else Status(L"尚未选择磁盘。请将程序放在 U 盘中运行，或手动选择安全的 USB 设备。");
    SendMessageW(progress, PBM_SETPOS, 0, 0);
    filling = false;
}
void Refresh(DWORD preferred = MAXDWORD) {
    const auto* old = SelectedDisk();
    if (preferred == MAXDWORD && old) preferred = old->number;
    std::wstring warning;
    if (!usb::Discover(warning)) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        Populate(); Status(warning); return;
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int chosen = -1;
    for (size_t i = 0; i < usb::g_disks.size(); ++i) {
        const auto label = usb::DiskLabel(usb::g_disks[i]);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (preferred == usb::g_disks[i].number) chosen = static_cast<int>(i);
    }
    if (chosen < 0) {
        for (size_t i = 0; i < usb::g_disks.size(); ++i)
            if (usb::g_disks[i].sourceDisk) { chosen = static_cast<int>(i); break; }
    }
    if (chosen >= 0) SendMessageW(combo, CB_SETCURSEL, chosen, 0);
    Populate();
    if (chosen < 0 && !warning.empty()) Status(warning);
}
void DoAction(policy::Action action) {
    usb::Disk* disk = SelectedDisk();
    if (!disk || busy) return;
    if (disk->style == PARTITION_STYLE_MBR) {
        for (const auto& v : disk->volumes) if (!v.selected) {
            MessageBoxW(window, L"MBR 的整盘只读属性会影响所有分区，请勾选所有列出的卷。",
                        L"无法进行设置", MB_OK | MB_ICONWARNING); return;
        }
    }
    std::wstring question = L"确认要操作以下 USB 磁盘吗？\n\n" + usb::DiskLabel(*disk) +
          L"\n\n此操作会修改磁盘上持久保存的只读属性。请先保存所有文件。";
    if (MessageBoxW(window, question.c_str(), L"确认目标磁盘", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const DWORD number = disk->number;
    SetBusy(true);
    Status(L"正在读取并更新持久只读属性……");
    std::wstring report;
    bool ok = usb::ChangeDisk(*disk, action, report,
       [](size_t done, size_t total, const std::wstring& stage) {
           SendMessageW(progress, PBM_SETPOS,
                        total ? static_cast<WPARAM>(done * 100 / total) : 0, 0);
           Status(stage);
           UpdateWindow(progress);
           UpdateWindow(status);
       });
    SendMessageW(progress, PBM_SETPOS, ok ? 100 : 0, 0);
    SetBusy(false);
    Status(report);
    MessageBoxW(window, report.c_str(), ok ? L"操作已完成" : L"操作失败",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    Refresh(number);
    Status(report);
    SendMessageW(progress, PBM_SETPOS, ok ? 100 : 0, 0);
}
void FillRound(HDC dc, RECT r, int radius, COLORREF fill, COLORREF outline) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, S(1), outline);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(radius), S(radius));
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(p); DeleteObject(b);
}
LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        window = h;
        titleFont = NewFont(25, FW_BOLD); headingFont = NewFont(15, FW_SEMIBOLD);
        normalFont = NewFont(12, FW_NORMAL); smallFont = NewFont(10, FW_NORMAL);
        whiteBrush = CreateSolidBrush(WHITE); bgBrush = CreateSolidBrush(BG); navyBrush = CreateSolidBrush(NAVY);
        HWND logo = Control(L"STATIC", L"", SS_ICON, 32, 35, 58, 58, 0);
        HICON heroIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(ICON_MAIN), IMAGE_ICON, S(48), S(48), LR_DEFAULTCOLOR | LR_SHARED));
        if (heroIcon) SendMessageW(logo, STM_SETICON, reinterpret_cast<WPARAM>(heroIcon), 0);
        Control(L"STATIC", L"USB GUARD", SS_LEFT, 105, 28, 420, 40, 0, titleFont);
        Control(L"STATIC", L"便携式磁盘写保护管理", SS_LEFT, 106, 78, 630, 28, 0, normalFont);
        Control(L"STATIC", L"目标 USB 磁盘", SS_LEFT, 34, 146, 540, 28, 0, headingFont);
        combo = Control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                        34, 182, 640, 260, C_DISK);
        HWND refresh = Control(L"BUTTON", L"重新扫描", BS_OWNERDRAW, 690, 181, 148, 36, C_REFRESH);
        actionButtons.push_back(refresh);
        Control(L"STATIC", L"磁盘中的可识别卷", SS_LEFT, 34, 254, 520, 28, 0, headingFont);
        Control(L"STATIC", L"默认全部选中；MBR 将一次设置整个磁盘。", SS_LEFT,
                34, 281, 710, 21, 0, smallFont);
        list = Control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                       34, 315, 804, 185, C_VOLS, normalFont, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(list, WHITE);
        ListView_SetTextBkColor(list, WHITE);
        ListView_SetTextColor(list, TEXT);
        const wchar_t* names[] = {L"盘符 / 挂载点", L"文件系统", L"状态"};
        const int widths[] = {S(487), S(172), S(120)};
        for (int i = 0; i != 3; ++i) {
            LVCOLUMNW col{}; col.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
            col.iSubItem = i; col.cx = widths[i]; col.pszText = const_cast<wchar_t*>(names[i]);
            ListView_InsertColumn(list, i, &col);
        }
        HWND enable = Control(L"BUTTON", L"启用只读保护", BS_OWNERDRAW, 34, 525, 245, 49, C_ENABLE);
        HWND disable = Control(L"BUTTON", L"解除只读保护", BS_OWNERDRAW, 295, 525, 245, 49, C_DISABLE);
        HWND toggle = Control(L"BUTTON", L"切换当前状态", BS_OWNERDRAW, 556, 525, 282, 49, C_TOGGLE);
        actionButtons.insert(actionButtons.end(), {enable, disable, toggle});
        progress = Control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 34, 597, 804, 8, C_PROGRESS);
        SendMessageW(progress, PBM_SETRANGE32, 0, 100);
        SendMessageW(progress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(CYAN));
        status = Control(L"STATIC", L"正在识别设备……", SS_LEFT, 34, 622, 804, 46, C_STATUS, smallFont);
        Refresh(usb::g_executableDisk);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps);
        RECT all{}; GetClientRect(h, &all);
        HBRUSH bg = CreateSolidBrush(BG); FillRect(dc, &all, bg); DeleteObject(bg);
        RECT hero{0, 0, all.right, S(124)};
        HBRUSH navy = CreateSolidBrush(NAVY); FillRect(dc, &hero, navy); DeleteObject(navy);
        RECT line{0, S(121), all.right, S(124)};
        HBRUSH accent = CreateSolidBrush(CYAN); FillRect(dc, &line, accent); DeleteObject(accent);
        FillRound(dc, RECT{S(18), S(132), S(854), S(236)}, 17, WHITE, BORDER);
        FillRound(dc, RECT{S(18), S(241), S(854), S(678)}, 17, WHITE, BORDER);
        EndPaint(h, &ps); return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT r{}; GetWindowRect(reinterpret_cast<HWND>(lp), &r);
        POINT pt{r.left, r.top}; ScreenToClient(h, &pt);
        bool hero = pt.y < S(124);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, hero ? WHITE : TEXT);
        return reinterpret_cast<INT_PTR>(hero ? navyBrush : whiteBrush);
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* info = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!info || info->CtlType != ODT_BUTTON) break;
        const bool disabled = (info->itemState & ODS_DISABLED) != 0;
        const bool pressed = (info->itemState & ODS_SELECTED) != 0;
        const bool primary = info->CtlID == C_ENABLE;
        const COLORREF fill = disabled ? RGB(215, 222, 229) :
           primary ? (pressed ? RGB(13, 98, 104) : CYAN) :
           (pressed ? RGB(224, 233, 243) : RGB(236, 242, 249));
        const COLORREF text = primary && !disabled ? WHITE : TEXT;
        FillRound(info->hDC, info->rcItem, 12, fill, fill);
        SetBkMode(info->hDC, TRANSPARENT); SetTextColor(info->hDC, text);
        HFONT f = reinterpret_cast<HFONT>(SendMessageW(info->hwndItem, WM_GETFONT, 0, 0));
        HGDIOBJ old = SelectObject(info->hDC, f);
        wchar_t caption[120]{}; GetWindowTextW(info->hwndItem, caption, 120);
        RECT r = info->rcItem;
        DrawTextW(info->hDC, caption, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(info->hDC, old); return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case C_DISK: if (HIWORD(wp) == CBN_SELCHANGE) Populate(); break;
        case C_REFRESH: if (!busy) Refresh(); break;
        case C_ENABLE: DoAction(policy::Action::Protect); break;
        case C_DISABLE: DoAction(policy::Action::Unprotect); break;
        case C_TOGGLE: DoAction(policy::Action::Toggle); break;
        } return 0;
    case WM_NOTIFY: {
        const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (!filling && !busy && hdr && hdr->idFrom == C_VOLS && hdr->code == LVN_ITEMCHANGED) {
            auto* state = reinterpret_cast<const NMLISTVIEW*>(lp);
            auto* disk = SelectedDisk();
            if (disk && state->iItem >= 0 && static_cast<size_t>(state->iItem) < disk->volumes.size() &&
                ((state->uNewState ^ state->uOldState) & LVIS_STATEIMAGEMASK))
                disk->volumes[state->iItem].selected = ListView_GetCheckState(list, state->iItem) != FALSE;
        }
        return 0;
    }
    case WM_DESTROY:
        for (auto f : {titleFont, headingFont, normalFont, smallFont}) if (f) DeleteObject(f);
        if (whiteBrush) DeleteObject(whiteBrush);
        if (bgBrush) DeleteObject(bgBrush);
        if (navyBrush) DeleteObject(navyBrush);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0; LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        const auto action = argc == 2 ? policy::ParseAction(args[1]) : policy::Action::Invalid;
        if (args) LocalFree(args);
        auto result = usb::RunOnSource(action);
        usb::WriteConsoleLine(result.message);
        return result.exitCode;
    }
    if (args) LocalFree(args);
    SetProcessDPIAware();
    HDC desktop = GetDC(nullptr);
    if (desktop) { g_dpi = GetDeviceCaps(desktop, LOGPIXELSX); ReleaseDC(nullptr, desktop); }
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.hInstance = instance;
    wc.lpfnWndProc = WindowProc; wc.lpszClassName = L"USBGuardUI.Win32";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(ICON_MAIN));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(ICON_MAIN), IMAGE_ICON, 16, 16, 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return 5;
    RECT rect{0, 0, S(872), S(702)};
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    window = CreateWindowExW(0, wc.lpszClassName, L"USB Guard  |  便携式写保护",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 5;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
