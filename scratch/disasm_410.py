import struct
import subprocess

with open(r"C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_feedb8c0271ca811\_nvngx.dll", "rb") as f:
    data = f.read()

def rva_to_offset(rva):
    return 0x400 + (rva - 0x1000)

start_rva = 0x41050
code = data[rva_to_offset(start_rva):rva_to_offset(start_rva)+350]

file_header = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0)
sec_header = struct.pack("<8sIIIIIIHHI", b".text\x00\x00\x00", 0, 0, len(code), 60, 0, 0, 0, 0, 0x60500020)
coff_obj = file_header + sec_header + code
with open("scratch/temp410.obj", "wb") as f:
    f.write(coff_obj)

dumpbin = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe"
out = subprocess.check_output([dumpbin, "/disasm", "scratch/temp410.obj"]).decode("utf-8", errors="ignore")
with open("scratch/disasm_410.txt", "w", encoding="utf-8") as f:
    f.write(out)

for line in out.splitlines()[:60]:
    print(line)
