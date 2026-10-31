#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddvol.h>
#include <functional>
#include <set>
#include <string>
#include <vector>
#include "policy.hpp"

namespace usb {
constexpr ULONGLONG kReadOnly = policy::kReadOnlyBit;
struct Volume {
    std::wstring devicePath;
    std::wstring mountPoints;
    std::wstring fileSystem;
    DWORD diskNumber = MAXDWORD;
    bool readonly = false;
    ULONGLONG attributes = 0;
    bool selected = true;
};
struct Disk {
    DWORD number = 0;
    PARTITION_STYLE style = PARTITION_STYLE_RAW;
    ULONGLONG size = 0;
    std::vector<Volume> volumes;
    bool sourceDisk = false;
};
using Progress = std::function<void(size_t completed, size_t total, const std::wstring& stage)>;
extern std::vector<Disk> g_disks;
extern DWORD g_executableDisk;
extern std::set<DWORD> g_windowsDisks;
std::wstring ErrorText(DWORD error);
bool Discover(std::wstring& error);
Disk* SourceDisk();
bool ChangeDisk(Disk& disk, policy::Action action, std::wstring& summary, Progress progress = {});
std::wstring DiskLabel(const Disk& disk);
}
