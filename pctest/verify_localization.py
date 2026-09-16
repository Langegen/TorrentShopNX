import json
import re
import glob
import os
import sys

LANGUAGES = {
    'RU': 'resources/i18n/ru/app.json',
    'EN': 'resources/i18n/en-US/app.json',
    'ES': 'resources/i18n/es/app.json',
    'FR': 'resources/i18n/fr/app.json',
    'DE': 'resources/i18n/de/app.json',
    'IT': 'resources/i18n/it/app.json',
    'PT-BR': 'resources/i18n/pt-BR/app.json',
    'ZH-HANS': 'resources/i18n/zh-Hans/app.json',
    'JA': 'resources/i18n/ja/app.json'
}

def get_all_keys(d, prefix='app'):
    keys = {}
    for k, v in d.items():
        full_key = f"{prefix}/{k}"
        if isinstance(v, dict):
            keys.update(get_all_keys(v, full_key))
        else:
            keys[full_key] = v
    return keys

loaded_keys = {}
for code, path in LANGUAGES.items():
    if not os.path.exists(path):
        print(f"ERROR: {path} not found!")
        sys.exit(1)
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    loaded_keys[code] = get_all_keys(data)

base_keys = set(loaded_keys['RU'].keys())
print(f"Total keys in RU dictionary: {len(base_keys)}")

parity_errors = 0
for code, keys in loaded_keys.items():
    diff_missing = base_keys - set(keys.keys())
    diff_extra = set(keys.keys()) - base_keys
    if diff_missing:
        print(f"[{code}] Missing {len(diff_missing)} keys: {list(diff_missing)[:5]}...")
        parity_errors += len(diff_missing)
    if diff_extra:
        print(f"[{code}] Extra {len(diff_extra)} keys: {list(diff_extra)[:5]}...")
        parity_errors += len(diff_extra)

if parity_errors == 0:
    print("SUCCESS: 100% key parity across all 9 languages!")

# Check Cyrillic literals in source/ui
ui_files = glob.glob('source/ui/**/*.cpp', recursive=True) + \
           glob.glob('source/ui/**/*.hpp', recursive=True) + \
           glob.glob('source/ui/**/*.h', recursive=True)

literal_re = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
cyrillic_literals = []

for fpath in sorted(ui_files):
    with open(fpath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
    for i, line in enumerate(lines):
        line_clean = re.sub(r'//.*', '', line)
        for m in literal_re.finditer(line_clean):
            s = m.group(1)
            if re.search(r'[\u0400-\u04FF]', s):
                cyrillic_literals.append((fpath, i + 1, s))

print(f"\nRemaining Cyrillic string literals in source/ui: {len(cyrillic_literals)}")
file_counts = {}
for fpath, lnum, s in cyrillic_literals:
    file_counts[fpath] = file_counts.get(fpath, 0) + 1

for fpath, count in sorted(file_counts.items(), key=lambda x: -x[1]):
    print(f"  {fpath}: {count} literals")

# Check XML files
xml_files = glob.glob('resources/xml/**/*.xml', recursive=True)
xml_cyrillic = 0
for fpath in xml_files:
    with open(fpath, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    if re.search(r'[\u0400-\u04FF]', content):
        print(f"WARNING: Cyrillic found in {fpath}")
        xml_cyrillic += 1

if xml_cyrillic == 0:
    print("SUCCESS: 0 Cyrillic characters in resources/xml/!")
