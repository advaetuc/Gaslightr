import struct
import glob

driver_store = r"C:\WINDOWS\System32\DriverStore\FileRepository"
matches = glob.glob(driver_store + r"\nv_dispi.inf_amd64_*\_nvngx.dll")
nvngx_path = matches[0]

with open(nvngx_path, "rb") as f:
    data = f.read()

# WinVerifyTrust is at offset 0xbe410 in .rdata
# In IAT / ILT, let's find the pointer to WinVerifyTrust
# Let's find IAT entry for WinVerifyTrust
import re
iat_target = struct.pack("<Q", 0x1800be410)
# But in IAT it's loaded at runtime, so code does call qword ptr [__imp_WinVerifyTrust]
# Let's find import directory
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
opt_header = e_lfanew + 24
import_rva = struct.unpack_from("<I", data, opt_header + 120)[0]
print(f"Import directory RVA: 0x{import_rva:x}")

def rva_to_offset(rva):
    return 0x400 + (rva - 0x1000)

# Search for calls to WinVerifyTrust in .text
# In x64, call qword ptr [rip + disp] is FF 15 disp
# Let's find the IAT RVA for WinVerifyTrust
# Search for string "WinVerifyTrust"
wvt_str_off = data.find(b"WinVerifyTrust\x00")
wvt_str_rva = 0xa8000 + (wvt_str_off - 0xa6e00)
print(f"WinVerifyTrust string RVA: 0x{wvt_str_rva:x}")

# Find IAT entry referencing this hint/name RVA
# In ILT or IAT, it stores (wvt_str_rva - 2)
hint_name_rva = wvt_str_rva - 2
hint_bytes = struct.pack("<Q", hint_name_rva)
ilt_off = data.find(hint_bytes)
print(f"ILT offset: 0x{ilt_off:x}" if ilt_off != -1 else "ILT not found")

# Let's find references to the IAT in .text
if ilt_off != -1:
    # ILT is in .rdata. The matching IAT entry is in .rdata as well
    # Let's find where FF 15 calls it
    pass
