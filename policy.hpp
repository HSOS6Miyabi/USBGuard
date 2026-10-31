#pragma once

#include <cstddef>
#include <string_view>

namespace policy {

enum class Action { Protect, Unprotect, Toggle, Status, Invalid };

inline Action ParseAction(std::wstring_view argument) {
    if (argument == L"--protect") return Action::Protect;
    if (argument == L"--unprotect") return Action::Unprotect;
    if (argument == L"--toggle") return Action::Toggle;
    if (argument == L"--status") return Action::Status;
    return Action::Invalid;
}

inline bool DesiredReadOnly(Action action, bool allReadOnly) {
    if (action == Action::Protect) return true;
    if (action == Action::Unprotect) return false;
    return !allReadOnly;
}

inline bool AllReadOnly(const bool* flags, std::size_t count) {
    if (!flags || count == 0) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!flags[i]) return false;
    }
    return true;
}

inline constexpr unsigned long long kReadOnlyBit = 0x1000000000000000ULL;

inline unsigned long long ReplaceReadOnly(unsigned long long attributes, bool enable) {
    return enable ? (attributes | kReadOnlyBit) : (attributes & ~kReadOnlyBit);
}

}  // namespace policy
