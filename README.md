# USBGuard

让 U 盘切换为只读，减少误操作和恶意程序写入文件的风险。支持 **Windows** 和 **Linux**，无需将程序安装到电脑上。

[下载最新版本](https://github.com/HSOS6Miyabi/USBGuard/releases/latest)

## Windows：下载后直接使用

**推荐使用 `USBGuardMini.exe`。** 将它复制到需要保护的 U 盘里，双击即可切换写保护状态。

1. 首次运行时，按 Windows 的提示同意管理员授权。
2. 右下角会出现一个小窗口，显示正在处理哪个分区、执行到了哪一步。
3. 操作完成后，窗口显示结果，稍后自动关闭，并尝试重新打开程序所在的 U 盘目录。

再次双击可以解除保护。如果 U 盘上有多个带盘符的分区，程序会检查它们的状态：只要有一个分区可写，切换操作就会尝试把所有目标分区设为只读；只有全部已保护时才会解除。

需要自己选择磁盘、查看各分区状态或使用更完整的界面，可以打开 **`USBGuardUI.exe`**。

### 该下载哪个 Windows 文件？

| 电脑类型 | 文件 |
| --- | --- |
| 大多数 Windows 10/11 电脑（Intel、AMD 64 位） | `USBGuard-x64.zip` |
| 32 位 Windows，包括面向 Windows XP 的兼容构建 | `USBGuard-x86.zip` |
| Windows ARM64 电脑 | `USBGuard-ARM64.zip` |

每个 ZIP 包都有 Mini 和完整界面两个 EXE。也可以在 Release 页面直接下载单个 EXE。Windows 版已静态链接 MSVC 运行库，无需另外安装 Visual C++ 运行库。x86 版本经过 Windows XP 目标编译和相关自动检查，实际 XP 机器上的运行仍需验证。

**Windows 的写保护会持久保存。** 在程序退出、U 盘拔出并重新插入后，已成功设置的卷属性通常仍然有效，直到主动解除。程序会强制卸载目标卷、刷新磁盘状态，并检查当前文件系统是否真的切换成功；如果检查失败，会明确提示。

> **操作前请保存 U 盘上的文件，并关闭正在使用 U 盘的程序。** 强制卸载可能让未保存的数据丢失。不要在 Windows 系统盘上使用这个工具。

### Windows 命令行

把 EXE 放在目标 U 盘里，然后在命令提示符中执行：

```bat
USBGuardMini.exe --protect
USBGuardMini.exe --unprotect
USBGuardMini.exe --toggle
USBGuardMini.exe --status
```

这些参数也可用于 `USBGuardUI.exe`。省略参数时，Mini 默认切换写保护状态。

## Linux：图形界面和命令行都能用

Linux 提供两个程序：

- **`USBGuardLinux`**：带进度条的 GTK 3 图形界面，可在 X11、Wayland 桌面下使用。
- **`USBGuardLinuxCLI`**：命令行版本，不需要图形桌面，适合服务器和轻量级窗口管理器。

从 Release 下载对应的 `USBGuard-Linux-x86_64.tar.gz` 或 `USBGuard-Linux-aarch64.tar.gz`，解压后在程序所在目录运行：

```sh
chmod +x USBGuardLinux USBGuardLinuxCLI
./USBGuardLinux
```

在图形界面中选择 U 盘，点击“切换保护”即可。需要修改设备状态时，程序会通过系统的 `pkexec` 请求管理员权限。图形界面需要安装 GTK 3；Sway、Hyprland、i3、Openbox 等桌面如果没有运行 polkit 授权代理，可以使用命令行：

```sh
./USBGuardLinuxCLI --list
./USBGuardLinuxCLI --device /dev/sdb --status
sudo ./USBGuardLinuxCLI --device /dev/sdb --protect
sudo ./USBGuardLinuxCLI --device /dev/sdb --unprotect
```

请把示例中的 `/dev/sdb` 换成实际 USB 磁盘的设备名。程序会检查设备是否属于 USB 总线，避免误操作普通系统磁盘。

**Linux 的只读保护只在本次连接期间有效。** 拔掉 U 盘后再插入，通常需要重新启用。Linux 版通过重新挂载文件系统和设置内核块设备只读状态实现保护，不会修改分区表，也不会安装常驻服务。

更多说明见 [Linux 使用文档](linux/README.md)。

## 下载全部版本

Release 页面还提供 **`USBGuard-All-In-One.zip`**，将所有 Windows、Linux 架构的程序放在同一个压缩包内，按操作系统和架构分类：

```text
Windows/
  x86/       USBGuardMini.exe、USBGuardUI.exe
  x64/       USBGuardMini.exe、USBGuardUI.exe
  ARM64/     USBGuardMini.exe、USBGuardUI.exe
Linux/
  x86_64/    USBGuardLinux、USBGuardLinuxCLI
  aarch64/   USBGuardLinux、USBGuardLinuxCLI
  README.md
README.md
```

只需要某一个系统的程序时，下载对应的小包即可。Release 同时提供 `SHA256SUMS.txt`，可以核对下载文件是否完整。

## 使用前需要知道

软件写保护能防止常规文件写入，但具有管理员权限的程序仍可能修改磁盘状态。它不能替代带有**硬件写保护开关**的 U 盘。

程序只处理识别出的 USB 设备及符合条件的卷。多卷操作会检查实际状态；发生错误时会尝试恢复已修改的状态，并报告失败。

## 自行编译与测试

Windows 需要 Visual Studio 2022、Windows SDK、CMake 和 Python 3：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Linux 需要 C++17 编译器、CMake；编译图形版还需要 GTK 3 开发包：

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

GitHub Actions 会自动编译 Windows x86、x64、ARM64 和 Linux x86_64、aarch64，执行测试后发布各平台文件。测试包含 Windows MBR/GPT 虚拟磁盘操作、Linux 模拟块设备操作以及 Linux X11/Wayland 窗口启动。真实 USB 设备和各类桌面环境下的效果仍可能受驱动、文件系统和系统配置影响。
