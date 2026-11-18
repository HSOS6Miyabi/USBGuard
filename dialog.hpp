#pragma once

#include <windows.h>
#include <string>

namespace ui_dialog {

enum class Severity { Information, Warning, Error };

// Returns IDYES for an accepted confirmation and IDNO/IDCANCEL otherwise.
int Show(HWND owner, const wchar_t* title, const wchar_t* instruction,
         const std::wstring& content, Severity severity, bool confirm = false);

} // namespace ui_dialog
