#include "core.hpp"
#include <sys/mount.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace linuxguard {
namespace fs = std::filesystem;
namespace {
std::string Error(const std::string& what) {
    return what + ": " + std::strerror(errno);
}
std::string ReadText(const std::string& filename) {
    std::ifstream file(filename);
    if (!file) throw std::runtime_error("无法读取 " + filename);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}
std::string FirstLine(const std::string& filename) {
    std::ifstream file(filename);
    std::string value;
    std::getline(file, value);
    return value;
}
std::string Canonical(const std::string& path) {
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return {};
    std::string result(resolved);
    free(resolved);
    return result;
}
bool IsUsbPath(const std::string& path) {
    // USB ancestry is visible in sysfs regardless of udev or desktop services.
    const auto offset = path.find("/usb");
    if (offset == std::string::npos) return false;
    const size_t n = offset + 4;
    return n < path.size() && std::isdigit(static_cast<unsigned char>(path[n]));
}
std::vector<MountEntry> LiveMounts() {
    return ParseMountInfo(ReadText("/proc/self/mountinfo"));
}
std::string DiskFromBlockName(const std::string& name) {
    const std::string path = Canonical("/sys/class/block/" + name);
    if (path.empty()) return {};
    const fs::path item(path);
    const auto partition = item / "partition";
    if (!fs::exists(partition)) return name;
    const std::string parent = item.parent_path().filename().string();
    return fs::exists("/sys/class/block/" + parent) ? parent : "";
}
void Report(const Progress& progress, int done, int total, const std::string& text) {
    if (progress) progress(done, total, text);
}
} // namespace

std::string DecodeMountPath(const std::string& encoded) {
    std::string out;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '\\' && i + 3 < encoded.size() &&
            encoded[i+1] >= '0' && encoded[i+1] <= '7' &&
            encoded[i+2] >= '0' && encoded[i+2] <= '7' &&
            encoded[i+3] >= '0' && encoded[i+3] <= '7') {
            const unsigned code = static_cast<unsigned>((encoded[i+1]-'0')*64 +
                                                          (encoded[i+2]-'0')*8 +
                                                          (encoded[i+3]-'0'));
            out.push_back(static_cast<char>(code));
            i += 3;
        } else out.push_back(encoded[i]);
    }
    return out;
}

std::vector<MountEntry> ParseMountInfo(const std::string& input) {
    std::vector<MountEntry> mounts;
    std::istringstream lines(input);
    std::string line;
    while (std::getline(lines, line)) {
        const auto separator = line.find(" - ");
        if (separator == std::string::npos) continue;
        std::istringstream head(line.substr(0, separator));
        std::istringstream tail(line.substr(separator+3));
        std::string id, parent, numbers, root, mountpoint, options, fs, source, super;
        if (!(head >> id >> parent >> numbers >> root >> mountpoint >> options) ||
            !(tail >> fs >> source >> super)) continue;
        const auto colon = numbers.find(':');
        if (colon == std::string::npos) continue;
        MountEntry entry;
        try {
            const auto lhs = numbers.substr(0, colon), rhs = numbers.substr(colon+1);
            const auto lhsNumber = std::stoul(lhs), rhsNumber = std::stoul(rhs);
            if (lhsNumber > 0xffffffffUL || rhsNumber > 0xffffffffUL) continue;
            entry.major = static_cast<unsigned>(lhsNumber);
            entry.minor = static_cast<unsigned>(rhsNumber);
        } catch (...) { continue; }
        entry.mountpoint = DecodeMountPath(mountpoint);
        entry.filesystem = fs;
        entry.source = DecodeMountPath(source);
        entry.readonly = options == "ro" || options.rfind("ro,",0) == 0 ||
                         options.find(",ro,") != std::string::npos ||
                         (options.size() >= 3 && options.compare(options.size()-3,3,",ro")==0);
        entry.bindMount = root != "/";
        mounts.push_back(std::move(entry));
    }
    return mounts;
}

Action ParseAction(const std::string& action) {
    if (action == "--protect") return Action::Protect;
    if (action == "--unprotect") return Action::Unprotect;
    if (action == "--toggle") return Action::Toggle;
    if (action == "--status") return Action::Status;
    throw std::invalid_argument("无效操作参数：" + action);
}
bool DesiredReadOnly(Action action, const std::vector<Volume>& volumes) {
    if (action == Action::Protect) return true;
    if (action == Action::Unprotect) return false;
    if (action == Action::Status) throw std::invalid_argument("状态查询无需修改");
    // One writable mount or writable partition means protect every mount.
    return !std::all_of(volumes.begin(), volumes.end(), [](const Volume& volume) {
        return volume.mountReadOnly && volume.blockReadOnly;
    });
}
std::string EscapeShellArgument(const std::string& value) {
    std::string escaped = "'";
    for (char c : value) escaped += c == '\'' ? "'\\''" : std::string(1,c);
    return escaped + "'";
}
bool IsSafeDeviceArgument(const std::string& device) {
    if (device.size() < 6 || device.rfind("/dev/",0) != 0) return false;
    const std::string name = device.substr(5);
    if (name.empty() || name.find('/') != std::string::npos) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}
bool IsUsbDisk(const std::string& path) {
    if (!IsSafeDeviceArgument(path)) return false;
    const std::string name = path.substr(5);
    const std::string sys = "/sys/class/block/" + name;
    if (!fs::exists(sys) || fs::exists(sys + "/partition")) return false;
    struct stat st{};
    if (stat(path.c_str(),&st) != 0 || !S_ISBLK(st.st_mode)) return false;
    const std::string sysDev = FirstLine(sys + "/dev");
    if (sysDev != std::to_string(major(st.st_rdev)) + ":" +
                  std::to_string(minor(st.st_rdev))) return false;
    return IsUsbPath(Canonical(sys + "/device"));
}
std::vector<Disk> Discover() {
    const auto mounts = LiveMounts();
    std::vector<Disk> disks;
    for (const auto& entry : fs::directory_iterator("/sys/class/block")) {
        const std::string name = entry.path().filename().string();
        const std::string device = "/dev/" + name;
        if (!IsUsbDisk(device)) continue;
        Disk disk;
        disk.device = device;
        disk.model = FirstLine("/sys/class/block/" + name + "/device/model");
        if (disk.model.empty()) disk.model = name;
        // USB drive's own filesystem and any mounted partitions.
        for (const auto& child : fs::directory_iterator("/sys/class/block")) {
            const std::string childName = child.path().filename().string();
            if (DiskFromBlockName(childName) != name) continue;
            const std::string childDevice = "/dev/" + childName;
            struct stat st{};
            if (stat(childDevice.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) continue;
            const std::string actual = FirstLine("/sys/class/block/" + childName + "/dev");
            if (actual != std::to_string(major(st.st_rdev)) + ":" +
                          std::to_string(minor(st.st_rdev))) continue;
            for (const auto& mounted : mounts) {
                if (mounted.major != static_cast<unsigned>(major(st.st_rdev)) ||
                    mounted.minor != static_cast<unsigned>(minor(st.st_rdev))) continue;
                if (mounted.filesystem == "swap" || mounted.filesystem == "overlay") continue;
                const std::string source = Canonical(mounted.source);
                // Detect false matches such as overlay/bind mounts of other sources.
                if (!source.empty() && source != Canonical(childDevice)) continue;
                Volume volume{childDevice,mounted.mountpoint,mounted.filesystem,mounted.readonly,false};
                try { volume.blockReadOnly = KernelBackend().BlockReadOnly(childDevice); }
                catch (...) { volume.blockReadOnly = false; }
                disk.volumes.push_back(std::move(volume));
            }
        }
        // A Linux installation can boot from an actual USB disk. Never
        // change protection on a disk that holds live system filesystems.
        const auto critical = [](const std::string& mount) {
            return mount == "/" || mount == "/boot" || mount == "/boot/efi" ||
                   mount == "/usr" || mount == "/var" || mount == "/home" ||
                   mount == "/run";
        };
        const bool systemMedia = std::any_of(disk.volumes.begin(),disk.volumes.end(),
            [&](const Volume& volume){return critical(volume.mountpoint);});
        if (!disk.volumes.empty() && !systemMedia) disks.push_back(std::move(disk));
    }
    std::sort(disks.begin(),disks.end(),[](const auto& a,const auto& b){return a.device<b.device;});
    return disks;
}
std::string SourceDisk() {
    char path[8192]{};
    const ssize_t n = readlink("/proc/self/exe",path,sizeof(path)-1);
    if (n <= 0) return {};
    path[n]=0;
    const std::string exe(path);
    const auto mounts = LiveMounts();
    size_t longest = 0;
    std::string source;
    for (const auto& mount : mounts) {
        const std::string& mp = mount.mountpoint;
        if (exe.compare(0,mp.size(),mp) != 0) continue;
        if (exe.size() > mp.size() && mp.back() != '/' && exe[mp.size()] != '/') continue;
        if (mp.size() >= longest) {longest=mp.size();source=mount.source;}
    }
    if (source.rfind("/dev/",0)!=0) return {};
    const std::string pathCanonical = Canonical(source);
    const std::string name = fs::path(pathCanonical).filename().string();
    const std::string parent = DiskFromBlockName(name);
    const std::string device = "/dev/" + parent;
    return IsUsbDisk(device) ? device : "";
}
void KernelBackend::Remount(const Volume& volume, bool readonly) {
    struct statvfs info{};
    if (statvfs(volume.mountpoint.c_str(),&info) != 0)
        throw std::runtime_error(Error("读取挂载状态失败"));
    unsigned long flags = MS_REMOUNT;
    if (readonly) flags |= MS_RDONLY;
    if (info.f_flag & ST_NOSUID) flags |= MS_NOSUID;
    if (info.f_flag & ST_NODEV) flags |= MS_NODEV;
    if (info.f_flag & ST_NOEXEC) flags |= MS_NOEXEC;
    if (info.f_flag & ST_SYNCHRONOUS) flags |= MS_SYNCHRONOUS;
    if (info.f_flag & ST_NOATIME) flags |= MS_NOATIME;
    if (info.f_flag & ST_NODIRATIME) flags |= MS_NODIRATIME;
    if (mount(nullptr,volume.mountpoint.c_str(),nullptr,flags,nullptr) != 0)
        throw std::runtime_error(Error("重新挂载失败 " + volume.mountpoint));
}
void KernelBackend::SetBlockReadOnly(const std::string& device, bool readonly) {
    const int fd = open(device.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if (fd < 0) throw std::runtime_error(Error("无法打开 " + device));
    int ro = readonly ? 1 : 0;
    const int result = ioctl(fd,BLKROSET,&ro);
    const int error = errno;
    close(fd);
    if (result != 0) {errno=error;throw std::runtime_error(Error("设置块设备只读属性失败 " + device));}
}
bool KernelBackend::MountedReadOnly(const Volume& volume) {
    const auto entries = LiveMounts();
    for (const auto& entry : entries)
        if (entry.mountpoint == volume.mountpoint && entry.source == volume.device)
            return entry.readonly;
    // symlink sources (e.g. /dev/disk/by-uuid) are common.
    for (const auto& entry : entries)
        if (entry.mountpoint == volume.mountpoint)
            return entry.readonly;
    throw std::runtime_error("挂载点已经消失：" + volume.mountpoint);
}
bool KernelBackend::BlockReadOnly(const std::string& device) {
    const int fd = open(device.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if (fd < 0) {
        // The sysfs read-only attribute can be inspected without disk-group
        // membership on distributions that restrict /dev/sdX to root.
        if ((errno == EACCES || errno == EPERM) && IsSafeDeviceArgument(device)) {
            const std::string entry = "/sys/class/block/" + device.substr(5) + "/ro";
            const std::string value = FirstLine(entry);
            if (value == "0" || value == "1") return value == "1";
        }
        throw std::runtime_error(Error("无法打开 " + device));
    }
    int ro = -1;
    const int result = ioctl(fd,BLKROGET,&ro);
    const int error = errno;
    close(fd);
    if (result != 0) {errno=error;throw std::runtime_error(Error("读取块设备属性失败 " + device));}
    return ro != 0;
}
Result Apply(Disk& disk, Action action, Backend& backend, const Progress& progress) {
    if (disk.volumes.empty()) return {false,false,"没有已挂载的目标文件系统"};
    if (action == Action::Status) {
        bool all = true;
        for (auto& volume : disk.volumes) {
            volume.blockReadOnly = backend.BlockReadOnly(volume.device);
            volume.mountReadOnly = backend.MountedReadOnly(volume);
            all = all && volume.blockReadOnly && volume.mountReadOnly;
        }
        return {true,all,all ? "全部目标卷当前均为只读" : "部分或全部目标卷当前可写"};
    }
    // Read kernel state immediately before deciding; never trust UI snapshots.
    for (auto& volume : disk.volumes) {
        volume.blockReadOnly = backend.BlockReadOnly(volume.device);
        volume.mountReadOnly = backend.MountedReadOnly(volume);
    }
    const bool desired = DesiredReadOnly(action,disk.volumes);
    struct Original{Volume volume;bool mounted;bool block;};
    std::vector<Original> original;
    for (const auto& v:disk.volumes) original.push_back({v,v.mountReadOnly,v.blockReadOnly});
    const int total = static_cast<int>(disk.volumes.size()*3+1);
    int done=0;
    try {
        for (auto& volume : disk.volumes) {
            Report(progress,done,total,"正在准备卷 " + volume.mountpoint);
            if (desired) {
                if (!backend.MountedReadOnly(volume)) backend.Remount(volume,true);
                ++done; Report(progress,done,total,"文件系统已重新挂载为只读：" + volume.mountpoint);
                if (!backend.BlockReadOnly(volume.device)) backend.SetBlockReadOnly(volume.device,true);
                ++done; Report(progress,done,total,"块设备只读属性已更新：" + volume.device);
            } else {
                if (backend.BlockReadOnly(volume.device)) backend.SetBlockReadOnly(volume.device,false);
                ++done; Report(progress,done,total,"块设备写入权限已更新：" + volume.device);
                if (backend.MountedReadOnly(volume)) backend.Remount(volume,false);
                ++done; Report(progress,done,total,"文件系统已重新挂载为可写：" + volume.mountpoint);
            }
            volume.blockReadOnly = backend.BlockReadOnly(volume.device);
            volume.mountReadOnly = backend.MountedReadOnly(volume);
            if (volume.blockReadOnly != desired || volume.mountReadOnly != desired)
                throw std::runtime_error("实际状态不一致：" + volume.mountpoint);
            ++done; Report(progress,done,total,"已验证实际状态：" + volume.mountpoint);
        }
        Report(progress,total,total,"所有目标卷状态一致");
        return {true,desired,desired ? "所有目标卷在当前连接期间已启用只读" :
                                       "所有目标卷已恢复可写"};
    } catch (const std::exception& e) {
        bool rolledBack=true;
        for(auto it=original.rbegin();it!=original.rend();++it) {
            try {
                if(!it->block) backend.SetBlockReadOnly(it->volume.device,false);
                if(backend.MountedReadOnly(it->volume)!=it->mounted)
                    backend.Remount(it->volume,it->mounted);
                if(it->block) backend.SetBlockReadOnly(it->volume.device,true);
            }catch(...){rolledBack=false;}
        }
        return {false,false,std::string(e.what())+
            (rolledBack ? "；已尝试恢复原有状态" : "；部分卷恢复失败，请立即检查")};
    }
}
} // namespace linuxguard
