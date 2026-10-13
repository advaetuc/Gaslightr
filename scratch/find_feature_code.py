import struct
import glob

driver_store = r"C:\WINDOWS\System32\DriverStore\FileRepository"
matches = glob.glob(driver_store + r"\nv_dispi.inf_amd64_*\_nvngx.dll")
nvngx_path = matches[0]

with open(nvngx_path, "rb") as f:
    data = f.read()

# Search for 8-byte pointer 0x00000001800bb698 or 4-byte RVA 0xbb698
ptr64 = struct.pack("<Q", 0x180000000 + 0xbb698)
idx64 = data.find(ptr64)
if idx64 != -1:
    print(f"Found 64-bit pointer at file offset 0x{idx64:x}")

# Or search RIP-relative disp
str_rva = 0xbb698
for i in range(len(data) - 4):
    # Could be any 4-byte displacement
    # insn_rva + length + disp = str_rva
    # So disp = str_rva - next_ip
    pass

# Let's search all occurrences of disp
# Let's check sections
def rva_to_offset(rva):
    return 0x400 + (rva - 0x1000)

for insn_len in (5, 6, 7, 8):
    for i in range(0x400, 0x400 + 0xa6a00):
        insn_rva = 0x1000 + (i - 0x400)
        disp = struct.unpack_from("<i", data, i)[0]
        if insn_rva + insn_len + disp == str_rva:
            # Check if previous byte is lea or mov
            prefix = data[i-3:i]
            print(f"Match at file offset 0x{i:x} (RVA 0x{insn_rva:x}), prefix: {prefix.hex()}, insn_len: {insn_len}")
