#!/usr/bin/env python3
import sys, struct

def main():
    if len(sys.argv) != 3:
        print("usage: elf2neap.py input.elf output.neap")
        sys.exit(1)
    elf_path = sys.argv[1]
    out_path = sys.argv[2]
    with open(elf_path, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF":
        print("not ELF")
        sys.exit(1)
    if elf[4] != 1:
        print("not 32-bit")
        sys.exit(1)
    e_entry = struct.unpack_from("<I", elf, 24)[0]
    e_phoff = struct.unpack_from("<I", elf, 28)[0]
    e_phentsize = struct.unpack_from("<H", elf, 42)[0]
    e_phnum = struct.unpack_from("<H", elf, 44)[0]
    code = b""
    data = b""
    code_vaddr = 0xFFFFFFFF
    entry_off = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", elf, off)[0]
        if p_type != 1:
            continue
        p_offset = struct.unpack_from("<I", elf, off + 4)[0]
        p_vaddr = struct.unpack_from("<I", elf, off + 8)[0]
        p_filesz = struct.unpack_from("<I", elf, off + 16)[0]
        if p_vaddr <= e_entry < p_vaddr + p_filesz:
            entry_off = e_entry - p_vaddr
            code_vaddr = p_vaddr
        seg = elf[p_offset:p_offset + p_filesz]
        if p_vaddr == code_vaddr:
            code += seg
        else:
            data += seg
    manifest = "name=hello\nversion=1.0\nauthor=neo\npermissions=\n"
    manifest_b = manifest.encode("utf-8")
    header_size = 72
    code_off = header_size
    data_off = code_off + len(code)
    manifest_off = data_off + len(data)
    header = struct.pack("<4sIIIIIIIIII",
        b"NEAP", 1, entry_off,
        code_off, len(code),
        data_off, len(data),
        0, manifest_off, len(manifest_b), 0)
    header += b"\x00" * 32
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(code)
        f.write(data)
        f.write(manifest_b)
    print(f"generated {out_path}: entry=0x{e_entry:x} off={entry_off} code={len(code)} data={len(data)}")

if __name__ == "__main__":
    main()
