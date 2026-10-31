#include "core.hpp"
#include <unistd.h>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace linuxguard;
namespace {
void Usage() {
    std::cout << "USBGuard Linux CLI\n"
                 "  --list\n"
                 "  [--device /dev/sdX] --status\n"
                 "  [--device /dev/sdX] --protect|--unprotect|--toggle\n"
                 "  For changes use sudo (or GUI with polkit).\n"
                 "  Linux block-device protection lasts only for the current attachment.\n";
}
}
int main(int argc,char** argv) {
    try {
        std::string device;
        std::string actionName;
        bool listing = false;
        for (int i=1; i<argc; ++i) {
            const std::string arg(argv[i]);
            if(arg=="--device") {
                if(++i>=argc) throw std::invalid_argument("需要指定设备路径");
                device=argv[i];
            } else if(arg=="--list") listing=true;
            else if(arg=="--status"||arg=="--protect"||arg=="--unprotect"||arg=="--toggle") {
                if(!actionName.empty()) throw std::invalid_argument("只能指定一种操作");
                actionName=arg;
            } else if(arg=="--help"||arg=="-h"){Usage();return 0;}
            else throw std::invalid_argument("无法识别参数："+arg);
        }
        if(!device.empty()&&!IsSafeDeviceArgument(device))
            throw std::invalid_argument("设备路径必须是 /dev/ 下的直接块设备名称");
        auto disks = Discover();
        if(listing) {
            if(!actionName.empty())throw std::invalid_argument("--list 不能与修改操作组合");
            for(const auto& disk:disks) {
                std::cout << disk.device << " " << disk.model << "\n";
                for(const auto& volume:disk.volumes)
                    std::cout << "  " << volume.device << " " << volume.mountpoint
                              << " (" << volume.filesystem << ") "
                              << ((volume.mountReadOnly&&volume.blockReadOnly)?"RO":"RW/mixed") << "\n";
            }
            return 0;
        }
        if(actionName.empty()) {Usage();return 2;}
        Action action=ParseAction(actionName);
        if(device.empty()) device=SourceDisk();
        if(!IsUsbDisk(device))
            throw std::runtime_error("没有找到安全的 USB 目标磁盘。请使用 --list 和 --device 指定已挂载的 USB 设备");
        Disk* selected=nullptr;
        for(auto& disk:disks)
            if(disk.device==device){selected=&disk;break;}
        if(!selected)throw std::runtime_error("目标 USB 磁盘没有符合条件的已挂载卷");
        if(action!=Action::Status && geteuid()!=0)
            throw std::runtime_error("修改设备只读属性需要管理员权限；请使用 sudo 或图形界面中的授权操作");
        KernelBackend backend;
        const auto result=Apply(*selected,action,backend,[](int done,int total,const std::string& phase) {
            std::cout << "PROGRESS " << done << "/" << total << " " << phase << std::endl;
        });
        std::cout << (result.success?"RESULT OK ":"RESULT ERROR ") << result.message << std::endl;
        return result.success?0:4;
    }catch(const std::invalid_argument& e){
        std::cerr << "ERROR " << e.what() << std::endl;
        return 2;
    }catch(const std::exception& e){
        std::cerr << "ERROR " << e.what() << std::endl;
        return 4;
    }
}
