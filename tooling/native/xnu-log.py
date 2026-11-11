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
base = 12
# k27 0055: "DBGB" header + the ring 11 times ("DBG3": three) (bitwise majority against DRAM bit decay); "DBGC" = one copy.
dist = lambda a, b: bin(a ^ b).count('1')
copies = {0x52474244: 11, 0x42474244: 11, 0x33474244: 3}   # "DBGR": header and ring 11 times; "DBGB": ring 11 times; "DBG3": 3
# The signature word is stored once. DRAM decay clears bits (1 -> 0) and almost never sets one, and after a hot run it cleared
# 5 of the 11 one-bits of "DBGR" (2026-11-11 07:50, 0x10054044, the GPU at 56 C: the log itself was intact). So a signature with
# no bit outside a known one and at most 8 of its bits lost is that one, as is one within 3 flips either way.
def matches(c):
    return dist(sig, c) <= 3 or (not (sig & ~c & 0xffffffff) and bin(c & ~sig).count('1') <= 8)
best = next((c for c in copies if matches(c)), min(copies, key=lambda c: dist(sig, c)))
def vote_word(words, need):
    return sum(1 << b for b in range(32) if sum((w >> b) & 1 for w in words) >= need)
if matches(best) and best == 0x52474244:
    # k27 0058: 256-byte header, {start, size} at words 4 + 2k (k < 11), the ring copies from byte 256; voted below.
    hdr = struct.unpack_from('<64I', raw, 0); base = 256
if matches(best):
    n = copies[best]; cap = (size - (256 if base == 256 else 16)) // n
    cs = [raw[base + k * cap:base + (k + 1) * cap] for k in range(n)]
    # Bitwise majority without numpy: a bit-sliced 4-bit counter over big integers (n <= 15).
    b0 = b1 = b2 = b3 = 0
    for x in cs:
        v = int.from_bytes(x, 'little')
        c0 = b0 & v; b0 ^= v
        c1 = b1 & c0; b1 ^= c0
        c2 = b2 & c1; b2 ^= c1
        b3 ^= c2
    need = n // 2 + 1                          # count >= need: plain majority, unless the histogram says otherwise
    # Adaptive threshold (2026-11-11): the DRAM decays mostly 1 -> 0, so a bit that was 1 often survives in only 4-5 of the 11
    # copies, while a 0 rarely shows up as 1 in more than 2. Count how many bits have each number k of copies at 1 and put the
    # threshold just above the valley between the two populations: the minimum between k = 2 and the peak of the 1-population
    # (not the global minimum: after heavy decay the 1-population peaks at 5-6 and its tail at 9-10 is smaller than the valley).
    # The histogram covers the whole capacity: unused zero bytes only add to k = 0.
    if n == 11:
        hist = []
        for k in range(n + 1):
            m = (1 << (8 * cap)) - 1
            for j, bit in enumerate((b0, b1, b2, b3)):
                m &= bit if (k >> j) & 1 else ~bit
            hist.append(bin(m).count('1'))
        peak = max(range(3, n + 1), key=lambda k: hist[k])
        valley = min(range(2, peak), key=lambda k: hist[k]) if peak > 2 else n // 2
        need = valley + 1
        sys.stderr.write(f'copies-at-1 histogram {hist}, threshold {need} of {n}\n')
    if base == 256:                            # the header's 11 {start, size} copies decay like the ring: the same threshold
        start = vote_word(hdr[4:26:2], need); used = vote_word(hdr[5:26:2], need)
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
# The header is stored once, so DRAM decay can flip a bit of start or size (seen 2026-11-11: size 8013 vs start 8141, the
# log's last 128 bytes cut). Before the ring wraps both hold the byte count: take their bitwise OR (decay clears bits).
if base == 12 and used != start and max(used, start) < cap: used = start = used | start
start %= cap; used = min(used, cap)
out = data[:used] if used < cap else data[start:] + data[:start]
sys.stderr.write(f'XNU log: {used} bytes (start {start}, capacity {cap}, {fixed} bytes corrected by majority)\n')
sys.stdout.buffer.write(out)
