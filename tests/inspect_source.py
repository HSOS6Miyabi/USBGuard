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
assert 'GetTickCount64()' in mini and 'kFinishDurationMs = 4200' in mini
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
    sizes.add((width or 256, height or 256))
assert {(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)} <= sizes
print('Source, manifest and icon checks passed')
