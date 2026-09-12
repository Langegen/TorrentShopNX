import json
import re
import glob
import os

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

loaded_langs = {}
for code, path in LANGUAGES.items():
    if os.path.exists(path):
        loaded_langs[code] = json.load(open(path, encoding='utf-8'))
    else:
        print(f"Warning: File {path} not found!")

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
    for lang_code, data in loaded_langs.items():
        if not key_exists(data, k):
            missing.append((k, f, lang_code))

print(f"Total languages checked: {len(loaded_langs)} ({', '.join(loaded_langs.keys())})")
print(f"Total referenced keys: {len(found_keys)}")
print(f"Missing keys: {len(missing)}")
for k, f, lang in missing:
    print(f"  [{lang}] Missing key \"{k}\" referenced in {f}")

if missing:
    exit(1)
