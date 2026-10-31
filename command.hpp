#pragma once
#include "engine.hpp"
#include "policy.hpp"
#include <string>
namespace usb {
struct CommandResult {
    int exitCode = 0;
    std::wstring message;
};
CommandResult RunOnSource(policy::Action action, Progress progress = {});
void WriteConsoleLine(const std::wstring& line);
}
