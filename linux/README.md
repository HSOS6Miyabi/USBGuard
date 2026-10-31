# USBGuard Linux 使用说明

Linux 版可以在图形桌面中使用，也可以直接在终端里运行。它只会处理识别为 USB 磁盘、并且已经挂载的文件系统。

## 选择程序

| 程序 | 用途 |
| --- | --- |
| `USBGuardLinux` | GTK 3 图形界面，可选择 USB 磁盘、切换只读、查看进度 |
| `USBGuardLinuxCLI` | 命令行程序，不依赖图形桌面 |

支持 x86_64 和 aarch64。在 [Releases](https://github.com/HSOS6Miyabi/USBGuard/releases/latest) 下载对应的 `.tar.gz` 文件并解压。

## 图形界面怎么用？

将两个可执行文件放在同一个目录中，然后运行：

```sh
chmod +x USBGuardLinux USBGuardLinuxCLI
./USBGuardLinux
```

选择要保护的 USB 磁盘，然后点击“切换保护”。程序会在需要时请求管理员权限，并展示写保护进度与结果。

GTK 3 支持 X11、Wayland 和 Xwayland，可在 GNOME、KDE Plasma、Xfce、LXQt 等桌面中使用。Sway、Hyprland、i3、Openbox 等环境也可使用相应显示后端；需要系统安装 GTK 3，并有可用的 polkit 授权代理。

如果点击按钮后没有出现授权窗口，通常是当前会话没有运行 polkit 认证代理。这时可以直接使用下面的命令行方式。

## 终端怎么用？

先查看 U 盘对应的设备名称：

```sh
./USBGuardLinuxCLI --list
```

假设目标为 `/dev/sdb`：

```sh
./USBGuardLinuxCLI --device /dev/sdb --status
sudo ./USBGuardLinuxCLI --device /dev/sdb --protect
sudo ./USBGuardLinuxCLI --device /dev/sdb --unprotect
sudo ./USBGuardLinuxCLI --device /dev/sdb --toggle
```

请确认实际设备名后再执行。程序会检查物理磁盘是否属于 USB 总线，并避开检测到的系统启动盘。若 CLI 就在目标 U 盘中，可以尝试省略 `--device` 自动选择。

## 需要安装什么？

图形版依赖系统提供的 GTK 3 和 polkit。CLI 不依赖 GTK。

```sh
# Ubuntu / Debian
sudo apt install libgtk-3-0 policykit-1

# Fedora
sudo dnf install gtk3 polkit

# Arch Linux
sudo pacman -S gtk3 polkit
```

不同发行版的 polkit 认证代理可能单独打包，请根据正在使用的桌面环境安装相应组件。

## 写保护能持续多久？

**Linux 版的只读状态通常只在本次连接期间有效。** U 盘拔出后再插入，内核可能恢复可写状态，需要重新启用保护。

程序先将已挂载的文件系统重新挂载为只读，再使用 `BLKROSET` 设置分区的块设备只读属性；解除保护时先取消块设备只读，再恢复文件系统可写。每一步都会验证实际结果。

如果有多个已挂载分区，只要其中一个可写，“切换保护”就会尝试将它们统一改为只读。出现失败时，程序会尝试恢复此前状态，并明确提示。

程序不会修改 GPT/MBR 分区表，不会安装服务或修改 udev 规则。具有管理员权限的程序仍可以取消软件写保护。对写保护要求很高的场景，应选择带硬件写保护开关的设备。

**操作前请先保存文件，停止正在写入 U 盘的程序。** 个别文件系统和驱动可能拒绝立即重新挂载，此时需要关闭占用程序后重试。

## 编译与测试

需要 C++17 编译器和 CMake。编译图形版还需要 GTK 3 开发包，自动测试窗口可安装 `xvfb` 和 `xauth`。

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

如果只需要 CLI：

```sh
cmake -S . -B build-linux-cli -DUSBGUARD_LINUX_GUI=OFF
cmake --build build-linux-cli --parallel
```

项目的 GitHub Actions 会在 x86_64、aarch64 Linux 上编译与测试，并分别使用 Xvfb 和无界面的 Weston 检查 X11、Wayland 的界面启动。USB 设备的实际效果还需要结合设备及文件系统验证。
