#!/usr/bin/env python3
"""Flatten nested <path> entries of Samsung's mixer_paths.xml into 'control<TAB>value' lines.

usage: expand-path.py mixer_paths.xml path-name [path-name ...]
Initial <ctl> entries outside any <path> are emitted with the pseudo path name "@init".
Includes (<include name=...>) are resolved relative to --include-dir (default: dir of the xml).
"""
import os
import sys
import xml.etree.ElementTree as ET


def load(path, incdir, paths, init):
    root = ET.parse(path).getroot()
    for el in root:
        if el.tag == "include":
            inc = os.path.join(incdir, os.path.basename(el.get("name")))
            if os.path.exists(inc):
                load(inc, incdir, paths, init)
        elif el.tag == "path":
            paths.setdefault(el.get("name"), []).append(el)
        elif el.tag == "ctl":
            init.append((el.get("name"), el.get("value")))


def expand(name, paths, out, seen=()):
    if name in seen:
        return
    for p in paths.get(name, []):
        for el in p:
            if el.tag == "ctl":
                out.append((el.get("name"), el.get("value")))
            elif el.tag == "path":
                expand(el.get("name"), paths, out, seen + (name,))


def main():
    args = sys.argv[1:]
    incdir = None
    if args and args[0] == "--include-dir":
        incdir, args = args[1], args[2:]
    xml, names = args[0], args[1:]
    incdir = incdir or os.path.dirname(os.path.abspath(xml))
    paths, init = {}, []
    load(xml, incdir, paths, init)
    out = []
    for n in names:
        if n == "@init":
            out += init
        elif n not in paths:
            sys.exit("no such path: " + n)
        else:
            expand(n, paths, out)
    for k, v in out:
        print("%s\t%s" % (k, v))


if __name__ == "__main__":
    main()
