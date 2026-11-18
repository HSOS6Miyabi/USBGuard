#include "dialog.hpp"

#include <commctrl.h>
#include <cstddef>

namespace ui_dialog {
namespace {

// Use the documented TaskDialogIndirect ABI so the Windows XP toolset can
// compile this file without exposing Vista-only SDK declarations.
// commctrl.h defines these structures with one-byte packing.
#pragma pack(push, 1)
struct TaskDialogButtonCompat {
    int id;
    const wchar_t* text;
};
using TaskDialogCallbackCompat = HRESULT (CALLBACK*)(HWND, UINT, WPARAM, LPARAM, LONG_PTR);
struct TaskDialogConfigCompat {
    UINT size;
    HWND owner;
    HINSTANCE instance;
    UINT flags;
    UINT commonButtons;
    const wchar_t* title;
    const wchar_t* mainIcon;
    const wchar_t* instruction;
    const wchar_t* content;
    UINT buttonCount;
    const TaskDialogButtonCompat* buttons;
    int defaultButton;
    UINT radioCount;
    const TaskDialogButtonCompat* radioButtons;
    int defaultRadio;
    const wchar_t* verification;
    const wchar_t* expandedInformation;
    const wchar_t* expandedControlText;
    const wchar_t* collapsedControlText;
    const wchar_t* footerIcon;
    const wchar_t* footer;
    TaskDialogCallbackCompat callback;
    LONG_PTR callbackData;
    UINT width;
};
#pragma pack(pop)

#if _WIN32_WINNT >= 0x0600
static_assert(sizeof(TaskDialogConfigCompat) == sizeof(TASKDIALOGCONFIG),
              "Task dialog structure must match the Windows SDK");
#endif

using TaskDialogIndirectProc = HRESULT (WINAPI*)(
    const TaskDialogConfigCompat*, int*, int*, BOOL*);

struct LegacyDialogData {
    const wchar_t* title;
    const wchar_t* instruction;
    std::wstring content;
    Severity severity;
    bool confirm;
    HFONT headingFont = nullptr;
};

int ShowNativeDialog(HWND owner, const wchar_t* title,
                     const wchar_t* instruction, const std::wstring& content,
                     Severity severity, bool confirm) {
    HMODULE controls = LoadLibraryW(L"comctl32.dll");
    if (!controls) return -1;
    const auto taskDialog = reinterpret_cast<TaskDialogIndirectProc>(
        GetProcAddress(controls, "TaskDialogIndirect"));
    if (!taskDialog) {
        FreeLibrary(controls);
        return -1;
    }

    TaskDialogConfigCompat config{};
    config.size = sizeof(config);
    config.owner = owner;
    config.flags = 0x0008 | 0x1000; // Cancellation and parent-relative position.
    config.commonButtons = confirm ? 0x0002 | 0x0004 : 0x0001;
    config.defaultButton = confirm ? IDNO : IDOK;
    config.title = title;
    config.instruction = instruction;
    config.content = content.c_str();
    const WORD icon = severity == Severity::Warning ? 0xffff :
                      severity == Severity::Error ? 0xfffe : 0xfffd;
    config.mainIcon = MAKEINTRESOURCEW(icon);

    int selected = IDCANCEL;
    const HRESULT result = taskDialog(&config, &selected, nullptr, nullptr);
    FreeLibrary(controls);
    return SUCCEEDED(result) ? selected : -1;
}

std::wstring NormalizeNewlines(const std::wstring& input) {
    std::wstring output;
    output.reserve(input.size() + 8);
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == L'\n' && (i == 0 || input[i - 1] != L'\r'))
            output += L'\r';
        output += input[i];
    }
    return output;
}

INT_PTR CALLBACK LegacyDialogProc(HWND dialog, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* data = reinterpret_cast<LegacyDialogData*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        SetWindowTextW(dialog, data->title);

        RECT bounds{};
        GetClientRect(dialog, &bounds);
        const int w = bounds.right;
        const int h = bounds.bottom;
        const int margin = 20;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HFONT bodyFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        data->headingFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE,
            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
            L"Segoe UI");
        auto addControl = [&](const wchar_t* type, const wchar_t* caption,
                              DWORD style, DWORD extraStyle,
                              int x, int y, int width, int height,
                              int id, HFONT font) {
            HWND control = CreateWindowExW(extraStyle, type, caption,
                WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                instance, nullptr);
            if (control) SendMessageW(control, WM_SETFONT,
                                      reinterpret_cast<WPARAM>(font), TRUE);
            return control;
        };
        HWND icon = addControl(L"STATIC", L"", SS_ICON, 0, margin, 18,
                               36, 38, 0, bodyFont);
        const wchar_t* iconId = data->severity == Severity::Warning ? IDI_WARNING :
                                data->severity == Severity::Error ? IDI_ERROR : IDI_INFORMATION;
        if (icon) SendMessageW(icon, STM_SETICON,
            reinterpret_cast<WPARAM>(LoadIconW(nullptr, iconId)), 0);
        addControl(L"STATIC", data->instruction, SS_LEFT, 0,
                   margin + 50, 19, w - margin * 2 - 50, 45,
                   0, data->headingFont ? data->headingFont : bodyFont);
        const std::wstring content = NormalizeNewlines(data->content);
        addControl(L"EDIT", content.c_str(),
                   WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                   ES_AUTOVSCROLL, WS_EX_CLIENTEDGE,
                   margin, 69, w - margin * 2, h - 133, 0, bodyFont);
        if (data->confirm) {
            addControl(L"BUTTON", L"继续操作", WS_TABSTOP | BS_PUSHBUTTON,
                       0, w - 254, h - 48, 108, 30, IDYES, bodyFont);
            HWND cancel = addControl(L"BUTTON", L"取消", WS_TABSTOP |
                       BS_DEFPUSHBUTTON, 0, w - 134, h - 48, 108, 30,
                       IDNO, bodyFont);
            SendMessageW(dialog, DM_SETDEFID, IDNO, 0);
            if (cancel) SetFocus(cancel);
        } else {
            HWND ok = addControl(L"BUTTON", L"确定", WS_TABSTOP |
                       BS_DEFPUSHBUTTON, 0, w - 134, h - 48, 108, 30,
                       IDOK, bodyFont);
            SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
            if (ok) SetFocus(ok);
        }
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDYES || LOWORD(wParam) == IDNO ||
            LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wParam));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case WM_DESTROY: {
        auto* data = reinterpret_cast<LegacyDialogData*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        if (data && data->headingFont) {
            DeleteObject(data->headingFont);
            data->headingFont = nullptr;
        }
        return TRUE;
    }
    }
    return FALSE;
}

int ShowLegacyDialog(HWND owner, const wchar_t* title,
                     const wchar_t* instruction, const std::wstring& content,
                     Severity severity, bool confirm) {
    // An empty modal dialog template; the controls are created with the
    // system's GUI font inside WM_INITDIALOG.
    alignas(DWORD) unsigned char bytes[sizeof(DLGTEMPLATE) + 3 * sizeof(WORD)]{};
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(bytes);
    dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    dlg->cdit = 0;
    dlg->cx = 380;
    dlg->cy = 185;
    LegacyDialogData data{title, instruction, content, severity, confirm};
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
        dlg, owner, LegacyDialogProc, reinterpret_cast<LPARAM>(&data));
    return result == -1 ? IDCANCEL : static_cast<int>(result);
}

} // namespace

int Show(HWND owner, const wchar_t* title, const wchar_t* instruction,
         const std::wstring& content, Severity severity, bool confirm) {
    const int result = ShowNativeDialog(owner, title, instruction, content,
                                        severity, confirm);
    return result == -1 ? ShowLegacyDialog(owner, title, instruction, content,
                                           severity, confirm) : result;
}

} // namespace ui_dialog
