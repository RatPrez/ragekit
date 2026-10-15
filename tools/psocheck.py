#!/usr/bin/env python3
"""Compares ragekit's generic PSO walker with CodeWalker's XML, value by value.

    python tools/psocheck.py <rk.exe> <xml-dir> <pso file>...|@list.txt

<xml-dir> holds <file name>.pso.xml as written by `oracle pso <pairs.txt> <xml-dir>` (tests/oracle/PsoOracle.cs);
for every PSO file `rk psodump --flat` is run and each path is compared with the XML leaf at the same path
(names are compared as joaat hashes, numbers numerically, hash_XXXXXXXX equals the name that hashes to it).
Prints one line per file with differences and a summary. Exit code 1 when anything differs.
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor


def joaat(s):
    h = 0
    for c in s.lower().encode("latin1", "replace"):
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


def comp_key(c):
    if c.startswith("[") or c == "":
        return c
    m = re.fullmatch(r"hash_([0-9A-Fa-f]{8})", c)
    if m:
        return int(m.group(1), 16)
    return joaat(c)


def path_key(path):
    return tuple(comp_key(c) for c in path.strip("/").split("/"))


def leaf_value(el):
    if "value" in el.attrib:
        return el.attrib["value"]
    if "x" in el.attrib:
        return " ".join(el.attrib[k] for k in ("x", "y", "z", "w") if k in el.attrib)
    return " ".join((el.text or "").split())


def walk(el, path, out):
    kids = list(el)
    if not kids:
        out[path_key(path)] = leaf_value(el)
        return
    if all(k.tag == "Item" and not list(k) for k in kids):
        out[path_key(path)] = " ".join(leaf_value(k) for k in kids)
        return
    i = 0
    for k in kids:
        if k.tag == "Item":
            walk(k, path + "/[%d]" % i, out)
            i += 1
        else:
            walk(k, path + "/" + k.tag, out)


TOK = re.compile(r"0x[0-9a-fA-F]+|-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?|[A-Za-z_][A-Za-z_0-9]*")


def tokens(v):
    return TOK.findall(re.sub(r"\\x[0-9A-F]{2}", " ", v).replace('"', " "))


def num(t):
    try:
        if t.lower().startswith("0x"):
            return float(int(t, 16))
        return float(t)
    except ValueError:
        return None


def same_token(a, b):
    na, nb = num(a), num(b)
    if na is not None and nb is not None:
        return abs(na - nb) <= 1e-5 * max(1.0, abs(na), abs(nb))
    if a.lower() == b.lower():
        return True
    for x, y in ((a, b), (b, a)):
        m = re.fullmatch(r"hash_([0-9A-Fa-f]{8})", x)
        if m and joaat(y) == int(m.group(1), 16):
            return True
    if {a.lower(), b.lower()} <= {"true", "false", "1", "0"} and (a.lower() in ("true", "1")) == (b.lower() in ("true", "1")):
        return True
    return False


def same_value(a, b):
    ta, tb = tokens(a), tokens(b)
    if len(ta) != len(tb):
        # a flags value of "0" vs an empty tag
        return all(num(t) == 0 for t in ta + tb)
    return all(same_token(x, y) for x, y in zip(ta, tb))


def check(rk, xmldir, f):
    base = os.path.basename(f)
    xp = os.path.join(xmldir, base + ".pso.xml")
    if not os.path.exists(xp):
        return base, ["no xml"], 0
    try:
        root = ET.parse(xp).getroot()
    except ET.ParseError as e:
        return base, ["xml parse error: %s" % e], 0
    cw = {}
    walk(root, "/" + root.tag, cw)
    r = subprocess.run([rk, "psodump", "--flat", f], capture_output=True)
    mine = {}
    for line in r.stdout.decode("latin1").split("\n"):
        if "\t" in line:
            p, v = line.split("\t", 1)
            mine[path_key(p)] = v
    problems = []
    for k, v in mine.items():
        if k not in cw:
            if v.strip():
                problems.append("only ragekit: %s = %s" % ("/".join(map(str, k)), v[:60]))
            continue
        if not same_value(v, cw[k]):
            problems.append("differs %s: ragekit '%s' vs CodeWalker '%s'" % ("/".join(map(str, k)), v[:60], cw[k][:60]))
    for k, v in cw.items():
        if k not in mine and v.strip() and not all(num(t) == 0 for t in tokens(v) if num(t) is not None and False):
            problems.append("only CodeWalker: %s = %s" % ("/".join(map(str, k)), v[:60]))
    return base, problems, len(mine)


def main():
    rk, xmldir, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    if len(files) == 1 and files[0].startswith("@"):  # @list.txt: one path per line
        files = [l.strip() for l in open(files[0][1:]) if l.strip()]
    bad = 0
    leaves = 0
    with ThreadPoolExecutor(8) as ex:
        for base, problems, n in ex.map(lambda f: check(rk, xmldir, f), files):
            leaves += n
            if problems:
                bad += 1
                print("DIFF %s (%d): %s" % (base, len(problems), problems[0]))
                for p in problems[1:3]:
                    print("       " + p)
    print("%d files, %d leaf values compared, %d files with differences" % (len(files), leaves, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
