import struct
import glob
import re

driver_store = r"C:\WINDOWS\System32\DriverStore\FileRepository"
matches = glob.glob(driver_store + r"\nv_dispi.inf_amd64_*\nvngx.dll")
if not matches:
    print("nvngx.dll not found")
    exit(1)

nvngx_path = matches[0]

with open(nvngx_path, "rb") as f:
    data = f.read()

# Search wide and ascii strings
for s in [b"AllowUnsignedBinaries", b"A\x00l\x00l\x00o\x00w\x00U\x00n\x00s\x00i\x00g\x00n\x00e\x00d\x00B\x00i\x00n\x00a\x00r\x00i\x00e\x00s\x00",
          b"EnableDevSignatures", b"E\x00n\x00a\x00b\x00l\x00e\x00D\x00e\x00v\x00S\x00i\x00g\x00n\x00a\x00t\x00u\x00r\x00e\x00s\x00",
          b"DisableNgxSignatureChecks", b"OverrideFeatureDenial"]:
    idx = data.find(s)
    if idx != -1:
        print(f"Found {s} in nvngx.dll at file offset 0x{idx:x}")
    else:
        print(f"NOT found: {s[:25]}")
