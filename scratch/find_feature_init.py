import struct
import glob

driver_store = r"C:\WINDOWS\System32\DriverStore\FileRepository"
matches = glob.glob(driver_store + r"\nv_dispi.inf_amd64_*\_nvngx.dll")
nvngx_path = matches[0]

with open(nvngx_path, "rb") as f:
    data = f.read()

s = b"DLSSNR.FeatureInitResult"
idx = data.find(s)
print(f"DLSSNR.FeatureInitResult in _nvngx.dll: offset 0x{idx:x}" if idx != -1 else "Not found in _nvngx.dll")

# Also check incoming nvngx_dlssnr.dll
with open("vendor/nvidia/incoming/nvngx_dlssnr.dll", "rb") as f:
    nr_data = f.read()
idx_nr = nr_data.find(s)
print(f"DLSSNR.FeatureInitResult in nvngx_dlssnr.dll: offset 0x{idx_nr:x}" if idx_nr != -1 else "Not found in nvngx_dlssnr.dll")
