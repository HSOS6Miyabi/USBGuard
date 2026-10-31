#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace linuxguard {

struct BlockIdentity {
    unsigned major = 0;
    unsigned minor = 0;
    std::string sysfsPath;
    std::string usbPath;
    std::string usbVendor;
    std::string usbProduct;
    std::string usbSerial;
};
bool SameBlockIdentity(const BlockIdentity& expected, const BlockIdentity& current);

struct Volume {
    std::string device;
    std::string mountpoint;
    std::string filesystem;
    bool mountReadOnly = false;
    bool blockReadOnly = false;
    BlockIdentity identity;
};

struct Disk {
    std::string device;
    std::string model;
    std::vector<Volume> volumes;
    BlockIdentity identity;
};

enum class Action { Protect, Unprotect, Toggle, Status };
using Progress = std::function<void(int, int, const std::string&)>;

struct MountEntry {
    unsigned major = 0;
    unsigned minor = 0;
    std::string mountpoint;
    std::string filesystem;
    std::string source;
    bool readonly = false;
    bool bindMount = false;
};

std::string DecodeMountPath(const std::string& encoded);
std::vector<MountEntry> ParseMountInfo(const std::string& input);
bool DesiredReadOnly(Action action, const std::vector<Volume>& volumes);
Action ParseAction(const std::string& action);
std::string EscapeShellArgument(const std::string& value);

class Backend {
public:
    virtual ~Backend() = default;
    virtual void Remount(const Volume& volume, bool readonly) = 0;
    virtual void SetBlockReadOnly(const Volume& volume, bool readonly) = 0;
    virtual void ValidateTarget(const Disk& disk, const Volume& volume) = 0;
    virtual bool MountedReadOnly(const Volume& volume) = 0;
    virtual bool BlockReadOnly(const std::string& device) = 0;
};

struct Result {
    bool success = false;
    bool readonly = false;
    std::string message;
};

Result Apply(Disk& disk, Action action, Backend& backend, const Progress& progress);
std::vector<Disk> Discover();
std::string SourceDisk();
bool IsUsbDisk(const std::string& path);
bool IsSafeDeviceArgument(const std::string& path);

class KernelBackend final : public Backend {
public:
    void Remount(const Volume& volume, bool readonly) override;
    void SetBlockReadOnly(const Volume& volume, bool readonly) override;
    void ValidateTarget(const Disk& disk, const Volume& volume) override;
    bool MountedReadOnly(const Volume& volume) override;
    bool BlockReadOnly(const std::string& device) override;
};

} // namespace linuxguard
