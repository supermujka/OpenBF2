import re, sys, zipfile

z = zipfile.ZipFile(sys.argv[1])
t = z.read('gamemodes/gpm_cq/64/GamePlayObjects.con').decode('latin-1')
cur = None
for line in t.splitlines():
    m = re.match(r'\s*(?:Object|ObjectTemplate)\.create\s+(?:\S+\s+)?(\S+)', line)
    if m:
        cur = m.group(1)
    m = re.match(r'\s*Object\.absolutePosition\s+(\S+)', line)
    if m and cur and re.match(r'CPNAME_SK_64_[A-Za-z]+$', cur):
        print(cur, m.group(1))
