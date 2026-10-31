from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
engine = (root / 'engine.cpp').read_text(encoding='utf-8')
mini = (root / 'mini_main.cpp').read_text(encoding='utf-8')
ui = (root / 'ui_main.cpp').read_text(encoding='utf-8')
cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')

assert 'IOCTL_VOLUME_SET_GPT_ATTRIBUTES' in engine
assert 'RevertOnClose = FALSE' in engine
assert 'ApplyToAllConnectedVolumes = allVolumes ? TRUE : FALSE' in engine
assert 'IOCTL_DISK_SET_DISK_ATTRIBUTES' not in engine + mini + ui
assert 'IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS' in engine
assert 'g_windowsDisks.count' in engine
assert 'PBM_SETPOS' in ui and 'LVS_EX_CHECKBOXES' in ui
assert 'GetNamedPipeClientProcessId' in mini
assert 'ShellExecuteExW' in mini
assert 'L"runas"' in mini
assert 'WaitForSingleObject(launch.hProcess' in mini
assert 'CreateMutexW(' in mini and 'USBGuardMini.SingleOperation' in mini
assert 'WS_EX_TOPMOST' in mini and 'MonitorFromWindow' in mini
assert 'SW_SHOWNOACTIVATE' in mini and 'SetWindowPos' in mini
assert 'WM_PAINT' in mini and 'BitBlt' in mini and 'RoundRect' in mini
assert 'kProgressMessage' in mini and 'kFinishedMessage' in mini
assert 'PostMessageW(window,kProgressMessage' in mini
assert 'PostMessageW(window,kFinishedMessage' in mini
assert 'std::thread' in mini and 'g_ui.worker.join()' in mini
assert 'CompatTickCount()' in mini and 'kFinishDurationMs = 4200' in mini
assert 'GetProcAddress(kernel, "GetTickCount64")' in mini
assert 'GetProcAddress(kernel, "GetNamedPipeClientProcessId")' in mini
assert 'GetProcAddress(kernel, "CancelIoEx")' in mini
assert 'GetProcAddress(user32, "SetProcessDpiAwarenessContext")' in mini
assert 'CheckTokenMembership' in mini
assert 'bool RestoreExplorerWindow(HWND owner)' in mini
assert 'g_ui.executableDirectory=ExecutableDirectory();' in mini
assert 'ShellExecuteW(owner, L"open", directory.c_str()' in mini
assert 'g_ui.requestedAction != policy::Action::Status' in mini
assert 'g_ui.succeeded && !g_ui.smokeTest' in mini
assert 'HasDriveLetterMount' in engine and 'IsEligiblePartition' in engine
assert 'IsBasicDataPartition' not in engine
assert 'if (!HasDriveLetterMount(v.mountPoints)) continue' in engine
assert 'If selected volumes differ, Toggle protects all' in engine
assert '正在刷新物理磁盘属性' in engine
assert '正在请求卷独占访问' in engine
assert '正在强制卸载卷' in engine
assert '正在重新挂载卷' in engine
assert '正在验证卷只读状态' in engine
assert '正在复查卷持久属性' in engine
assert 'const size_t total = 1 + volumeDevices.size() * 4' in engine
assert 'RefreshMountedState(disk.number, devices, desired, refreshed,' in engine
assert 'PostUpdate(window,done,total,stage)' in mini
assert '"Win32"' in cmake and '/SUBSYSTEM:WINDOWS,5.01' in cmake
assert 'DestroyWindow(window)' in mini
assert '--ui-self-test' in mini and 'mini_ui_smoke' in cmake
assert 'USBGuardMini' in cmake and 'USBGuardUI' in cmake
assert 'RegCreateKey' not in mini and 'RegSetValue' not in mini
assert 'Shell_NotifyIcon' not in mini
assert 'ToastNotification' not in mini
assert 'AppUserModelID' not in mini
assert 'CreateShortcut' not in mini
assert 'RoInitialize' not in mini
assert 'Sleep(' not in mini

for name, level in [('app.manifest', 'requireAdministrator'), ('mini.manifest', 'asInvoker')]:
    parsed = ET.parse(root / name)
    actual = [e.attrib['level'] for e in parsed.iter() if e.tag.endswith('requestedExecutionLevel')]
    assert actual == [level], (name, actual)

with tempfile.TemporaryDirectory() as temp_dir:
    generated_icon = Path(temp_dir) / "usb-guard.ico"
    subprocess.run([sys.executable, str(root / 'assets' / 'generate_icon.py'),
                    str(generated_icon)], check=True, capture_output=True, text=True)
    ico = generated_icon.read_bytes()
    if len(sys.argv) > 1:
        built_icon = Path(sys.argv[1]).read_bytes()
        assert built_icon == ico, 'The embedded icon input differs from the icon generator output'
assert b'\x89PNG\r\n\x1a\n' in ico
reserved, kind, count = struct.unpack_from('<HHH', ico, 0)
assert reserved == 0 and kind == 1 and count >= 4
sizes = set()
for i in range(count):
    width, height = struct.unpack_from('<BB', ico, 6 + i * 16)
    actual_size = width or 256
    sizes.add((actual_size, height or 256))
    image_length, image_offset = struct.unpack_from('<II', ico, 6 + i * 16 + 8)
    assert image_offset + image_length <= len(ico)
    image = ico[image_offset:image_offset + image_length]
    if actual_size <= 48:
        assert struct.unpack_from('<I', image)[0] == 40, 'Legacy icons must use a DIB'
        dib_height = struct.unpack_from('<i', image, 8)[0]
        assert dib_height == 2 * actual_size
    if actual_size == 256:
        assert image.startswith(b'\\x89PNG\\r\\n\\x1a\\n')
assert {(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)} <= sizes
print('Source, manifest and icon checks passed')
