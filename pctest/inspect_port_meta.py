import os
import sys

def decode_bencode(data, idx=0):
    if idx >= len(data):
        return None, idx
    char = data[idx:idx+1]
    if char == b'i':
        end = data.index(b'e', idx)
        return int(data[idx+1:end]), end + 1
    elif char == b'l':
        idx += 1
        items = []
        while data[idx:idx+1] != b'e':
            item, idx = decode_bencode(data, idx)
            items.append(item)
        return items, idx + 1
    elif char == b'd':
        idx += 1
        d = {}
        while data[idx:idx+1] != b'e':
            key, idx = decode_bencode(data, idx)
            val, idx = decode_bencode(data, idx)
            if isinstance(key, bytes):
                try:
                    key = key.decode('utf-8')
                except Exception:
                    key = str(key)
            d[key] = val
        return d, idx + 1
    elif char.isdigit():
        colon = data.index(b':', idx)
        length = int(data[idx:colon])
        start = colon + 1
        val = data[start:start+length]
        return val, start + length
    else:
        raise ValueError(f"Unknown bencode char at {idx}: {char}")

def inspect_meta(meta_path, title_name):
    print(f"\n=======================================================")
    print(f" TORRENT: {title_name}")
    print(f" File: {meta_path}")
    print(f"=======================================================")
    with open(meta_path, "rb") as f:
        data = f.read()
    info, _ = decode_bencode(data)
    name = info.get("name", b"").decode("utf-8", errors="replace")
    print(f"Root Name: {name}")
    
    files = []
    if "files" in info:
        for f in info["files"]:
            length = f.get("length", 0)
            parts = [p.decode("utf-8", errors="replace") for p in f.get("path", [])]
            full_path = "/".join(parts)
            files.append((full_path, length))
    else:
        files.append((name, info.get("length", 0)))
        
    print(f"Total files in torrent: {len(files)}")
    print("Files preview:")
    for path, sz in files:
        sz_mb = sz / (1024 * 1024)
        if sz_mb >= 1.0:
            sz_str = f"{sz_mb:.2f} MB"
        else:
            sz_str = f"{sz / 1024:.1f} KB"
        print(f"  [{sz_str:>10}] {path}")
    return name, files

def main():
    metas = [
        ("cache/meta/8c6c01329b590abf9576986ab36d6dabef3d1ff7.meta", "Halo Combat Evolved (порт OpenCE) [NRO][ENG,RUS (Voice.)]"),
        ("cache/meta/e5509845ec2b586170238751177af702dd0d428e.meta", "Flatout 2 (хоумбрю) [NRO][ENG]"),
        ("cache/meta/f24283dd0a331489607780138b5a5c5bffb405d0.meta", "Silent Hill 2 EE (порт) [NRO][RUS]"),
        ("cache/meta/73dabdbfc9123f9334110b4c06fb20512cca4c0e.meta", "Call of Duty Black Ops Zombies (iOS-порт) [NRO][ENG]")
    ]
    for path, title in metas:
        if os.path.exists(path):
            inspect_meta(path, title)
        else:
            print(f"Not found: {path}")

if __name__ == "__main__":
    main()
