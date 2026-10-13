import struct
import glob

driver_store = r"C:\WINDOWS\System32\DriverStore\FileRepository"
for name in ("nvngx.dll", "_nvngx.dll"):
    matches = glob.glob(driver_store + rf"\nv_dispi.inf_amd64_*\{name}")
    if matches:
        with open(matches[0], "rb") as f:
            data = f.read()
        print(f"=== {name} ===")
        for s in [b"WinVerifyTrust", b"Wintrust.dll", b"WINTRUST.dll", b"CryptCATAdminCalcHashFromFileHandle"]:
            idx = data.find(s)
            print(f"  {s}: offset 0x{idx:x}" if idx != -1 else f"  {s}: Not found")
