"""Check release PE files without additional dependencies or running them."""
import pathlib
import struct
import sys


def inspect(path: pathlib.Path, expected_machine: int):
    data = path.read_bytes()
    assert data[:2] == b'MZ' and len(data) > 1024, f'{path}: missing MZ header'
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0', f'{path}: missing PE signature'
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    assert machine == expected_machine, f'{path}: machine 0x{machine:04x}, expected 0x{expected_machine:04x}'
    assert sections >= 2
    optional = pe + 24
    magic = struct.unpack_from('<H', data, optional)[0]
    assert magic in (0x10b, 0x20b)
    subsystem = struct.unpack_from('<H', data, optional + 68)[0]
    assert subsystem == 2, f'{path}: executable is not a Windows GUI application'
    # Windows resource directory is data directory index 2 (requires .rsrc section).
    dd = optional + (112 if magic == 0x20b else 96)
    rsrc_rva, rsrc_size = struct.unpack_from('<II', data, dd + 8 * 2)
    assert rsrc_rva != 0 and rsrc_size > 0, f'{path}: missing embedded icon/manifest resources'
    print(f'{path}: PE machine=0x{machine:04x}, GUI subsystem, resources present')


if __name__ == '__main__':
    assert len(sys.argv) == 3, 'usage: inspect_pe.py EXE 014c|8664|aa64'
    inspect(pathlib.Path(sys.argv[1]), int(sys.argv[2], 16))
