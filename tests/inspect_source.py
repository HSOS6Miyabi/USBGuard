from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
engine = (root / 'engine.cpp').read_text(encoding='utf-8')
toast = (root / 'toast_main.cpp').read_text(encoding='utf-8')
ui = (root / 'ui_main.cpp').read_text(encoding='utf-8')
cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')

assert 'IOCTL_VOLUME_SET_GPT_ATTRIBUTES' in engine
assert 'RevertOnClose = FALSE' in engine
assert 'ApplyToAllConnectedVolumes = allVolumes ? TRUE : FALSE' in engine
assert 'IOCTL_DISK_SET_DISK_ATTRIBUTES' not in engine + toast + ui
assert 'IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS' in engine
assert 'g_windowsDisks.count' in engine
assert 'PBM_SETPOS' in ui and 'LVS_EX_CHECKBOXES' in ui
assert 'Shell_NotifyIconW' in toast and 'NIM_DELETE' in toast
assert 'TemporaryNotificationIdentity' in toast
assert 'FOLDERID_Programs' in toast and 'PKEY_AppUserModel_ID' in toast
assert 'DeleteFileW(shortcutPath.c_str())' in toast
assert 'PortableUSBGuard.Notifications' in toast and 'CoCreateGuid' in toast
assert 'ClearWithId(appId.raw)' in toast
assert 'history->Remove(tag.raw)' in toast
assert toast.count('history->Remove(tag.raw)') == 2
assert 'UpdateWithTag' in toast and 'NotificationNotFound' in toast
assert 'if (!modern && build < 15063) balloon.Start();' in toast
assert 'balloon.Tell(L"USB Guard 正在处理"' not in toast
assert 'toast.Finish(title, result.message)' in toast
assert toast.count('balloon.Tell(') == 2
assert toast.count('toast.Show(L"USB Guard 需要管理员权限"') == 1
assert 'NativeToast' in toast and 'progressStarted' in toast
assert 'UpdateWithTag' in toast and 'progressValue' in toast
assert 'RegCreateKey' not in toast and 'RegSetValue' not in toast
assert 'L"USB Guard 需要管理员权限"' in toast
assert '请在 Windows 用户账户控制对话框中点击“是”' in toast
assert toast.index('toast.Show(L"USB Guard 需要管理员权限"') < toast.index('auto result = LaunchWorker(')
assert 'Sleep(' not in toast
assert '<actions>' not in toast
assert 'ShellExecuteExW' in toast and 'GetNamedPipeClientProcessId' in toast
assert 'WaitForSingleObject(launch.hProcess' in toast
assert 'USBGuardToast' in cmake and 'USBGuardUI' in cmake

for name, level in [('app.manifest', 'requireAdministrator'), ('toast.manifest', 'asInvoker')]:
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
    sizes.add((width or 256, height or 256))
assert {(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)} <= sizes
print('Source, manifest and icon checks passed')
