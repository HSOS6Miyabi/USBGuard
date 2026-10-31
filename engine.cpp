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
    if (descriptor->BusType != BusTypeUsb) return false;

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

bool IsBasicDataPartition(HANDLE handle, PARTITION_STYLE style) {
    PARTITION_INFORMATION_EX partition{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0,
                         &partition, sizeof(partition), &returned, nullptr)) return false;
    if (partition.PartitionStyle != style) return false;
    if (style == PARTITION_STYLE_GPT) {
        const GUID basicData{0xebd0a0a2, 0xb9e5, 0x4433,
                             {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};
        return IsEqualGUID(partition.Gpt.PartitionType, basicData) != 0;
    }
    // 0x42 is the dynamic-disk (LDM) MBR partition type.
    return partition.Mbr.PartitionType != 0x42 && partition.Mbr.PartitionType != 0;
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
        if (!IsBasicDataPartition(handle.get(), style)) continue;
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
        v.fileSystem = fileSystem;
        v.attributes = attributes;
        v.readonly = (attributes & kReadOnly) != 0;
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
            !IsBasicDataPartition(handle.get(), disk.style)) {
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
        summary = allReadOnly ? L"所有选中卷均已启用持久只读。" : L"至少一个选中卷处于可写状态。";
        return true;
    }
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
        if (progress) progress(1, 1, L"整盘属性已复查");
        summary = desired ? L"MBR 整盘持久只读属性已启用。" : L"MBR 整盘持久只读属性已解除。";
        return true;
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
    summary = desired ? L"选中 GPT 分区的持久只读属性已启用。" : L"选中 GPT 分区的持久只读属性已解除。";
    return true;
}

std::wstring DiskLabel(const Disk& disk) {
    const wchar_t* style = disk.style == PARTITION_STYLE_MBR ? L"MBR" : L"GPT";
    std::wstring label = L"USB 磁盘 " + std::to_wstring(disk.number) + L"  |  " + style +
        L"  |  " + std::to_wstring(disk.size / (1024ULL * 1024ULL)) + L" MiB";
    if (disk.sourceDisk) label += L"  |  程序所在 U 盘";
    return label;
}

} // namespace usb
