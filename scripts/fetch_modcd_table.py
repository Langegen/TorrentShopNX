#!/usr/bin/env python3
"""
Fetches ModCD's table.md and generates resources/modcd.json.
Format:
{
  "format": 1,
  "titles": {
    "0100F2C0115B6000": {
      "name": "The Legend of Zelda: Tears of the Kingdom",
      "mods": 12,
      "versions": [393216],
      "types": ["gfx"]
    }
  }
}
"""
import urllib.request
import re
import json
import sys
import os

URL = 'https://raw.githubusercontent.com/kawaii-flesh/ModCD/main/table.md'

def main():
    print(f"Fetching {URL}...")
    req = urllib.request.Request(URL, headers={'User-Agent': 'Mozilla/5.0'})
    try:
        content = urllib.request.urlopen(req, timeout=15).read().decode('utf-8', errors='ignore')
    except Exception as e:
        print(f"Error fetching table.md: {e}", file=sys.stderr)
        return 1

    titles = {}
    for line in content.splitlines():
        if not line.startswith('|'):
            continue
        parts = [p.strip() for p in line.split('|')[1:-1]]
        if len(parts) < 3:
            continue
        tid_str, name, mods_info = parts[0], parts[1], parts[2]
        if not re.match(r'^[0-9A-Fa-f]{16}$', tid_str):
            continue

        mod_blocks = [b for b in mods_info.split('<br><br>') if b.strip()]
        count = len(mod_blocks) if mod_blocks else 1

        versions = set()
        for m in re.finditer(r'Supported Versions\*\*:\s*([0-9,\s]+)', mods_info):
            for v in m.group(1).split(','):
                v = v.strip()
                if v.isdigit():
                    versions.add(int(v))

        types = set()
        for m in re.finditer(r'Type\*\*:\s*([a-zA-Z0-9_-]+)', mods_info):
            types.add(m.group(1).strip().lower())

        titles[tid_str.upper()] = {
            'name': name,
            'mods': count,
            'versions': sorted(list(versions)),
            'types': sorted(list(types)) if types else ['gfx']
        }

    out_path = os.path.join(os.path.dirname(__file__), '..', 'resources', 'modcd.json')
    out_path = os.path.abspath(out_path)
    existing_patch_versions = {}
    if os.path.exists(out_path):
        try:
            with open(out_path, 'r', encoding='utf-8') as ef:
                old_data = json.load(ef)
                for old_tid, old_val in old_data.get('titles', {}).items():
                    if 'patch_versions' in old_val:
                        existing_patch_versions[old_tid.upper()] = old_val['patch_versions']
        except Exception:
            pass

    for tid, info in titles.items():
        if tid in existing_patch_versions:
            info['patch_versions'] = existing_patch_versions[tid]

    output = {
        'format': 1,
        'titles': titles
    }

    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(output, f, ensure_ascii=False, indent=2)

    print(f"Wrote {len(titles)} titles to {out_path}")
    return 0

if __name__ == '__main__':
    sys.exit(main())
