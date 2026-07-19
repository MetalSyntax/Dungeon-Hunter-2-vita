import sys
sys.path.append('/Users/metalsyntax/vita-tools/vita-parse-core')
from core import CoreParser

c = CoreParser('psp2core-1783994658-0x001fa831dd-eboot.bin.psp2dmp')
print("Memory Segments:")
for seg in c.segments:
    if len(seg.data) > 4 and seg.data[:4] == b'\x7fELF':
        print(f"ELF found at {hex(seg.vaddr)} (size {hex(len(seg.data))})")
