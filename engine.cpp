#include "engine.hpp"
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <iterator>
#include <utility>
#include <memory>
#include <vector>
namespace usb {
std::vector<Disk> g_disks;
DWORD g_executableDisk = MAXDWORD;
std::set<DWORD> g_windowsDisks;
constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
struct AutoHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    AutoHandle() = default;
    explicit AutoHandle(HANDLE h) : handle(h) {}
    AutoHandle(const AutoHandle&) = delete;
    AutoHandle& operator=(const AutoHandle&) = delete;
    ~AutoHandle() { if (handle != INVALID_HANDLE_VALUE && handle != nullptr) CloseHandle(handle); }
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
    HANDLE get() const { return handle; }
};
std::wstring ErrorText(DWORD error) {
    wchar_t* p = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, error, 0, reinterpret_cast<LPWSTR>(&p), 0, nullptr);
    std::wstring result = (n && p) ? std::wstring(p, n) : L"未知错误";
    if (p) LocalFree(p);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result + L" (" + std::to_wstring(error) + L")";
}

std::wstring DiskPath(DWORD number) {
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
}

std::wstring DevicePath(const std::wstring& volumeName) {
    if (!volumeName.empty() && volumeName.back() == L'\\') {
        return volumeName.substr(0, volumeName.size() - 1);
    }
    return volumeName;
}

AutoHandle OpenVolume(const std::wstring& deviceName) {
    HANDLE h = CreateFileW(DevicePath(deviceName).c_str(), GENERIC_READ | GENERIC_WRITE,
                           kShare, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(DevicePath(deviceName).c_str(), GENERIC_READ,
                        kShare, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return AutoHandle(h);
}

bool OneDiskExtent(HANDLE handle, DWORD& disk, std::wstring& error) {
    std::vector<BYTE> storage(4096);
    for (int tries = 0; tries < 6; ++tries) {
        DWORD returned = 0;
        if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, storage.data(), static_cast<DWORD>(storage.size()),
                            &returned, nullptr)) {
            if (returned < offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT)) {
                error = L"卷的物理位置数据无效";
                return false;
            }
            auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(storage.data());
            if (extents->NumberOfDiskExtents != 1) {
                error = L"卷跨越多个物理区段，已拒绝处理";
                return false;
            }
            disk = extents->Extents[0].DiskNumber;
            return true;
        }
        DWORD e = GetLastError();
        if (e != ERROR_MORE_DATA && e != ERROR_INSUFFICIENT_BUFFER) {
            error = L"无法确定卷的物理磁盘：" + ErrorText(e);
            return false;
        }
        storage.resize(storage.size() * 2);
    }
    error = L"卷区段数量超出安全限制";
    return false;
}

bool VolumeDiskByRoot(const std::wstring& absolutePath, DWORD& disk) {
    wchar_t root[MAX_PATH + 1]{};
    if (!GetVolumePathNameW(absolutePath.c_str(), root, MAX_PATH + 1)) return false;
    wchar_t volumeName[MAX_PATH + 1]{};
    if (!GetVolumeNameForVolumeMountPointW(root, volumeName, MAX_PATH + 1)) return false;
    auto handle = OpenVolume(volumeName);
    if (!handle) return false;
    std::wstring error;
    return OneDiskExtent(handle.get(), disk, error);
}

bool ExecutableDisk(DWORD& disk) {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return false;
    return VolumeDiskByRoot(std::wstring(path.data(), n), disk);
}

void FindWindowsDisks() {
    g_windowsDisks.clear();
    wchar_t windowsDir[MAX_PATH + 1]{};
    if (GetWindowsDirectoryW(windowsDir, MAX_PATH + 1)) {
        DWORD number = MAXDWORD;
        if (VolumeDiskByRoot(windowsDir, number)) g_windowsDisks.insert(number);
    }
    wchar_t systemDir[MAX_PATH + 1]{};
    if (GetSystemDirectoryW(systemDir, MAX_PATH + 1)) {
        DWORD number = MAXDWORD;
        if (VolumeDiskByRoot(systemDir, number)) g_windowsDisks.insert(number);
    }
}

bool DiskProperties(DWORD number, PARTITION_STYLE& style, ULONGLONG& size) {
    auto handle = AutoHandle(CreateFileW(DiskPath(number).c_str(), GENERIC_READ,
                                         kShare, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    alignas(STORAGE_DEVICE_DESCRIPTOR) BYTE response[2048]{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query), response, sizeof(response), &returned, nullptr) ||
        returned < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties)) return false;
    auto* descriptor = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(response);
#ifdef USBGUARD_VIRTUAL_DISK_TEST
    // Only the CI-only executable is permitted to exercise disposable virtual disks.
    if (descriptor->BusType != BusTypeUsb &&
        descriptor->BusType != BusTypeFileBackedVirtual &&
        descriptor->BusType != BusTypeVirtual) return false;
#else
    if (descriptor->BusType != BusTypeUsb) return false;
#endif

    std::vector<BYTE> layoutBuffer(65536);
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                         layoutBuffer.data(), static_cast<DWORD>(layoutBuffer.size()),
                         &returned, nullptr) || returned < offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) return false;
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuffer.data());
    style = static_cast<PARTITION_STYLE>(layout->PartitionStyle);
    if (style != PARTITION_STYLE_MBR && style != PARTITION_STYLE_GPT) return false;

    GET_LENGTH_INFORMATION length{};
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                         &length, sizeof(length), &returned, nullptr)) return false;
    size = static_cast<ULONGLONG>(length.Length.QuadPart);
    return true;
}

std::wstring MountPoints(const std::wstring& volumeName) {
    DWORD needed = 0;
    GetVolumePathNamesForVolumeNameW(volumeName.c_str(), nullptr, 0, &needed);
    if (needed == 0 || needed > 65536) return L"（没有盘符）";
    std::vector<wchar_t> buffer(needed + 2, L'\0');
    if (!GetVolumePathNamesForVolumeNameW(volumeName.c_str(), buffer.data(),
                                           static_cast<DWORD>(buffer.size()), &needed)) return L"（没有盘符）";
    std::wstring result;
    for (const wchar_t* it = buffer.data(); *it; it += wcslen(it) + 1) {
        if (!result.empty()) result += L", ";
        result += it;
    }
    return result.empty() ? L"（没有盘符）" : result;
}

bool IsEligiblePartition(HANDLE handle, PARTITION_STYLE style) {
    PARTITION_INFORMATION_EX partition{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0,
                         &partition, sizeof(partition), &returned, nullptr)) return false;
    if (partition.PartitionStyle != style) return false;
    if (style == PARTITION_STYLE_GPT) {
        // Protect Windows-mountable data regardless of GPT type, excluding
        // special partitions whose attribute changes can break dynamic disks.
        const GUID msr = {0xe3c9e316, 0x0b5c, 0x4db8,
                          {0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae}};
        const GUID ldmData = {0xaf9b60a0, 0x1431, 0x4f62,
                              {0xbc, 0x68, 0x33, 0x11, 0x71, 0x4a, 0x69, 0xad}};
        const GUID ldmMetadata = {0x5808c8aa, 0x7e8f, 0x42e0,
                                  {0x85, 0xd2, 0xe1, 0xe9, 0x04, 0x34, 0xcf, 0xb3}};
        return !IsEqualGUID(partition.Gpt.PartitionType, msr) &&
               !IsEqualGUID(partition.Gpt.PartitionType, ldmData) &&
               !IsEqualGUID(partition.Gpt.PartitionType, ldmMetadata);
    }
    return partition.Mbr.PartitionType != 0x42 && partition.Mbr.PartitionType != 0;
}
bool HasDriveLetterMount(const std::wstring& mounts) {
    size_t at = 0;
    while (at < mounts.size()) {
        const size_t end = mounts.find(L", ", at);
        const size_t length = end == std::wstring::npos ?
                              mounts.size() - at : end - at;
        if (length == 3) {
            const wchar_t letter = mounts[at];
            if (((letter >= L'A' && letter <= L'Z') ||
                 (letter >= L'a' && letter <= L'z')) &&
                mounts[at + 1] == L':' && mounts[at + 2] == L'\\') return true;
        }
        if (end == std::wstring::npos) break;
        at = end + 2;
    }
    return false;
}

bool ReadAttributes(HANDLE handle, ULONGLONG& attributes, std::wstring& error) {
    VOLUME_GET_GPT_ATTRIBUTES_INFORMATION data{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_VOLUME_GET_GPT_ATTRIBUTES, nullptr, 0,
                         &data, sizeof(data), &returned, nullptr)) {
        error = L"无法读取磁盘上保存的卷属性：" + ErrorText(GetLastError());
        return false;
    }
    attributes = data.GptAttributes;
    return true;
}

bool WriteAttributes(const std::wstring& device, ULONGLONG attributes,
                     bool allVolumes, std::wstring& error) {
    // A read-only volume may reject GENERIC_WRITE on open. The volume IOCTL
    // accepts a volume handle, so retry with read access when necessary.
    auto handle = OpenVolume(device);
    if (!handle) {
        error = L"无法打开卷设备：" + ErrorText(GetLastError());
        return false;
    }
    VOLUME_SET_GPT_ATTRIBUTES_INFORMATION data{};
    data.GptAttributes = attributes;
    data.RevertOnClose = FALSE;
    data.ApplyToAllConnectedVolumes = allVolumes ? TRUE : FALSE;
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_VOLUME_SET_GPT_ATTRIBUTES,
                         &data, sizeof(data), nullptr, 0, &returned, nullptr)) {
        error = L"写入持久卷属性失败：" + ErrorText(GetLastError());
        return false;
    }
    ULONGLONG after = 0;
    if (!ReadAttributes(handle.get(), after, error)) return false;
    if (((after ^ attributes) & kReadOnly) != 0) {
        error = L"卷属性复查失败：只读状态与请求不一致";
        return false;
    }
    return true;
}


bool ReadMountedReadOnly(const std::wstring& volume, bool& readonly) {
    std::wstring root = volume;
    // Both volume GUID roots and ordinary drive roots are supported.
    if (root.size() == 6 && root.compare(0, 4, L"\\\\.\\") == 0 && root[5] == L':') {
        root = root.substr(4, 2) + L"\\";
    } else if (!root.empty() && root.back() != L'\\') {
        root.push_back(L'\\');
    }
    DWORD flags = 0;
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr,
                               &flags, nullptr, 0)) return false;
    readonly = (flags & FILE_READ_ONLY_VOLUME) != 0;
    return true;
}

bool RefreshMountedState(DWORD diskNumber, const std::vector<std::wstring>& volumeDevices,
                         bool desiredReadOnly, std::wstring& detail) {
    // Refresh the partition manager before forcing the file system to remount.
    DWORD bytes = 0;
    auto disk = AutoHandle(CreateFileW(DiskPath(diskNumber).c_str(), GENERIC_READ,
                                       kShare, nullptr, OPEN_EXISTING, 0, nullptr));
    const bool diskUpdated = disk && DeviceIoControl(disk.get(), IOCTL_DISK_UPDATE_PROPERTIES,
                                                     nullptr, 0, nullptr, 0, &bytes, nullptr);
    const DWORD updateError = diskUpdated ? ERROR_SUCCESS : GetLastError();
    size_t unresolved = 0;
    std::wstring reasons;
    for (const auto& volume : volumeDevices) {
        bool dismounted = false;
        DWORD lockError = ERROR_SUCCESS;
        DWORD dismountError = ERROR_SUCCESS;
        {
            auto handle = OpenVolume(volume);
            if (!handle) {
                dismountError = GetLastError();
            } else {
                // An exclusive lock flushes pending writes when possible.
                // If other processes keep the volume open, dismount it anyway:
                // stale handles may then fail and unflushed writes may be lost.
                const bool locked = DeviceIoControl(handle.get(), FSCTL_LOCK_VOLUME,
                                                     nullptr, 0, nullptr, 0, &bytes, nullptr) != FALSE;
                if (!locked) lockError = GetLastError();
                dismounted = DeviceIoControl(handle.get(), FSCTL_DISMOUNT_VOLUME,
                                             nullptr, 0, nullptr, 0, &bytes, nullptr) != FALSE;
                if (!dismounted) dismountError = GetLastError();
            }
        }
        // Querying the volume by GUID forces Windows to mount a fresh instance.
        bool liveReadOnly = false;
        const bool known = dismounted && ReadMountedReadOnly(volume, liveReadOnly);
        if (!known || liveReadOnly != desiredReadOnly) {
            ++unresolved;
            reasons += L"卷 " + volume + L" 未确认当前文件系统已切换";
            if (dismountError != ERROR_SUCCESS)
                reasons += L"（强制卸载失败：" + ErrorText(dismountError) + L"）";
            else if (!known)
                reasons += L"（无法重新读取挂载状态）";
            if (lockError != ERROR_SUCCESS)
                reasons += L"（此前无法获取独占锁：" + ErrorText(lockError) + L"）";
            reasons += L"。";
        }
    }
    if (unresolved == 0) {
        detail = L"目标卷已卸载并重新读取，当前文件系统只读状态已验证。";
        if (!diskUpdated) detail += L"磁盘属性刷新请求未成功：" + ErrorText(updateError);
        return true;
    }
    detail = reasons + L"磁盘上的持久属性已修改，但当前文件系统状态尚未确认，请安全弹出并重新插入 U 盘。";
    if (!diskUpdated) detail += L"磁盘属性刷新请求未成功：" + ErrorText(updateError);
    return false;
}

bool Discover(std::wstring& error) {
    g_disks.clear();
    g_executableDisk = MAXDWORD;
    if (!ExecutableDisk(g_executableDisk)) {
        error = L"无法判断程序所在物理磁盘。可以查看其他 USB 磁盘，但无法自动选择。";
    }
    FindWindowsDisks();
    if (g_windowsDisks.empty()) {
        error = L"无法确认 Windows 所在磁盘。为了安全，已停止 USB 磁盘检测。";
        return false;
    }

    wchar_t name[MAX_PATH + 1]{};
    HANDLE enumeration = FindFirstVolumeW(name, MAX_PATH + 1);
    if (enumeration == INVALID_HANDLE_VALUE) {
        error = L"无法枚举 Windows 卷：" + ErrorText(GetLastError());
        return false;
    }

    do {
        const std::wstring volumeName(name);
        wchar_t fileSystem[64]{};
        if (!GetVolumeInformationW(volumeName.c_str(), nullptr, 0, nullptr, nullptr,
                                    nullptr, fileSystem, 64)) continue;
        if (!fileSystem[0]) continue;
        auto handle = OpenVolume(volumeName);
        if (!handle) continue;
        DWORD diskNumber = MAXDWORD;
        std::wstring tempError;
        if (!OneDiskExtent(handle.get(), diskNumber, tempError)) continue;
        PARTITION_STYLE style = PARTITION_STYLE_RAW;
        ULONGLONG diskSize = 0;
        if (!DiskProperties(diskNumber, style, diskSize)) continue;
        if (g_windowsDisks.count(diskNumber) != 0) continue;
        if (!IsEligiblePartition(handle.get(), style)) continue;
        ULONGLONG attributes = 0;
        if (!ReadAttributes(handle.get(), attributes, tempError)) continue;

        auto entry = std::find_if(g_disks.begin(), g_disks.end(),
                                  [diskNumber](const Disk& d) { return d.number == diskNumber; });
        if (entry == g_disks.end()) {
            Disk d;
            d.number = diskNumber;
            d.style = style;
            d.size = diskSize;
            d.sourceDisk = diskNumber == g_executableDisk;
            g_disks.push_back(std::move(d));
            entry = std::prev(g_disks.end());
        }
        Volume v;
        v.devicePath = volumeName;
        v.diskNumber = diskNumber;
        v.mountPoints = MountPoints(volumeName);
        if (!HasDriveLetterMount(v.mountPoints)) continue;
        v.fileSystem = fileSystem;
        v.attributes = attributes;
        v.readonly = (attributes & kReadOnly) != 0;
        v.mountStateKnown = ReadMountedReadOnly(volumeName, v.mountedReadOnly);
        entry->volumes.push_back(std::move(v));
    } while (FindNextVolumeW(enumeration, name, MAX_PATH + 1));
    FindVolumeClose(enumeration);

    std::sort(g_disks.begin(), g_disks.end(), [](const Disk& a, const Disk& b) { return a.number < b.number; });
    for (auto& disk : g_disks) {
        std::sort(disk.volumes.begin(), disk.volumes.end(),
                  [](const Volume& a, const Volume& b) { return a.mountPoints < b.mountPoints; });
    }
    return true;
}

Disk* SourceDisk() {
    for (auto& disk : g_disks) {
        if (disk.number == g_executableDisk) return &disk;
    }
    return nullptr;
}

bool ReadSelectedAttributes(Disk& disk, std::vector<size_t>& targets,
                            bool& allReadOnly, std::wstring& error) {
    targets.clear();
    allReadOnly = true;
    for (size_t index = 0; index < disk.volumes.size(); ++index) {
        auto& volume = disk.volumes[index];
        if (!volume.selected) continue;
        auto handle = OpenVolume(volume.devicePath);
        if (!handle) {
            error = L"无法打开卷 " + volume.mountPoints + L"：" + ErrorText(GetLastError());
            return false;
        }
        DWORD actualDisk = MAXDWORD;
        if (!OneDiskExtent(handle.get(), actualDisk, error) || actualDisk != disk.number ||
            (!IsEligiblePartition(handle.get(), disk.style) ||
              !HasDriveLetterMount(volume.mountPoints))) {
            error = L"卷的物理位置或分区类型发生变化，已拒绝操作：" + volume.mountPoints;
            return false;
        }
        ULONGLONG flags = 0;
        if (!ReadAttributes(handle.get(), flags, error)) {
            error = volume.mountPoints + L"：" + error;
            return false;
        }
        volume.attributes = flags;
        volume.readonly = (flags & kReadOnly) != 0;
        if (!volume.readonly) allReadOnly = false;
        targets.push_back(index);
    }
    if (targets.empty()) {
        error = L"至少需要选择一个 Windows 可以识别的卷。";
        return false;
    }
    if (disk.style == PARTITION_STYLE_MBR && targets.size() != disk.volumes.size()) {
        error = L"MBR 的持久只读属性作用于整块磁盘，必须选择所有列出的卷。";
        return false;
    }
    return true;
}

bool ChangeDisk(Disk& disk, policy::Action action, std::wstring& summary, Progress progress) {
    if (disk.number == MAXDWORD || g_windowsDisks.count(disk.number) != 0) {
        summary = L"禁止修改 Windows 所在磁盘。";
        return false;
    }
    // Re-check USB bus and partition type immediately before modifying the target.
    PARTITION_STYLE currentStyle = PARTITION_STYLE_RAW;
    ULONGLONG currentSize = 0;
    if (!DiskProperties(disk.number, currentStyle, currentSize) ||
        currentStyle != disk.style || currentSize != disk.size) {
        summary = L"目标设备已断开、改变或不再是受支持的 USB 磁盘。";
        return false;
    }
    std::vector<size_t> targets;
    bool allReadOnly = false;
    if (!ReadSelectedAttributes(disk, targets, allReadOnly, summary)) return false;
    if (action == policy::Action::Status) {
        size_t stale = 0;
        for (const size_t index : targets) {
            bool current = false;
            if (!ReadMountedReadOnly(disk.volumes[index].devicePath, current) ||
                current != disk.volumes[index].readonly) ++stale;
        }
        summary = allReadOnly ? L"所有选中卷的持久只读标志已启用。" :
                               L"至少一个选中卷的持久只读标志未启用。";
        if (stale) {
            summary += L"注意：当前文件系统状态与持久属性不一致，请安全弹出并重新插入 U 盘。";
            return false;
        }
        summary += L"当前文件系统状态与持久属性一致。";
        return true;
    }
    // If selected volumes differ, Toggle protects all of them in one operation.
    const bool desired = policy::DesiredReadOnly(action, allReadOnly);

    // The MBR layout has one portable metadata record for the entire disk.
    if (disk.style == PARTITION_STYLE_MBR) {
        if (progress) progress(0, 1, L"正在更新 MBR 的整盘持久属性");
        const size_t first = targets.front();
        const ULONGLONG requested = policy::ReplaceReadOnly(disk.volumes[first].attributes, desired);
        std::wstring error;
        if (!WriteAttributes(disk.volumes[first].devicePath, requested, true, error)) {
            summary = L"MBR 整盘持久属性操作失败：" + error;
            return false;
        }
        // Confirm every recognizable volume, since MBR applies to all of them.
        for (auto index : targets) {
            auto h = OpenVolume(disk.volumes[index].devicePath);
            ULONGLONG flags = 0;
            if (!h || !ReadAttributes(h.get(), flags, error) || (((flags & kReadOnly) != 0) != desired)) {
                summary = L"MBR 已提交，但部分卷无法复查；请安全弹出并重新插入 U 盘后检查。";
                return false;
            }
            disk.volumes[index].attributes = flags;
            disk.volumes[index].readonly = desired;
        }
        if (progress) progress(1, 2, L"整盘持久属性已复查，正在刷新当前文件系统");
        std::vector<std::wstring> devices;
        for (const auto index : targets) devices.push_back(disk.volumes[index].devicePath);
        std::wstring refreshed;
        const bool active = RefreshMountedState(disk.number, devices, desired, refreshed);
        if (progress) progress(2, 2, active ? L"文件系统只读状态已验证" : L"需要重新插入 U 盘");
        summary = (desired ? L"MBR 整盘持久只读标志已启用。" :
                              L"MBR 整盘持久只读标志已解除。") + refreshed;
        return active;
    }

    struct PriorState { size_t index; ULONGLONG flags; };
    std::vector<PriorState> completed;
    size_t steps = 0;
    for (const size_t index : targets) {
        if (progress) progress(steps, targets.size(), L"正在处理 " + disk.volumes[index].mountPoints);
        auto& volume = disk.volumes[index];
        if (volume.readonly == desired) {
            ++steps;
            if (progress) progress(steps, targets.size(), L"已复查 " + volume.mountPoints);
            continue;
        }
        const ULONGLONG previous = volume.attributes;
        const ULONGLONG requested = policy::ReplaceReadOnly(previous, desired);
        std::wstring error;
        if (!WriteAttributes(volume.devicePath, requested, false, error)) {
            // Best-effort rollback of changes already made in this operation.
            bool restored = true;
            for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
                std::wstring rollbackError;
                if (!WriteAttributes(disk.volumes[it->index].devicePath, it->flags, false, rollbackError)) {
                    restored = false;
                }
            }
            summary = L"卷 " + volume.mountPoints + L" 操作失败：" + error;
            summary += restored ? L" 已尝试恢复此前修改的卷。" : L" 部分卷恢复失败，请检查每个卷的状态。";
            return false;
        }
        completed.push_back({index, previous});
        volume.attributes = requested;
        volume.readonly = desired;
        ++steps;
        if (progress) progress(steps, targets.size(), L"已处理 " + volume.mountPoints);
    }
    // Verify every selected GPT volume, including unchanged volumes, before
    // accepting the disk-wide result.
    for (const size_t index : targets) {
        std::wstring error;
        auto handle = OpenVolume(disk.volumes[index].devicePath);
        ULONGLONG attributes = 0;
        if (!handle || !ReadAttributes(handle.get(), attributes, error) ||
            (((attributes & kReadOnly) != 0) != desired)) {
            summary = L"卷属性状态不一致：" + disk.volumes[index].mountPoints +
                      L"。请重新读取卷状态。";
            return false;
        }
        disk.volumes[index].attributes = attributes;
        disk.volumes[index].readonly = desired;
    }
    if (progress) progress(targets.size(), targets.size() + 1, L"正在刷新文件系统并验证实际状态");
    std::vector<std::wstring> devices;
    for (const auto index : targets) devices.push_back(disk.volumes[index].devicePath);
    std::wstring refreshed;
    const bool active = RefreshMountedState(disk.number, devices, desired, refreshed);
    if (progress) progress(targets.size() + 1, targets.size() + 1,
                           active ? L"文件系统只读状态已验证" : L"需要重新插入 U 盘");
    summary = (desired ? L"选中 GPT 分区的持久只读标志已启用。" :
                          L"选中 GPT 分区的持久只读标志已解除。") + refreshed;
    return active;
}

std::wstring DiskLabel(const Disk& disk) {
    const wchar_t* style = disk.style == PARTITION_STYLE_MBR ? L"MBR" : L"GPT";
    std::wstring label = L"USB 磁盘 " + std::to_wstring(disk.number) + L"  |  " + style +
        L"  |  " + std::to_wstring(disk.size / (1024ULL * 1024ULL)) + L" MiB";
    if (disk.sourceDisk) label += L"  |  程序所在 U 盘";
    return label;
}

} // namespace usb
