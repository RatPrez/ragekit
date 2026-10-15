#!/usr/bin/env python3
"""Compares two unencrypted (OPEN) RPF7 archives block by block: header, table rows, names block, and the stored bytes
of every file (nested archives recursively). Prints every difference in layout; exit 1 if any file's stored bytes differ.
    rpf_blockdiff.py <a.rpf> <b.rpf>"""
import struct, sys

def parse(d, base=0, size=None):
    magic, n, nl, enc = struct.unpack_from('<4I', d, base)
    assert magic == 0x52504637 and enc in (0, 0x4E45504F), 'not an unencrypted RPF7'
    rows = [d[base+16+i*16: base+32+i*16] for i in range(n)]
    names = d[base+16+n*16: base+16+n*16+nl]
    ents = []
    for i, r in enumerate(rows):
        y, x = struct.unpack_from('<2I', r)
        if x == 0x7fffff00:
            e = dict(kind='D', first=struct.unpack_from('<I', r, 8)[0], count=struct.unpack_from('<I', r, 12)[0], noff=y)
        elif x & 0x80000000 == 0:
            q = struct.unpack_from('<Q', r)[0]
            e = dict(kind='B', noff=q & 0xFFFF, stored=(q >> 16) & 0xFFFFFF, off=(q >> 40) & 0xFFFFFF,
                     unc=struct.unpack_from('<I', r, 8)[0], enc=struct.unpack_from('<I', r, 12)[0])
        else:
            e = dict(kind='R', noff=r[0] | r[1] << 8, stored=r[2] | r[3] << 8 | r[4] << 16, off=(r[5] | r[6] << 8 | r[7] << 16) & 0x7FFFFF,
                     sys=struct.unpack_from('<I', r, 8)[0], gfx=struct.unpack_from('<I', r, 12)[0])
        end = names.index(b'\0', e['noff'])
        e['name'] = names[e['noff']:end].decode()
        ents.append(e)
    for e in ents:
        if e['kind'] == 'D':
            for c in range(e['first'], e['first'] + e['count']):
                ents[c]['parent'] = ents.index(e)
    for i, e in enumerate(ents):
        parts = []
        j = i
        while j > 0:
            parts.append(ents[j]['name']); j = ents[j]['parent']
        e['path'] = '/'.join(reversed(parts)).lower()
    return dict(n=n, nl=nl, enc=enc, ents=ents, names=names, rows=rows, base=base)

def stored(d, base, e):
    if e['kind'] == 'D': return b''
    ln = e['stored'] or e.get('unc', 0)
    return d[base + e['off']*512: base + e['off']*512 + ln]

def compare(da, a, db, b, prefix, out):
    bad = 0
    out.append(f'{prefix or "<root>"}: entries {a["n"]} vs {b["n"]}, names length {a["nl"]} vs {b["nl"]}, enc {a["enc"]:#x} vs {b["enc"]:#x}')
    if [e['path'] for e in a['ents']] != [e['path'] for e in b['ents']]:
        out.append('  table order/paths differ')
    ma = {e['path']: e for e in a['ents']}; mb = {e['path']: e for e in b['ents']}
    if set(ma) != set(mb):
        out.append(f'  only in a: {sorted(set(ma)-set(mb))[:5]}  only in b: {sorted(set(mb)-set(ma))[:5]}'); bad += 1
    rowdiff = namediff = offdiff = 0
    for p in ma:
        if p not in mb: continue
        x, y = ma[p], mb[p]
        for k in ('kind', 'stored', 'unc', 'sys', 'gfx', 'enc', 'first', 'count'):
            if x.get(k) != y.get(k): out.append(f'  {p}: {k} {x.get(k)} vs {y.get(k)}'); rowdiff += 1
        if x.get('off') != y.get('off'): offdiff += 1
        if x['noff'] != y['noff']: namediff += 1
        if x['kind'] != 'D':
            sa, sb = stored(da, a['base'], x), stored(db, b['base'], y)
            if sa != sb:
                if p.endswith('.rpf') and x['kind'] == 'B' and sa[:4] == b'RPF7' and sb[:4] == b'RPF7':
                    pa, pb = parse(da, a['base'] + x['off']*512), parse(db, b['base'] + y['off']*512)
                    bad += compare(da, pa, db, pb, p + '/', out)
                else:
                    out.append(f'  {p}: stored bytes differ'); bad += 1
    out.append(f'  rows differing (ignoring offsets/name offsets): {rowdiff}; entries at a different block offset: {offdiff}; at a different name offset: {namediff}')
    out.append(f'  same set of names: {sorted(set(e["name"] for e in a["ents"])) == sorted(set(e["name"] for e in b["ents"]))}')
    out.append(f'  names block identical: {a["names"] == b["names"]}; table rows identical: {a["rows"] == b["rows"]}')
    return bad

a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
out = [f'sizes {len(a)} vs {len(b)}']
bad = compare(a, parse(a), b, parse(b), '', out)
print('\n'.join(out[:60]))
print('FILE DATA DIFFERENCES:', bad)
sys.exit(1 if bad else 0)
