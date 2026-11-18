#!/usr/bin/env python3
"""Generate cpp/script/pc_addr_map.inc from plugin-sdk-sa (https://github.com/ThirtyFoxMCx00E1/plugin-sdk-sa).

Every PC GTA:SA 1.0 US address that plugin-sdk names (variables `T& Class::name = *(T*)0xADDR;`, arrays
`T* Class::name = (T*)0xADDR;` and functions `// Converted from <cc> <ret> Class::Name(params) 0xADDR`) becomes
an entry {pc_address, kind, element_size, param_kinds, "Class::name"}. At runtime MonetLoader resolves the name
against libGTASA.so's exported symbols (demangled), so PC scripts that read/write/call those PC addresses hit the
real mobile object instead of crashing.  Usage: gen_pc_addr_map.py <plugin_sa/game_sa> <out.inc>
"""
import os, re, sys

SRC, OUT = sys.argv[1], sys.argv[2]
PRIM = {
    'bool': 1, 'char': 1, 'unsigned char': 1, 'signed char': 1, 'uchar': 1, 'int8_t': 1, 'uint8_t': 1, 'BYTE': 1,
    'short': 2, 'unsigned short': 2, 'ushort': 2, 'int16_t': 2, 'uint16_t': 2, 'WORD': 2, 'wchar_t': 2,
    'int': 4, 'unsigned int': 4, 'uint': 4, 'int32_t': 4, 'uint32_t': 4, 'DWORD': 4, 'long': 4, 'unsigned long': 4,
    'float': 4, 'unsigned': 4, 'eModelID': 4, 'RwBool': 4, 'RwInt32': 4, 'RwUInt32': 4, 'RwReal': 4,
    'double': 8, 'int64_t': 8, 'uint64_t': 8, 'long long': 8, 'unsigned long long': 8,
}
# element kinds: 0 = primitive (same size on both ABIs), 1 = pointer (4 on PC, sizeof(void*) on mobile), 2 = struct
var_ref = re.compile(r'^\s*(?:const\s+)?([\w:<>,\s]+?)\s*(\*+)?\s*&\s*([\w:]*\w+)\s*=\s*\*\s*\(.*?\)\s*\(?(0x[0-9A-Fa-f]+)')
var_ptr = re.compile(r'^\s*(?:const\s+)?([\w:<>,\s]+?)\s*(\*+)\s*([\w:]+::\w+)\s*(\[[^\]]*\])*\s*=\s*\(\s*[\w:<>\s]+\*+\s*\)\s*\(?(0x[0-9A-Fa-f]+)')
func = re.compile(r'//\s*Converted from\s+(?:(cdecl|thiscall|stdcall|__cdecl|fastcall)\s+)?(.+?)\s+([\w:~]+::[\w~]+|[\w~]+)\s*\((.*)\)\s*(0x[0-9A-Fa-f]+)\s*$')

def kind_of(t, stars):
    t = re.sub(r'\b(const|volatile|struct|class|enum|static)\b', '', t).strip()
    t = re.sub(r'\s+', ' ', t)
    if stars:
        return 1, 4
    if t in PRIM:
        return 0, PRIM[t]
    if t.startswith('e') and t[1:2].isupper():  # enums
        return 0, 4
    return 2, 0

def param_kinds(params, thiscall):
    p = params.strip()
    out = 'i' if thiscall else ''
    if p in ('', 'void'):
        return out
    depth, cur, parts = 0, '', []
    for ch in p:
        if ch in '<(':
            depth += 1
        elif ch in '>)':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur); cur = ''
        else:
            cur += ch
    parts.append(cur)
    for a in parts:
        a = a.strip()
        if '*' in a or '&' in a:
            out += 'i'
        elif re.match(r'^(const\s+)?float\b', a):
            out += 'f'
        elif re.match(r'^(const\s+)?double\b', a):
            out += 'd'
        elif re.match(r'^(const\s+)?(CVector|CVector2D|RwV3d|CRGBA|CRect|CMatrix|RwRGBA)\b', a):
            out += 's'  # struct by value: callFunction can't pass it portably
        else:
            out += 'i'
    return out

pv_ref = re.compile(r'^\s*(?:PLUGIN_VARIABLE\s+)?(?:const\s+)?([\w:<>,\s]+?)\s*(\*+)?\s*&\s*([\w:]*\w+).*GLOBAL_ADDRESS_BY_VERSION\(\s*(0x[0-9A-Fa-f]+)')
pv_ptr = re.compile(r'^\s*(?:PLUGIN_VARIABLE\s+)?(?:const\s+)?([\w:<>,\s]+?)\s*(\*+)\s*([\w:]*\w+)\s*(\[[^\]]*\])*\s*=.*GLOBAL_ADDRESS_BY_VERSION\(\s*(0x[0-9A-Fa-f]+)')
addrof = re.compile(r'\bg?addrof(?:_o)?\(\s*([\w:~]+::[\w~]+)\s*(?:,[^)]*)?\)\s*=\s*(?:GLOBAL_)?ADDRESS_BY_VERSION\(\s*(0x[0-9A-Fa-f]+)')
dyncall = re.compile(r'plugin::Call(Method)?(AndReturn)?DynGlobal<(.*)>\s*\(\s*g?addrof(?:_o)?\(\s*([\w:~]+::[\w~]+)')
defn = re.compile(r'^(?!\s*(?://|#|return|if|else|for|while))\s*[\w:<>,\s\*&]*?\b([\w]+(?:::[~\w]+)?)\s*\(([^;{}]*)\)\s*(?:const)?\s*\{?\s*$')
callc = re.compile(r'plugin::Call(?:Method)?(?:AndReturn)?<\s*(?:[^,<>]+,\s*)?(0x[0-9A-Fa-f]{5,6})\b|\)\)\s*(0x[0-9A-Fa-f]{5,6})\)\s*\(')

def split_targs(s):
    depth, cur, parts = 0, '', []
    for ch in s:
        if ch == '<': depth += 1
        elif ch == '>': depth -= 1
        if ch == ',' and depth == 0: parts.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): parts.append(cur.strip())
    return parts

entries = {}
pending = {}   # name -> pc address (from addrof), params filled by the DynGlobal call
sigs = {}
for root, _, files in os.walk(SRC):
    for fn in sorted(files):
        if not fn.endswith('.cpp'):
            continue
        last_def = None
        for line in open(os.path.join(root, fn), encoding='utf-8', errors='replace'):
            m = pv_ref.match(line)
            if m:
                k, sz = kind_of(m.group(1), m.group(2))
                entries.setdefault(int(m.group(4), 16), ('V', k, sz, '', m.group(3)))
                continue
            m = pv_ptr.match(line)
            if m:
                k, sz = kind_of(m.group(1), m.group(2)[1:])
                entries.setdefault(int(m.group(5), 16), ('A', k, sz, '', m.group(3)))
                continue
            m = addrof.search(line)
            if m:
                pending.setdefault(m.group(1), int(m.group(2), 16))
                continue
            m = dyncall.search(line)
            if m:
                targs = split_targs(m.group(3))
                if m.group(2): targs = targs[1:]
                sigs.setdefault(m.group(4), param_kinds(', '.join(targs), False))
                continue
            m = defn.match(line)
            if m and 'Converted from' not in line:
                last_def = (m.group(1), m.group(2))
            m = callc.search(line)
            if m and last_def and '//' not in line.split('plugin::')[0]:
                a = int(m.group(1) or m.group(2), 16)
                nm, prm = last_def
                if a not in entries:
                    this = '::' in nm and 'Method' in line
                    entries[a] = ('F', 0, 0, param_kinds(prm, this), nm)
                continue
            m = var_ref.match(line)
            if m:
                k, sz = kind_of(m.group(1), m.group(2))
                entries.setdefault(int(m.group(4), 16), ('V', k, sz, '', m.group(3)))
                continue
            m = var_ptr.match(line)
            if m:
                stars = m.group(2)[1:]  # one '*' is the array decay
                k, sz = kind_of(m.group(1), stars)
                entries.setdefault(int(m.group(5), 16), ('A', k, sz, '', m.group(3)))
                continue
            m = func.search(line)
            if m:
                name = m.group(3)
                if '::' not in name:
                    continue
                cc = (m.group(1) or '').replace('__', '')
                this = cc == 'thiscall' and 'operator new' not in line and 'operator delete' not in line
                entries.setdefault(int(m.group(5), 16), ('F', 0, 0, param_kinds(m.group(4), this), name))

for nm, a in pending.items():
    entries.setdefault(a, ('F', 0, 0, sigs.get(nm, '?'), nm))

with open(OUT, 'w') as f:
    f.write('// Generated by tools/gen_pc_addr_map.py from plugin-sdk-sa (PC GTA:SA 1.0 US). Do not edit.\n')
    f.write('// {pc_address, type (V=variable, A=array, F=function), elem kind (0 prim, 1 ptr, 2 struct), elem size, params, name}\n')
    for a in sorted(entries):
        t, k, sz, pk, name = entries[a]
        f.write('{0x%06X, \'%s\', %d, %d, "%s", "%s"},\n' % (a, t, k, sz, pk, name))
print(len(entries), 'entries;', sum(1 for e in entries.values() if e[0] == 'F'), 'functions')
