# USBGuard Linux

适用于 Linux 的便携式 U 盘只读保护工具，提供两个可执行文件：

- **USBGuardLinux**：GTK 3 图形界面，适用 Wayland、X11、Xwayland，支持 GNOME、KDE Plasma、Xfce、LXQt、Cinnamon、MATE、Sway、Hyprland、i3、Openbox 等采用上述显示系统的环境。
- **USBGuardLinuxCLI**：无图形环境的命令行版本，不依赖 GTK、特定窗口管理器或桌面服务。

## 使用方式

将两个可执行文件放在同一个目录并赋予执行权限：

\`\`\`sh
chmod +x USBGuardLinux USBGuardLinuxCLI
./USBGuardLinux
\`\`\`

在界面中选择 USB 设备，点击“切换保护”“启用只读”“解除只读”或“查看状态”。如果程序本身位于 U 盘上，会优先选择其所在的 USB 设备。

命令行示例：

\`\`\`sh
./USBGuardLinuxCLI --list
./USBGuardLinuxCLI --device /dev/sdb --status
sudo ./USBGuardLinuxCLI --device /dev/sdb --protect
sudo ./USBGuardLinuxCLI --device /dev/sdb --unprotect
sudo ./USBGuardLinuxCLI --device /dev/sdb --toggle
\`\`\`

省略 \`--device\` 时会尝试选择 CLI 程序所在的 USB 磁盘。程序会验证目标物理磁盘具有 USB 总线的 sysfs 设备关系，避免误操作主机系统盘；目标 USB 磁盘必须存在当前已挂载的文件系统。

**管理员授权**：图形界面调用系统 \`pkexec\` 请求授权。GNOME、KDE Plasma、Xfce 等桌面一般自带 polkit 代理。独立的 Sway、Hyprland、i3、Openbox 等会话可能需要单独运行 polkit 认证代理；没有代理时仍可在终端使用 \`sudo USBGuardLinuxCLI\`。本程序不安装服务、驱动、认证代理或 udev 规则。

## 写保护原理和有效期

1. 在 \`/sys/class/block\` 识别 USB 总线物理磁盘，读取 \`/proc/self/mountinfo\` 选择当前已挂载的文件系统。
2. 通过 \`mount(MS_REMOUNT | MS_RDONLY)\` 将挂载的文件系统切换到只读状态。
3. 通过 \`BLKROSET\` 设置对应分区的 Linux 内核块设备只读状态。
4. 使用挂载信息与 \`BLKROGET\` 分别检查两种实际状态；只有所有卷均满足期望时才报告成功。
5. 解除保护时先解除块设备只读，再将文件系统重新挂载为可写。若部分卷的设置失败，会尝试恢复原有状态，并如实报告任何未完成的恢复操作。

当部分卷只读、部分卷可写时，“切换保护”会统一将当前目标卷设为只读；只有所有目标卷均为只读时才统一解除。

**重要：Linux 版保护仅对当前设备连接期间有效。** USB 设备断开并重新接入后，内核块设备只读状态通常会被重置。Linux 不会普遍通过 GPT 的 Windows 专用位 60 属性自动实施只读保护。Linux 版本不会修改 GPT/MBR 分区表，也不会安装自动恢复保护的系统规则。若需要跨操作系统、拔插后仍然持久生效的防写入机制，应使用具有硬件写保护开关的介质。

保护过程中会同步及重新挂载文件系统。操作前请保存未保存的文件、停止正在写入 U 盘的程序。部分设备、文件系统和文件系统驱动不支持即时重新挂载，程序会报告失败。具有管理员权限的进程仍可主动取消软件只读状态。

## 图形环境兼容性

GTK 3 自动使用会话支持的 GDK 后端：

- **Wayland**：优先使用原生 Wayland 后端；浮窗的最终位置由 compositor 决定。
- **X11/Xorg**：使用 GTK 的 X11 后端，兼容主流桌面及独立窗口管理器。
- **Xwayland**：当 GTK 在 Wayland 环境中选择 X11 后端时可以使用，取决于系统是否安装 Xwayland。
- **无图形桌面**：直接运行 \`USBGuardLinuxCLI\`。

GTK 3 图形程序需要系统安装 GTK 3 运行库；命令行程序无 GTK 依赖。安装依赖示例：

\`\`\`sh
# Debian / Ubuntu
sudo apt install libgtk-3-0 policykit-1

# Fedora
sudo dnf install gtk3 polkit

# Arch Linux
sudo pacman -S gtk3 polkit
\`\`\`

实际 polkit 认证界面由用户桌面中的授权代理提供；若没有图形授权代理，可以使用终端中的 sudo。

## 本地编译与测试

\`\`\`sh
# Debian/Ubuntu 编译依赖：g++ cmake pkg-config libgtk-3-dev xvfb xauth
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
\`\`\`

如果没有 GTK 3 开发包，CMake 仍可只编译命令行版本；也可以设置 \`-DUSBGUARD_LINUX_GUI=OFF\`。GitHub Actions 在 Ubuntu 上分别构建 x86_64 和 aarch64 Linux 版本，自动运行块设备模拟测试和 Xvfb 图形窗口测试。

CI 的模拟测试验证两卷混合状态、回退行为及进度报告；不能替代在真实 USB 硬件、各种文件系统或真实 Wayland 会话下的验证。
