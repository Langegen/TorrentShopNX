import sys
import re

if len(sys.argv) < 2:
    print("Usage: python dump_literals.py <file>")
    sys.exit(1)

fpath = sys.argv[1]
with open(fpath, 'r', encoding='utf-8', errors='ignore') as f:
    lines = f.readlines()

literal_re = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
for i, line in enumerate(lines):
    line_clean = re.sub(r'//.*', '', line)
    matches = [m.group(1) for m in literal_re.finditer(line_clean) if re.search(r'[\u0400-\u04FF]', m.group(1))]
    if matches:
        print(f"Line {i+1}: {matches}")
        print(f"   {line.strip()[:100]}")
