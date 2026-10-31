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
    void Remount(const Volume& v,bool readonly) override {
        if (failOn==v.mountpoint) throw std::runtime_error("mounted device busy");
        mounts[v.mountpoint]=readonly;
        operations.push_back("mount:"+v.mountpoint+":"+(readonly?"ro":"rw"));
    }
    void SetBlockReadOnly(const std::string& d,bool readonly) override {
        if (failOn==d) throw std::runtime_error("ioctl failure");
        blocks[d]=readonly;
        operations.push_back("block:"+d+":"+(readonly?"ro":"rw"));
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
    FakeBackend backend;
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
    std::cout << "Linux device policy, mount parsing, rollback and progress tests PASSED\n";
}
