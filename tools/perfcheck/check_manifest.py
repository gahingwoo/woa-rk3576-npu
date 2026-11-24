#!/usr/bin/env python3
# check_manifest.py - rknpu.man against rknpu_perf.h, on the host
#
# What ctrpp and lodctr would only tell on Windows, or not at all: every
# counter names a structure and a field that exist, the field's size is the
# size its counter type reads, ids and names are unique, every average has a
# base of the right type, and the provider GUID is the one
# tools/rknpu-perfcounters.ps1 unregisters.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

import re
import sys
import xml.etree.ElementTree as ET

MAN, HDR, PS1 = sys.argv[1:4]
NS = {'c': 'http://schemas.microsoft.com/win/2005/12/counters'}

SIZE = {'unsigned long long': 8, 'unsigned int': 4}
TYPE_SIZE = {
    'perf_100nsec_timer': 8, 'perf_counter_bulk_count': 8,
    'perf_average_bulk': 8, 'perf_counter_large_rawcount': 8,
    'perf_average_base': 4, 'perf_counter_rawcount': 4,
}
NEEDS_BASE = {'perf_average_bulk': 'perf_average_base', 'perf_average_timer': 'perf_average_base'}

err = []

# typedef struct _X { type field; ... } X;
structs = {}
src = re.sub(r'/\*.*?\*/', '', open(HDR).read(), flags=re.S)
for body, name in re.findall(r'typedef\s+struct\s+\w+\s*\{(.*?)\}\s*(\w+)\s*;', src, re.S):
    fields = {}
    for t, f in re.findall(r'(unsigned long long|unsigned int)\s+(\w+)\s*;', body):
        fields[f] = SIZE[t]
    structs[name] = fields

root = ET.parse(MAN).getroot()
prov = root.find('.//c:provider', NS)
if prov is None:
    sys.exit('no provider')
if prov.get('providerType') != 'kernelMode':
    err.append('providerType is not kernelMode')
if prov.get('applicationIdentity') != 'rknpu.sys':
    err.append('applicationIdentity is not rknpu.sys')
guid = prov.get('providerGuid', '').lower()
if guid not in open(PS1).read().lower():
    err.append(f'provider GUID {guid} is not the one in {PS1}')

sets = {}
for cs in prov.findall('c:counterSet', NS):
    sname = cs.get('name')
    st = {s.get('name'): s.get('type') for s in cs.findall('c:structs/c:struct', NS)}
    ids, names = {}, set()
    for c in cs.findall('c:counter', NS):
        cid, ctype, cname = int(c.get('id')), c.get('type'), c.get('name')
        if cid in ids:
            err.append(f'{sname}: id {cid} twice')
        ids[cid] = c
        hidden = any(a.get('name') == 'noDisplay' for a in c.findall('c:counterAttributes/c:counterAttribute', NS))
        if not hidden:
            if not cname or not c.get('description'):
                err.append(f'{sname}: counter {cid} has no name or description')
            if cname in names:
                err.append(f'{sname}: name {cname!r} twice')
            names.add(cname)
        stype = st.get(c.get('struct'))
        if stype not in structs:
            err.append(f'{sname}: counter {cid}: struct {c.get("struct")} -> {stype} not in {HDR}')
            continue
        fsize = structs[stype].get(c.get('field'))
        if fsize is None:
            err.append(f'{sname}: counter {cid}: no field {c.get("field")} in {stype}')
        elif TYPE_SIZE.get(ctype) != fsize:
            err.append(f'{sname}: counter {cid} ({ctype}) reads {TYPE_SIZE.get(ctype)} bytes, '
                       f'{stype}.{c.get("field")} has {fsize}')
    for cid, c in ids.items():
        need = NEEDS_BASE.get(c.get('type'))
        if need:
            b = ids.get(int(c.get('baseID', '-1')))
            if b is None or b.get('type') != need:
                err.append(f'{sname}: counter {cid} needs a {need} base')
    sets[sname] = (cs.get('instances'), sorted(names))

for e in err:
    print('ERROR:', e)
for k, (inst, names) in sets.items():
    print(f'{k} ({inst}): ' + ', '.join(names))
sys.exit(1 if err else 0)
