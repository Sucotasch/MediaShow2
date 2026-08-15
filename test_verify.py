import struct, os, zipfile, sys

results = {}

def check(name, ok, detail=""):
    results[name] = bool(ok)
    status = "PASS" if ok else "FAIL"
    print(f"  {status}: {name} {detail}")

def verify_dll(path, label):
    print(f"\n{'='*60}")
    print(f"  {label}: {os.path.basename(path)}")
    print(f"{'='*60}")

    with open(path, 'rb') as f:
        data = f.read()

    print(f"  File size: {len(data):,} bytes")

    pe_off = struct.unpack_from('<I', data, 0x3C)[0]
    sig = data[pe_off:pe_off+4]
    check(label + "_PE", sig == b'PE\x00\x00', f"(PE signature {sig})")
    if sig != b'PE\x00\x00':
        return

    machine = struct.unpack_from('<H', data, pe_off + 4)[0]
    arch = "x86" if machine == 0x014C else "x64" if machine == 0x8664 else "unknown"
    print(f"  Machine: 0x{machine:04X} ({arch})")

    opt_off = pe_off + 24
    magic = struct.unpack_from('<H', data, opt_off)[0]
    dll_chars = struct.unpack_from('<H', data, opt_off + 70)[0]
    print(f"  DLL Characteristics: 0x{dll_chars:04X}")
    check(label + "_ASLR", bool(dll_chars & 0x0040), "(ASLR)")
    check(label + "_DEP",  bool(dll_chars & 0x0100), "(DEP)")

    export_rva_offset = opt_off + 96 if magic == 0x10b else opt_off + 112
    export_rva = struct.unpack_from('<I', data, export_rva_offset)[0]
    if export_rva > 0:
        num_sections = struct.unpack_from('<H', data, pe_off + 6)[0]
        opt_hdr_size = struct.unpack_from('<H', data, pe_off + 20)[0]
        sec_start = pe_off + 24 + opt_hdr_size
        found = []
        for i in range(num_sections):
            sec = sec_start + i * 40
            vaddr = struct.unpack_from('<I', data, sec + 12)[0]
            vsize = struct.unpack_from('<I', data, sec + 8)[0]
            raw_ptr = struct.unpack_from('<I', data, sec + 20)[0]
            if vaddr <= export_rva < vaddr + vsize:
                offset = raw_ptr + (export_rva - vaddr)
                num_names = struct.unpack_from('<I', data, offset + 24)[0]
                name_ptr_rva = struct.unpack_from('<I', data, offset + 32)[0]
                for j in range(num_names):
                    n_rva = struct.unpack_from('<I', data, name_ptr_rva - vaddr + raw_ptr + j * 4)[0]
                    name = data[n_rva - vaddr + raw_ptr:n_rva - vaddr + raw_ptr + 60].split(b'\x00')[0].decode()
                    found.append(name)
                break
        required = ['ListLoad', 'ListLoadW', 'ListLoadNext', 'ListLoadNextW',
                    'ListCloseWindow', 'ListGetDetectString', 'ListSetDefaultParams',
                    'ListSendCommand', 'ListNotificationReceived']
        missing = [r for r in required if not any(r in f for f in found)]
        check(label + "_exports", not missing, f"(missing: {missing or 'none'})")
    else:
        check(label + "_exports", False, "(no export table)")

    check(label + "_detect", b'MULTIMEDIA' in data, "(detect string)")

def verify_archive(path, label):
    print(f"\n{'='*60}")
    print(f"  Archive: {os.path.basename(path)}")
    print(f"{'='*60}")
    ok = True
    try:
        with zipfile.ZipFile(path, 'r') as zf:
            names = zf.namelist()
            for info in zf.infolist():
                print(f"    {info.filename:30s} {info.file_size:>10,} bytes")
            # Плагин внутри .wlx лежит под именем MediaShow2.wlx / .wlx64
            # (package.py кладёт DLL с внутренним именем без расширения .dll)
            dll_name = [n for n in names if n != 'pluginst.inf']
            check(label + "_dll", len(dll_name) == 1, f"(plugin entries: {len(dll_name)})")
            check(label + "_inf", 'pluginst.inf' in names, "")
            if dll_name:
                with zf.open(dll_name[0]) as f:
                    dll_data = f.read()
                pe_off = struct.unpack_from('<I', dll_data, 0x3C)[0]
                machine = struct.unpack_from('<H', dll_data, pe_off + 4)[0]
                arch = "x86" if machine == 0x014C else "x64" if machine == 0x8664 else "unknown"
                print(f"    DLL inside: {arch}, {len(dll_data):,} bytes")
    except Exception as e:
        ok = False
        print(f"    ERROR: {e}")
    check(label + "_opens", ok, "")

base = os.path.dirname(os.path.abspath(__file__))

print("=" * 60)
print("  MediaShow2 Plugin Test Report")
print("=" * 60)

dll_x86 = os.path.join(base, 'build', 'bin', 'Release', 'MediaShow2.dll')
dll_x64 = os.path.join(base, 'build-x64', 'bin', 'Release', 'MediaShow2_x64.dll')

if os.path.exists(dll_x86):
    verify_dll(dll_x86, 'x86 DLL')
else:
    check('x86_build', False, f"(missing {dll_x86})")
if os.path.exists(dll_x64):
    verify_dll(dll_x64, 'x64 DLL')
else:
    check('x64_build', False, f"(missing {dll_x64})")

verify_archive(os.path.join(base, 'MediaShow2.wlx'), 'x86 Archive')
verify_archive(os.path.join(base, 'MediaShow2.wlx64'), 'x64 Archive')

failed = [k for k, v in results.items() if not v]
print(f"\n{'='*60}")
if failed:
    print(f"  FAILED CHECKS: {failed}")
    print(f"{'='*60}")
    sys.exit(1)
print("  ALL CHECKS PASSED")
print(f"{'='*60}")
sys.exit(0)
