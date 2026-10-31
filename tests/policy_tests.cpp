#include "../policy.hpp"
#include <cassert>
int main() {
    using namespace policy;
    assert(ParseAction(L"--toggle") == Action::Toggle);
    assert(ParseAction(L"--protect") == Action::Protect);
    assert(ParseAction(L"--unprotect") == Action::Unprotect);
    assert(ParseAction(L"--status") == Action::Status);
    assert(ParseAction(L"--delete") == Action::Invalid);
    assert(DesiredReadOnly(Action::Toggle, false));
    assert(!DesiredReadOnly(Action::Toggle, true));
    assert(DesiredReadOnly(Action::Protect, true));
    assert(!DesiredReadOnly(Action::Unprotect, false));
    const bool yes[] = {true, true, true};
    const bool mixed[] = {true, false, true};
    assert(AllReadOnly(yes, 3));
    assert(!AllReadOnly(mixed, 3));
    assert(!AllReadOnly(nullptr, 0));
    auto other = 0x8000000000000005ULL;
    assert(ReplaceReadOnly(other, true) == 0x9000000000000005ULL);
    assert(ReplaceReadOnly(ReplaceReadOnly(other, true), false) == other);
    return 0;
}
