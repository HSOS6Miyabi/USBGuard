# USB Guard · 便携式 U 盘持久写保护

两个独立的 Windows 原生 Win32 程序，使用 C++17、Windows SDK 和操作系统自带接口，不需要安装第三方运行库。

| 程序 | 使用方式 | 运行期间的界面 | 权限 |
|---|---|---|---|
| `USBGuardUI.exe` | 双击进入完整管理界面 | 磁盘列表、卷复选框、操作按钮、进度条、结果提示 | EXE 清单要求管理员权限 |
| `USBGuardToast.exe` | 双击直接切换程序所在 U 盘的只读状态 | Windows 10 1703+ 系统 Toast 进度条；旧版 Windows 显示兼容提示 | 普通进程发送通知，短暂管理员进程执行磁盘操作 |

两个版本都支持 `--protect`、`--unprotect`、`--toggle`、`--status`。通知版没有参数时执行 `--toggle`；界面版没有参数时打开图形界面。命令行模式仅操作 **当前 EXE 所在 USB 物理磁盘**，不会自动选择其他磁盘。

## 持久写保护的实现

**不使用当前 Windows 安装环境保存的磁盘只读属性。** 所有持久性更改使用 `IOCTL_VOLUME_SET_GPT_ATTRIBUTES`，设置 `RevertOnClose = FALSE`：

- **MBR 基本磁盘：** 对一个可识别卷执行一次 `ApplyToAllConnectedVolumes = TRUE` 操作；按照 Windows 的 MBR 基本磁盘规则，其持久属性影响整个磁盘的所有卷。即使磁盘有其他未列出的卷，也会受到影响。
- **GPT 基本磁盘：** GPT 没有同样作用于全盘的分区只读位，因此对选定的 Windows 可识别 Microsoft Basic Data 分区逐一修改 `GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY`。无法识别的分区不会被默认选中或修改。
- 保留除只读位以外的所有 GPT/卷属性。修改之前检查设备、卷所属物理磁盘和分区类型，修改之后再次读取只读状态。
- 只接受能够通过 `IOCTL_STORAGE_QUERY_PROPERTY` 验证 USB 总线的 MBR/GPT 基本磁盘；排除 Windows 系统目录所在磁盘，不处理跨磁盘卷。
- 默认只选择 EXE 所在的 USB 磁盘；完整界面可以显式选择其他经识别的 USB 磁盘。

软件只读是 Windows 对存储属性的解释和执行。具有管理权限或直接磁盘访问能力的恶意软件可能解除或绕过这种保护，Linux/macOS 对 Windows 持久卷属性的处理也可能不同。需要抵御已经控制操作系统的勒索软件，应使用物理写保护 U 盘或硬件写入阻断设备。

## 通知版的生命周期

通知版采用同一 EXE 的短暂双进程架构：

1. 普通权限进程负责通知并创建会话内临时命名管道，不显示应用主窗口。
2. 先显示“需要管理员权限”的通知，提示用户在 Windows 用户账户控制对话框中点击“是”，随即通过系统 `runas` 请求 UAC 权限并启动工作进程。无需点击通知中的按钮，也不会增加人为等待时间。工作进程只负责磁盘操作和进度报告。
3. Windows 10 1703 及更新版本使用系统 Toast 实时进度条；Windows 8/8.1 使用普通 Toast 状态通知。Windows 7 以及无法使用 Toast 的系统使用 `Shell_NotifyIconW` 通知气球报告进度百分比。
4. 管理员进程在操作结束后退出，普通进程发布结果通知、删除本次运行创建的通知身份和临时通知图标，然后立即退出，不保留后台服务或进程。

旧版 Windows 的通知气球依赖运行中的通知图标，进程退出后可能提前消失。通知是否呈现由系统设置决定，程序不会为了维持通知而占用 U 盘。

**临时通知身份：** 现代 Windows 的桌面 Toast 需要开始菜单快捷方式和 `AppUserModelID`。通知版只在本次执行期间创建具有随机标识的专用 `.lnk`，并通过对象生命周期管理在正常退出时删除；它不会覆盖现有快捷方式，不创建注册表持久项或 COM 激活程序。系统强制结束进程或突然断电时，清理操作无法保证执行，可能残留本次临时快捷方式。清理后通知无法再唤醒程序，因此结果通知不提供可在程序退出后使用的操作按钮。Windows 自己生成的运行记录、UAC 记录及通知历史由系统管理。

## 操作

将任意一个 EXE 放在要保护的 U 盘内。通知版双击会自动识别 EXE 所在 U 盘并切换状态，遇到 UAC 时确认管理员权限。界面版可查看文件系统、勾选卷并通过按钮选择启用、解除或切换保护。**执行完成后建议安全弹出并重新插入，再进行写入验证。**

在命令提示符中：

```bat
USBGuardToast.exe --protect
USBGuardToast.exe --unprotect
USBGuardToast.exe --toggle
USBGuardToast.exe --status

USBGuardUI.exe --protect
USBGuardUI.exe --unprotect
USBGuardUI.exe --status
```

`--toggle`：所有选中卷均为只读时解除只读；只要存在可写卷，就启用只读。`--status` 只读取状态。成功退出码为 0，无效参数为 2，无法找到可靠目标或 UAC 被取消为 3，操作失败为 4。

两个 EXE 使用 Windows GUI 子系统，批处理可通过 `start /wait` 等待结束：

```bat
start /wait "" "E:\\USBGuardToast.exe" --toggle
```

## Windows 构建

需要 Visual Studio 2022（包含 MSVC、Windows 10/11 SDK、C++ 桌面开发工具）、CMake 3.21+ 以及 Python 3（仅用于编译期间生成多尺寸图标，使用 Python 标准库）。运行生成的 EXE 无需安装 Python、NuGet、Qt、.NET 或 Windows App SDK。

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

也可以双击 `BUILD_WINDOWS.cmd`，脚本会进行 Release 构建、运行测试，并将两个 EXE 复制到 `dist` 目录。产物分别是 `build\Release\USBGuardUI.exe` 和 `build\Release\USBGuardToast.exe`。两种清单和由 `assets/generate_icon.py` 生成的 16～256 像素 EXE 图标都直接嵌入两个程序，可以分别独立拷贝到 U 盘。`USBGuardToast.exe` 使用运行时加载的 Windows Runtime 接口发布 Toast；Windows 7 可进入 `Shell_NotifyIconW` 兼容流程，无需 Windows App SDK。

Windows 7/8 应在真实设备上测试。通知 API 兼容性并不意味着该操作系统已经获得所有安全更新。

## 验证

项目附带无磁盘写入的策略单元测试。`ctest` 可以在 Linux 上测试跨平台策略逻辑，但 Win32 程序必须在 Windows 环境中编译和运行，GitHub Actions 在 Windows 上分别构建 x86、x64、ARM64 两种应用程序，并验证 PE 格式、目标架构、资源文件和独立策略逻辑。x86/x64 执行策略测试；ARM64 由 x64 Windows 运行器交叉编译，执行源码检查及 PE 静态验证。每次 `main` 分支提交通过所有检查后，会创建带 SHA-256 校验值的 GitHub Release；`v*` 标签也会触发发布。

用于验证的测试 U 盘应有完整的数据备份。测试包括 MBR/GPT、含多个分区的 U 盘、NTFS/FAT32/exFAT、Windows 不识别的分区、管理员权限取消、U 盘重新插入、Windows 7/8/10/11 的通知、进度更新及临时通知身份清理，以及通知版操作后能否立即弹出设备。

## Microsoft API 文档

- https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/attributes-volume
- https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_volume_get_gpt_attributes
- https://learn.microsoft.com/en-us/windows/win32/shell/notification-area
- https://learn.microsoft.com/en-us/windows/win32/shell/enable-desktop-toast-with-appusermodelid
- https://learn.microsoft.com/en-us/uwp/api/windows.ui.notifications.toastnotifier.update
- https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw