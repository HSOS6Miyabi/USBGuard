# USB Guard

便携式 Windows U 盘写保护程序。使用 C++17、原生 Win32 API 和 Windows SDK 开发。发布的可执行文件不依赖第三方运行框架。

## 两种界面

| 程序 | 功能 | 权限 |
|---|---|---|
| `USBGuardUI.exe` | 完整管理界面，可选择 USB 磁盘及可识别卷，显示操作进度和结果 | 启动时请求管理员权限 |
| `USBGuardMini.exe` | 在屏幕右下角显示独立的精简窗口，自动选择程序所在 U 盘，实时显示进度；完成后显示结果并在约 4 秒后自动关闭 | 普通权限窗口，通过 Windows UAC 启动短暂的管理员工作进程 |

迷你窗口使用 Windows 原生 GDI 双缓冲绘制，支持系统 DPI 缩放、圆角卡片、细分进度条以及状态颜色。进度展示包括写入持久属性、逐卷复查、刷新磁盘信息、请求独占访问、强制卸载卷、重新挂载卷和验证只读状态，每个阶段显示目标盘符。它只在执行期间存在，不创建通知、开始菜单快捷方式、注册表项、服务或驻留进程。

启动 `USBGuardMini.exe` 后，窗口立即提示用户在 Windows 用户账户控制对话框中点击“是”，随后直接发起 UAC 请求，无额外确认步骤。磁盘操作在独立工作线程中进行，窗口保持响应。操作成功时，普通权限的迷你窗口进程会重新打开 EXE 所在目录的资源管理器窗口，便于继续浏览 U 盘。`--status` 查询、失败操作以及界面测试不触发此行为。结果显示约 4 秒后自动退出，进程结束后不会继续占用 U 盘上的可执行文件。

两个程序都支持 `--protect`、`--unprotect`、`--toggle` 和 `--status`。迷你版双击默认执行 `--toggle`，完整界面双击后显示磁盘管理窗口。命令行方式默认操作当前 EXE 所在的 USB 物理磁盘。

## 写保护机制

所有持久性修改均通过 `IOCTL_VOLUME_SET_GPT_ATTRIBUTES`，并设置 `RevertOnClose = FALSE`。

- MBR 基本磁盘：通过 `ApplyToAllConnectedVolumes = TRUE` 更新磁盘关联卷的持久只读属性。
- GPT 磁盘：操作当前 Windows 已分配盘符、能够访问的卷，不以 Microsoft Basic Data 分区类型为必要条件。排除 MSR 和动态磁盘保留分区。
- 保留其他卷属性，只修改只读标志。排除 Windows 系统磁盘、无法确认的物理磁盘和跨物理磁盘卷。
- 修改后通过 `IOCTL_DISK_UPDATE_PROPERTIES` 请求刷新磁盘属性，并使用 `FSCTL_DISMOUNT_VOLUME` 重新挂载目标卷，验证当前文件系统实际只读状态。
- 设置只读和解除只读时，会重新核查每个选定卷的持久属性与文件系统实际状态。部分卷已保护、部分卷可写时，`--toggle` 统一将所有选定卷设置为只读；只有全部已保护时才统一解除。未能确认状态时报告失败。

**数据安全：** 重新挂载可能强制卸载仍被其他程序使用的卷。执行前请先保存文件并关闭占用 U 盘的应用程序。未保存的数据可能丢失。软件写保护也无法抵御拥有管理员权限或内核权限的恶意程序；需要可靠的不可写保护时，应使用带硬件写保护的设备。

## 使用方式

将相应 EXE 放入目标 U 盘根目录或子目录，双击运行。

也可使用命令行：

```bat
USBGuardMini.exe --protect
USBGuardMini.exe --unprotect
USBGuardMini.exe --toggle
USBGuardMini.exe --status

USBGuardUI.exe --protect
USBGuardUI.exe --unprotect
USBGuardUI.exe --status
```

`--toggle` 在所有选定卷都处于只读时解除保护，否则启用保护。成功时退出码为 0，参数错误为 2，无法确认目标或用户取消 UAC 为 3，执行失败为 4。

## 编译

需要 Visual Studio 2022（C++ 桌面开发工具、Windows SDK）、CMake 3.21+ 和 Python 3。x86 构建使用可选的 Visual Studio 2017 `v141_xp` 工具集，在同一 x86 可执行文件中通过运行时动态解析保留较新 Windows API，并在 Windows XP SP3 上使用兼容接口。Python 仅用于编译期间生成多尺寸图标，不是运行依赖。

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

也可以直接运行 `BUILD_WINDOWS.cmd`。完成后可在 `dist` 目录取得两个独立的 EXE。

## 自动测试

GitHub Actions 对 x86、x64、ARM64 分别编译完整界面和迷你窗口，检查 PE 架构、Windows GUI 子系统、资源及程序清单。x86 使用 `v141_xp` 并以 Windows XP SP3 为目标，同时保留在新系统使用新 API 的能力；x64 和 ARM64 使用 `v143`。

x86 和 x64 运行策略单元测试、源代码检查及迷你窗口自身的绘制与结束状态测试。x64 还会创建一次性 MBR/GPT 虚拟磁盘，验证写入、启用只读、拒绝写入、解除只读以及恢复写入的完整过程。ARM64 通过交叉编译和静态检查进行验证。CI 成功后自动发布六个 EXE、三个 ZIP 和 SHA-256 校验文件到 GitHub Releases。

虚拟磁盘测试与实际 U 盘控制器存在差异。Windows 执行器上的窗口测试检查窗口消息处理、绘制和退出行为，无法代替真实桌面上的视觉检查。

## 参考文档

- https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/attributes-volume
- https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_volume_get_gpt_attributes
- https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_dismount_volume
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindowexw
- https://learn.microsoft.com/en-us/windows/win32/gdi/double-buffering
