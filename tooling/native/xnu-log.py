#!/usr/bin/env python3
"""Run on the tablet (root): read or clear the XNU console mailbox that k27 0055 (pstore-console=) fills.
   xnu-log.py read [ADDR] [SIZE] > log.txt     xnu-log.py clear [ADDR]     xnu-log.py decode DUMP.bin > log.txt
Default ADDR 0x9cb00000 (cvp@9cb00000: no-map, unused by Ubuntu and the firmware, retained across resets; tested
2026-11-10), SIZE 0x200000. Layout: 12-byte header ("DBG3" signature, start, size) and the ring stored three times (majority vote: the DRAM flips
scattered bits across a reset); "DBGC" (one copy, Linux persistent_ram_buffer) is still read. /dev/mem
maps it as Device memory, so only aligned 8-byte accesses are used (unaligned ones raise SIGBUS)."""
import mmap, os, struct, sys
op = sys.argv[1]
if op == 'decode':                       # a dump made elsewhere (fastboot -s piano-uefi oem dump 9cb00000 700000)
    raw = open(sys.argv[2], 'rb').read(); size = len(raw)
else:
    addr = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x9cb00000
    size = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x700000
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=addr)
    q = memoryview(m).cast('Q')
    if op == 'clear':
        q[0] = 0; q[1] = 0
        print('cleared'); sys.exit(0)
    raw = b''.join(struct.pack('<Q', q[i]) for i in range(size // 8))
sig, start, used = struct.unpack_from('<III', raw, 0)
# k27 0055: "DBGB" header + the ring 11 times ("DBG3": three) (bitwise majority against DRAM bit decay); "DBGC" = one copy.
dist = lambda a, b: bin(a ^ b).count('1')
copies = {0x42474244: 11, 0x33474244: 3}   # "DBGB": 11 copies, "DBG3": 3
best = min(copies, key=lambda c: dist(sig, c))
if dist(sig, best) <= 3:
    n = copies[best]; cap = (size - 16) // n
    cs = [raw[12 + k * cap:12 + (k + 1) * cap] for k in range(n)]
    # Bitwise majority without numpy: a bit-sliced 4-bit counter over big integers (n <= 15).
    b0 = b1 = b2 = b3 = 0
    for x in cs:
        v = int.from_bytes(x, 'little')
        c0 = b0 & v; b0 ^= v
        c1 = b1 & c0; b1 ^= c0
        c2 = b2 & c1; b2 ^= c1
        b3 ^= c2
    need = n // 2 + 1                          # count >= need
    count_bits = (b0, b1, b2, b3)
    # count >= need, compared bitwise from the top: gt | eq-chain
    ge = 0; eq = (1 << (8 * cap)) - 1
    for k in (3, 2, 1, 0):
        bit = count_bits[k]
        if (need >> k) & 1:
            eq &= bit
        else:
            ge |= eq & bit; eq &= ~bit
    data = (ge | eq).to_bytes(cap, 'little')
    fixed = sum(1 for x, y in zip(cs[0], data) if x != y)
elif dist(sig, 0x43474244) <= 3:
    cap = size - 12; data = raw[12:12 + cap]; fixed = 0
else:
    sys.stderr.write(f'no XNU log (signature {sig:#x})\n'); sys.exit(1)
start %= cap; used = min(used, cap)
out = data[:used] if used < cap else data[start:] + data[:start]
sys.stderr.write(f'XNU log: {used} bytes (start {start}, capacity {cap}, {fixed} bytes corrected by majority)\n')
sys.stdout.buffer.write(out)
