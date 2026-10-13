import struct
import glob
import re

driver_store = r"C:\WINDOWS\System32\DriverStore\FileRepository"
matches = glob.glob(driver_store + r"\nv_dispi.inf_amd64_*\_nvngx.dll")
nvngx_path = matches[0]

with open(nvngx_path, "rb") as f:
    data = f.read()

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
size_opt_header = struct.unpack_from("<H", data, e_lfanew + 20)[0]
opt_header_offset = e_lfanew + 24
image_base = struct.unpack_from("<Q", data, opt_header_offset + 24)[0]
section_headers_offset = opt_header_offset + size_opt_header
sections = []
for i in range(num_sections):
    sec_offset = section_headers_offset + i * 40
    name = data[sec_offset:sec_offset+8].rstrip(b"\x00").decode("latin1")
    vsize, vaddr, raw_size, raw_offset = struct.unpack_from("<IIII", data, sec_offset + 8)
    sections.append((name, vaddr, vsize, raw_offset, raw_size))

def offset_to_rva(offset):
    for name, vaddr, vsize, raw_offset, raw_size in sections:
        if raw_offset <= offset < raw_offset + raw_size:
            return vaddr + (offset - raw_offset)
    return None

def rva_to_offset(rva):
    for name, vaddr, vsize, raw_offset, raw_size in sections:
        if vaddr <= rva < vaddr + vsize:
            return raw_offset + (rva - vaddr)
    return None

# Find "AllowUnsignedBinaries" in data
target = b"AllowUnsignedBinaries"
for m in re.finditer(re.escape(target), data):
    rva = offset_to_rva(m.start())
    print(f"Found '{target.decode()}' string at RVA 0x{rva:x}")
    # Now search for references to this RVA in .text
    # In x64, typically lea rdx, [rip + disp]
    # We search for RIP-relative references
    for sname, svaddr, svsize, sraw_off, sraw_sz in sections:
        if sname == ".text":
            sec_data = data[sraw_off:sraw_off+sraw_sz]
            for i in range(len(sec_data) - 7):
                # Check for 48 8d 15 disp (lea rdx, [rip+disp]) or 48 8d 0d disp (lea rcx, [rip+disp])
                b0 = sec_data[i]
                b1 = sec_data[i+1]
                if b0 == 0x48 and b1 in (0x8D, 0x8B):
                    disp = struct.unpack_from("<i", sec_data, i + 3)[0]
                    insn_rva = svaddr + i
                    next_ip = insn_rva + 7
                    target_ip = next_ip + disp
                    if target_ip == rva:
                        print(f"  Referenced at RVA 0x{insn_rva:x} in {sname}")
                        fo = rva_to_offset(insn_rva)
                        print("  Code around reference:")
                        print(" ", data[fo-16:fo+64].hex())
