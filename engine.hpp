#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#if defined(_WIN32_WINNT) && _WIN32_WINNT <= 0x0501
// The Windows 7.1A SDK bundled with v141_xp lacks ntddvol.h.
// These XP-era IOCTL constants and structures retain their Windows ABI.
#ifndef IOCTL_VOLUME_SET_GPT_ATTRIBUTES
#define IOCTL_VOLUME_SET_GPT_ATTRIBUTES CTL_CODE((DWORD)'V', 13, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _VOLUME_SET_GPT_ATTRIBUTES_INFORMATION {
    ULONGLONG GptAttributes;
    BOOLEAN RevertOnClose;
    BOOLEAN ApplyToAllConnectedVolumes;
    USHORT Reserved1;
    ULONG Reserved2;
} VOLUME_SET_GPT_ATTRIBUTES_INFORMATION;
#endif
static_assert(sizeof(VOLUME_SET_GPT_ATTRIBUTES_INFORMATION) == 16, "Unexpected GPT attribute ABI");
#else
#include <ntddvol.h>
#endif
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
    bool mountedReadOnly = false;
    bool mountStateKnown = false;
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
// Persistent metadata and mounted filesystem state must be evaluated independently.
// RefreshMountedState dismounts targeted volumes, including busy ones, and verifies remount.
bool ReadAttributes(HANDLE handle, ULONGLONG& attributes, std::wstring& error);
bool WriteAttributes(const std::wstring& device, ULONGLONG attributes,
                     bool allVolumes, std::wstring& error);
bool ReadMountedReadOnly(const std::wstring& volume, bool& readonly);
bool RefreshMountedState(DWORD diskNumber, const std::vector<std::wstring>& volumeDevices,
                         bool desiredReadOnly, std::wstring& detail, Progress progress = {});
bool Discover(std::wstring& error);
Disk* SourceDisk();
bool ChangeDisk(Disk& disk, policy::Action action, std::wstring& summary, Progress progress = {});
std::wstring DiskLabel(const Disk& disk);
}
