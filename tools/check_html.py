import re
with open(r'F:\NEO-OS\serial.log', 'r', encoding='utf-8', errors='replace') as f:
    content = f.read()
matches = re.findall(r'CN: ([0-9A-F]{4})', content)
unique = sorted(set(matches))
print(f'unique CN codes: {len(unique)}')
print('first 30:', unique[:30])
print('has 4E2D (中)?', '4E2D' in unique)
print('has 56FD (国)?', '56FD' in unique)
