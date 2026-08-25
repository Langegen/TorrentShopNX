import json
import re
import glob
import os

ru = json.load(open('resources/i18n/ru/app.json', encoding='utf-8'))
en = json.load(open('resources/i18n/en-US/app.json', encoding='utf-8'))

def key_exists(d, path):
    parts = path.strip('/').split('/')
    if parts[0] == 'app':
        parts = parts[1:]
    curr = d
    for p in parts:
        if isinstance(curr, dict) and p in curr:
            curr = curr[p]
        else:
            return False
    return isinstance(curr, str)

all_files = glob.glob('source/**/*', recursive=True) + glob.glob('resources/xml/**/*', recursive=True)
missing = []
found_keys = set()

for f in all_files:
    if not os.path.isfile(f):
        continue
    try:
        content = open(f, 'r', encoding='utf-8', errors='ignore').read()
    except Exception:
        continue
    # match string literals ending with _i18n or @i18n/
    for m in re.finditer(r'"(app/[^"]+)"(?:_i18n|\))', content):
        k = m.group(1)
        found_keys.add((k, f))
    for m in re.finditer(r'@i18n/(app/[^"\s>]+)', content):
        k = m.group(1)
        found_keys.add((k, f))
    for m in re.finditer(r'brls::getStr\(\s*"(app/[^"]+)"', content):
        k = m.group(1)
        found_keys.add((k, f))

for k, f in sorted(found_keys):
    if not key_exists(ru, k):
        missing.append((k, f, 'RU'))
    if not key_exists(en, k):
        missing.append((k, f, 'EN'))

print(f"Total referenced keys: {len(found_keys)}")
print(f"Missing keys: {len(missing)}")
for k, f, lang in missing:
    print(f"  [{lang}] Missing key \"{k}\" referenced in {f}")
