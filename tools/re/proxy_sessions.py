"""Split PakonIMAu_proxy.log into sessions (each PIBegin) and print, per session, the
call list and the args of the calls that carry geometry."""
import re, sys
text = open(sys.argv[1], 'rb').read().decode('latin1')
calls = re.split(r'\n(?=#\d+ PI\w+ tid)', text)
sess = []
for c in calls:
    m = re.match(r'#(\d+) (PI\w+) tid', c)
    if not m: continue
    if m.group(2) == 'PIBegin': sess.append([])
    if sess: sess[-1].append((int(m.group(1)), m.group(2), c))
want = set(sys.argv[2].split(',')) if len(sys.argv) > 2 else set()
for i, s in enumerate(sess):
    names = [n for _, n, _ in s]
    print(f'session {i}: {len(s)} calls, saves {names.count("PISaveFilePlanar_8")}, scenes {names.count("PIAnselAddScene")}')
    if str(i) in want:
        seen = set()
        for n, name, c in s:
            if name in seen and name not in ('PISaveFilePlanar_8',): continue
            seen.add(name)
            lines = c.split('\n')
            print('  ' + '\n  '.join(l[:200] for l in lines[:6]))
