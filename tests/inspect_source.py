from pathlib import Path
import struct
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
assert 'UpdateWithTag' in toast and "progressValue" in toast
assert 'ShellExecuteExW' in toast and 'GetNamedPipeClientProcessId' in toast
assert 'WaitForSingleObject(launch.hProcess' in toast
assert 'USBGuardToast' in cmake and 'USBGuardUI' in cmake

for name, level in [('app.manifest', 'requireAdministrator'), ('toast.manifest', 'asInvoker')]:
    parsed = ET.parse(root / name)
    actual = [e.attrib['level'] for e in parsed.iter() if e.tag.endswith('requestedExecutionLevel')]
    assert actual == [level], (name, actual)

ico = (root / 'assets' / 'usb-guard.ico').read_bytes()
reserved, kind, count = struct.unpack_from('<HHH', ico, 0)
assert reserved == 0 and kind == 1 and count >= 4
sizes = set()
for i in range(count):
    width, height = struct.unpack_from('<BB', ico, 6 + i * 16)
    sizes.add((width or 256, height or 256))
assert {(16, 16), (32, 32), (48, 48), (256, 256)} <= sizes
print('Source, manifest and icon checks passed')
