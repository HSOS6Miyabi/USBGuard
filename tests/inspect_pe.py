"""Validate portable USBGuard PE machine, resources and imported DLLs.

The audit uses only Python's standard library and runs for every Release EXE.
"""
from pathlib import Path
import struct
import sys

SYSTEM_DLLS = {
    'advapi32.dll', 'bcrypt.dll', 'cfgmgr32.dll', 'comctl32.dll',
    'comdlg32.dll', 'combase.dll', 'crypt32.dll', 'gdi32.dll',
    'gdi32full.dll', 'imm32.dll', 'kernel32.dll', 'kernelbase.dll',
    'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'propsys.dll',
    'rpcrt4.dll', 'shell32.dll', 'shlwapi.dll', 'user32.dll',
    'uxtheme.dll', 'version.dll', 'winmm.dll', 'ws2_32.dll',
}
RUNTIME_PREFIXES = (
    'msvcp', 'msvcr', 'vcruntime', 'concrt', 'ucrt',
    'api-ms-win-crt-', 'vcomp', 'libgcc', 'libstdc++', 'libwinpthread',
)


def inspect(path: Path, expected_machine: int) -> set[str]:
    data = path.read_bytes()
    assert data[:2] == b'MZ' and len(data) > 1024, f'{path}: missing MZ header'
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[pe:pe+4] == b'PE\0\0', f'{path}: missing PE signature'
    machine, section_count = struct.unpack_from('<HH', data, pe+4)
    assert machine == expected_machine, (
        f'{path}: machine 0x{machine:04x}, expected 0x{expected_machine:04x}'
    )
    assert section_count >= 2
    optional = pe + 24
    optional_size = struct.unpack_from('<H', data, pe+20)[0]
    magic = struct.unpack_from('<H', data, optional)[0]
    assert magic in (0x10B, 0x20B)
    subsystem = struct.unpack_from('<H', data, optional+68)[0]
    assert subsystem == 2, f'{path}: executable is not a Windows GUI application'
    min_system = struct.unpack_from('<HH', data, optional+48)
    if expected_machine == 0x014C:
        assert min_system <= (5, 1), (
            f'{path}: x86 subsystem requires {min_system[0]}.{min_system[1]}, '
            'but Windows XP requires 5.01 or earlier'
        )
    directory = optional + (112 if magic == 0x20B else 96)
    assert directory+14*8 <= optional+optional_size, 'Truncated PE data directories'
    resource_rva, resource_size = struct.unpack_from('<II', data, directory+2*8)
    assert resource_rva and resource_size, f'{path}: missing icon/manifest resources'
    image_base = struct.unpack_from(
        '<Q' if magic == 0x20B else '<I', data, optional+(24 if magic == 0x20B else 28)
    )[0]
    sections = []
    for index in range(section_count):
        pos = optional+optional_size+index*40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            '<IIII', data, pos+8
        )
        sections.append((virtual_address, max(virtual_size, raw_size),
                         raw_pointer, raw_size))

    def offset(rva: int) -> int:
        for start, mapped_size, raw, raw_size in sections:
            displacement = rva-start
            if 0 <= displacement < mapped_size and displacement < raw_size:
                at = raw+displacement
                if at < len(data):
                    return at
        raise AssertionError(f'{path}: invalid import RVA 0x{rva:x}')

    def dllname(rva: int) -> str:
        position = offset(rva)
        end = data.find(b'\0', position, min(position+256, len(data)))
        assert end > position, f'{path}: invalid imported DLL name'
        return data[position:end].decode('ascii').lower()

    imports: set[str] = set()
    imported_functions: set[str] = set()
    def inspect_thunks(rva: int):
        if not rva:
            return
        thunk_size = 8 if magic == 0x20B else 4
        ordinal_flag = 1 << (63 if magic == 0x20B else 31)
        fmt = '<Q' if magic == 0x20B else '<I'
        for index in range(8192):
            entry_offset = offset(rva + index * thunk_size)
            value = struct.unpack_from(fmt, data, entry_offset)[0]
            if value == 0:
                break
            if value & ordinal_flag:
                continue
            name_offset = offset(value) + 2
            end = data.find(b'\0', name_offset, min(len(data), name_offset + 256))
            assert end > name_offset, f'{path}: malformed imported function name'
            imported_functions.add(data[name_offset:end].decode('ascii'))
        else:
            raise AssertionError(f'{path}: unterminated function import thunk table')

    # Regular IMAGE_IMPORT_DESCRIPTOR entries: 20 bytes each.
    import_rva, import_size = struct.unpack_from('<II', data, directory+8)
    if import_rva:
        assert import_size >= 20
        base = offset(import_rva)
        for i in range(import_size//20):
            descriptor = struct.unpack_from('<IIIII', data, base+i*20)
            if not any(descriptor):
                break
            imports.add(dllname(descriptor[3]))
            inspect_thunks(descriptor[0] or descriptor[4])
        else:
            raise AssertionError(f'{path}: unterminated PE import table')

    # Delayed imports can hide an otherwise unobserved runtime dependency.
    delay_rva, delay_size = struct.unpack_from('<II', data, directory+13*8)
    if delay_rva:
        assert delay_size >= 32
        base = offset(delay_rva)
        for i in range(delay_size//32):
            values = struct.unpack_from('<IIIIIIII', data, base+i*32)
            if not any(values):
                break
            name_rva = values[1] if values[0] & 1 else values[1]-image_base
            imports.add(dllname(name_rva))
        else:
            raise AssertionError(f'{path}: unterminated delay import table')

    # Locate the RT_VERSION resource and inspect its actual serialized data.
    # This validates what Windows Explorer reads from the executable.
    def resource_entries(relative: int):
        directory_offset = offset(resource_rva + relative)
        named, numbered = struct.unpack_from('<HH', data, directory_offset + 12)
        assert named + numbered < 2048, f'{path}: unreasonable resource entry count'
        entries = []
        for i in range(named + numbered):
            key, value = struct.unpack_from('<II', data, directory_offset + 16 + i * 8)
            entries.append((key, value))
        return entries

    types = resource_entries(0)
    version_types = [value for key, value in types if key == 16]
    assert len(version_types) == 1 and version_types[0] & 0x80000000, (
        f'{path}: missing RT_VERSION resource'
    )
    version_ids = resource_entries(version_types[0] & 0x7fffffff)
    version_entries = [value for key, value in version_ids if key == 1]
    assert len(version_entries) == 1 and version_entries[0] & 0x80000000, (
        f'{path}: missing VERSIONINFO ID 1'
    )
    languages = resource_entries(version_entries[0] & 0x7fffffff)
    assert languages and not (languages[0][1] & 0x80000000)
    resource_data = offset(resource_rva + languages[0][1])
    version_rva, version_length = struct.unpack_from('<II', data, resource_data)
    version_start = offset(version_rva)
    version_blob = data[version_start:version_start + version_length]
    assert len(version_blob) == version_length and version_length >= 92
    assert struct.unpack_from('<H', version_blob, 0)[0] == version_length
    assert 'VS_VERSION_INFO'.encode('utf-16le') in version_blob
    assert struct.pack('<I', 0xFEEF04BD) in version_blob

    expected_description = (
        'USBGuard Mini - Portable USB write protection'
        if path.stem == 'USBGuardMini'
        else 'USBGuard - USB drive write protection manager'
    )
    expected_strings = (
        'Kenvix', 'Kenvix <i@kenvix.com>',
        'Copyright (C) 2026 Kenvix <i@kenvix.com>',
        'https://kenvix.com', 'USBGuard', expected_description,
        path.name, '1.1.0',
    )
    for value in expected_strings:
        assert value.encode('utf-16le') + b'\x00\x00' in version_blob, (
            f'{path}: VERSIONINFO is missing {value!r}'
        )

    assert imports, f'{path}: no DLL import descriptors were found'
    foreign = sorted(d for d in imports if
                     d not in SYSTEM_DLLS and
                     not d.startswith(('api-ms-win-core-', 'ext-ms-win-')))
    runtimes = sorted(d for d in imports if d.startswith(RUNTIME_PREFIXES))
    assert not runtimes, f'{path}: dynamic C/C++ runtime required: {runtimes}'
    assert not foreign, f'{path}: unexpected non-system DLL dependencies: {foreign}'
    if expected_machine == 0x014C:
        newer_apis = {
            'GetNamedPipeClientProcessId', 'GetTickCount64', 'CancelIoEx',
            'GetProcessId', 'SetProcessDpiAwarenessContext',
            'SetProcessDPIAware', 'GetFinalPathNameByHandleW',
            'InitializeCriticalSectionEx', 'InitOnceExecuteOnce',
        }
        direct = sorted(imported_functions & newer_apis)
        assert not direct, f'{path}: newer Windows functions imported directly: {direct}'
    print(f'{path}: PE machine=0x{machine:04x}; GUI; icon/manifest/version; '
          f'only Windows DLL imports: {", ".join(sorted(imports))}')
    return imports


if __name__ == '__main__':
    assert len(sys.argv) == 3, 'usage: inspect_pe.py EXE 014c|8664|aa64'
    inspect(Path(sys.argv[1]), int(sys.argv[2], 16))
