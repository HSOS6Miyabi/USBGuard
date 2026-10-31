#include "core.hpp"
#include <cassert>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>

using namespace linuxguard;
struct FakeBackend : Backend {
    std::map<std::string,bool> blocks;
    std::map<std::string,bool> mounts;
    std::string failOn;
    std::vector<std::string> operations;
    BlockIdentity liveDiskIdentity;
    std::map<std::string, BlockIdentity> liveVolumeIdentity;
    bool replaceAfterRemount = false;
    void ValidateTarget(const Disk& disk, const Volume& volume) override {
        const auto found = liveVolumeIdentity.find(volume.device);
        if (!SameBlockIdentity(disk.identity, liveDiskIdentity) ||
            found == liveVolumeIdentity.end() ||
            !SameBlockIdentity(volume.identity, found->second))
            throw std::runtime_error("目标设备身份改变");
    }
    void Remount(const Volume& v,bool readonly) override {
        if (failOn==v.mountpoint) throw std::runtime_error("mounted device busy");
        mounts[v.mountpoint]=readonly;
        operations.push_back("mount:"+v.mountpoint+":"+(readonly?"ro":"rw"));
        if (replaceAfterRemount) liveDiskIdentity.usbSerial = "different-device";
    }
    void SetBlockReadOnly(const Volume& v,bool readonly) override {
        if (failOn==v.device) throw std::runtime_error("ioctl failure");
        blocks[v.device]=readonly;
        operations.push_back("block:"+v.device+":"+(readonly?"ro":"rw"));
    }
    bool MountedReadOnly(const Volume& v) override {return mounts.at(v.mountpoint);}
    bool BlockReadOnly(const std::string& d) override {return blocks.at(d);}
};
int main() {
    const auto mounts=ParseMountInfo(
        "42 30 8:17 / /media/alice/My\\040USB rw,nosuid,nodev - vfat /dev/sdb1 rw\n"
        "43 30 8:18 / /media/alice/EFI ro - vfat /dev/sdb2 ro\n"
        "45 30 8:19 / /media/bind rw - ext4 /dev/sdb3 rw\n"
    );
    assert(mounts.size()==3);
    assert(mounts[0].mountpoint=="/media/alice/My USB");
    assert(!mounts[0].readonly);
    assert(mounts[1].readonly);
    assert(DecodeMountPath("/media/a\\134b")=="/media/a\\b");
    assert(!IsSafeDeviceArgument("/dev/../sda"));
    assert(!IsSafeDeviceArgument("/etc/passwd"));
    assert(IsSafeDeviceArgument("/dev/sdb"));
    assert(EscapeShellArgument("a'b")=="'a'\\''b'");
    Disk disk{"/dev/sdb","Disposable USB",{
        {"/dev/sdb1","/media/usb/one","vfat",true,true},
        {"/dev/sdb2","/media/usb/two","ext4",false,false}
    }};
    // A UASP disk is still represented by a USB ancestor in sysfs.
    disk.identity = {8,16,"/sys/devices/usb1/1-2/block/sdb",
                     "/sys/devices/usb1/1-2","abcd","1234","UASP-001"};
    disk.volumes[0].identity = {8,17,"/sys/devices/usb1/1-2/block/sdb/sdb1",
                                "/sys/devices/usb1/1-2","abcd","1234","UASP-001"};
    disk.volumes[1].identity = {8,18,"/sys/devices/usb1/1-2/block/sdb/sdb2",
                                "/sys/devices/usb1/1-2","abcd","1234","UASP-001"};
    assert(SameBlockIdentity(disk.identity, disk.identity));
    BlockIdentity sata = disk.identity;
    sata.usbPath.clear();
    assert(!SameBlockIdentity(disk.identity, sata));
    BlockIdentity nvme = disk.identity;
    nvme.sysfsPath = "/sys/devices/pci0000:00/nvme/nvme0n1";
    assert(!SameBlockIdentity(disk.identity, nvme));
    BlockIdentity reattached = disk.identity;
    reattached.usbSerial = "replacement-device";
    assert(!SameBlockIdentity(disk.identity, reattached));
    reattached = disk.identity;
    reattached.major = 259;
    assert(!SameBlockIdentity(disk.identity, reattached));
    FakeBackend backend;
    backend.liveDiskIdentity = disk.identity;
    for (const auto& volume : disk.volumes)
        backend.liveVolumeIdentity[volume.device] = volume.identity;
    backend.blocks={{"/dev/sdb1",true},{"/dev/sdb2",false}};
    backend.mounts={{"/media/usb/one",true},{"/media/usb/two",false}};
    assert(DesiredReadOnly(Action::Toggle,disk.volumes));
    std::vector<std::string> phases;
    int old=0;
    auto progress=[&](int done,int total,const std::string& text) {
        assert(total==7);
        assert(done>=old && done<=total);
        old=done; phases.push_back(text);
    };
    auto protect=Apply(disk,Action::Toggle,backend,progress);
    assert(protect.success && protect.readonly && old==7);
    assert(backend.blocks["/dev/sdb1"] && backend.blocks["/dev/sdb2"]);
    assert(backend.mounts["/media/usb/one"] && backend.mounts["/media/usb/two"]);
    assert(!DesiredReadOnly(Action::Toggle,disk.volumes));
    old=0;
    auto unprotect=Apply(disk,Action::Toggle,backend,progress);
    assert(unprotect.success && !unprotect.readonly && old==7);
    assert(!backend.blocks["/dev/sdb1"] && !backend.blocks["/dev/sdb2"]);
    assert(!backend.mounts["/media/usb/one"] && !backend.mounts["/media/usb/two"]);
    auto status=Apply(disk,Action::Status,backend,{});
    assert(status.success && !status.readonly);
    // A failing remount must never return success and must restore prior state.
    backend.failOn="/media/usb/two";
    auto failed=Apply(disk,Action::Protect,backend,{});
    assert(!failed.success);
    assert(!backend.blocks["/dev/sdb1"] && !backend.mounts["/media/usb/one"]);
    backend.failOn.clear();
    auto again=Apply(disk,Action::Protect,backend,{});
    assert(again.success);
    assert(backend.blocks["/dev/sdb1"] && backend.blocks["/dev/sdb2"]);
    assert(backend.mounts["/media/usb/one"] && backend.mounts["/media/usb/two"]);
    // A local SATA or NVMe replacement must be rejected before any writes.
    FakeBackend wrongBus = backend;
    wrongBus.liveDiskIdentity = sata;
    wrongBus.operations.clear();
    const auto deniedBus = Apply(disk,Action::Protect,wrongBus,{});
    assert(!deniedBus.success && wrongBus.operations.empty());

    // A USB disk unplugged after remount must not receive a block-device IOCTL.
    FakeBackend swapped = backend;
    swapped.blocks["/dev/sdb1"] = false;
    swapped.blocks["/dev/sdb2"] = false;
    swapped.mounts["/media/usb/one"] = false;
    swapped.mounts["/media/usb/two"] = false;
    swapped.operations.clear();
    swapped.replaceAfterRemount = true;
    const auto deniedSwap = Apply(disk,Action::Protect,swapped,{});
    assert(!deniedSwap.success);
    bool wroteReplacement = false;
    for (const auto& operation : swapped.operations)
        if (operation.rfind("block:", 0) == 0) wroteReplacement = true;
    assert(!wroteReplacement);

    std::cout << "Linux USB/UASP identity, SATA/NVMe rejection and hotplug tests PASSED\n";
}
