"""Creates the 7z archives used by pctest/test_extract_safety.

Usage: python make_extract_safety_archives.py <outDir>
Needs 7-Zip 23+ (ARM64 filter). Set SEVENZ to override the 7z binary path.
"""
import os, random, shutil, subprocess, sys

SEVENZ = os.environ.get("SEVENZ") or next(
    (p for p in ("C:/Program Files/7-Zip/7z.exe", "/c/Program Files/7-Zip/7z.exe", "/usr/bin/7z", "/usr/bin/7zz")
     if os.path.exists(p)), "7z")
out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
os.chdir(out)

def run(args):
    r = subprocess.run([SEVENZ] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        sys.exit(1)

def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)

rnd = random.Random(1234)

# 1) one solid LZMA2+ARM64 folder > 64 MB -> SDK defers to libarchive
for d in ("src_big", "src_small", "src_enc"):
    shutil.rmtree(d, ignore_errors=True)
line = b"".join(b"line %08d some payload text for compression\n" % i for i in range(100000))
big = (line * 17)[: 80 * 1024 * 1024]
write("src_big/switch/hl2_nx/hl2/payload.bin", big)
# fake AArch64 code: lots of BL/ADRP-looking words so the ARM64 filter really rewrites bytes
code = bytearray()
for i in range(200000):
    op = rnd.choice([0x94000000 | rnd.getrandbits(26), 0x90000000 | rnd.getrandbits(29), rnd.getrandbits(32)])
    code += op.to_bytes(4, "little")
write("src_big/switch/hl2_nx/lib/libclient.so", bytes(code))
write("src_big/switch/hl2_nx/readme.txt", b"hello\n")
for f in ("big_arm64.7z", "small_corrupt.7z", "encrypted.7z"):
    if os.path.exists(f):
        os.remove(f)
run(["a", "-t7z", "-mf=ARM64", "-m0=LZMA2:d=1m", "-mx1", "-ms=on", "big_arm64.7z", "./src_big/*"])

# 2) small non-solid archive (SDK path), last file's packed data corrupted -> CRC error
for i in range(6):
    write("src_small/game/data/file%d.bin" % i, bytes(rnd.getrandbits(8) for _ in range(20000)))
run(["a", "-t7z", "-m0=LZMA2", "-mx1", "-ms=off", "small_corrupt.7z", "./src_small/*"])
with open("small_corrupt.7z", "r+b") as f:
    # packed streams start right after the 32-byte signature header; file5 is last.
    f.seek(32 + 5 * 20000 + 5000)
    b = f.read(16)
    f.seek(32 + 5 * 20000 + 5000)
    f.write(bytes(x ^ 0x5A for x in b))

# 3) AES-encrypted data, readable header -> rejected before reading payload
write("src_enc/switch/secret/data.bin", bytes(rnd.getrandbits(8) for _ in range(8 * 1024 * 1024)))
run(["a", "-t7z", "-p123", "-mhe=off", "-mx1", "encrypted.7z", "./src_enc/*"])

for f in ("big_arm64.7z", "small_corrupt.7z", "encrypted.7z"):
    r = subprocess.run([SEVENZ, "l", "-slt", f], capture_output=True, text=True).stdout
    methods = sorted({l.split("=", 1)[1].strip() for l in r.splitlines() if l.startswith("Method")})
    print(f, os.path.getsize(f), methods)
