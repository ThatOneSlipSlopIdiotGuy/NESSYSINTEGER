"""Tiny two-pass 6502 assembler (official opcodes) used to build NESSYSINTEGER's self-test ROMs.
Syntax: labels 'name:', constants 'NAME = expr', directives .org .byte .word .fill .bank,
expressions with $hex %bin 'c' <lo >hi + - * / & | ( )."""
import re

OPS = {}
def _add(m, **modes):
    for k, v in modes.items(): OPS[(m, k)] = v
for m, base in [('ORA', 0x00), ('AND', 0x20), ('EOR', 0x40), ('ADC', 0x60), ('STA', 0x80), ('LDA', 0xA0), ('CMP', 0xC0), ('SBC', 0xE0)]:
    _add(m, izx=base + 1, zp=base + 5, imm=base + 9, abs=base + 0xD, izy=base + 0x11, zpx=base + 0x15, aby=base + 0x19, abx=base + 0x1D)
del OPS[('STA', 'imm')]
for m, base in [('ASL', 0x00), ('ROL', 0x20), ('LSR', 0x40), ('ROR', 0x60)]:
    _add(m, zp=base + 6, acc=base + 0xA, abs=base + 0xE, zpx=base + 0x16, abx=base + 0x1E)
_add('DEC', zp=0xC6, abs=0xCE, zpx=0xD6, abx=0xDE); _add('INC', zp=0xE6, abs=0xEE, zpx=0xF6, abx=0xFE)
_add('LDX', imm=0xA2, zp=0xA6, abs=0xAE, zpy=0xB6, aby=0xBE); _add('LDY', imm=0xA0, zp=0xA4, abs=0xAC, zpx=0xB4, abx=0xBC)
_add('STX', zp=0x86, abs=0x8E, zpy=0x96); _add('STY', zp=0x84, abs=0x8C, zpx=0x94)
_add('CPX', imm=0xE0, zp=0xE4, abs=0xEC); _add('CPY', imm=0xC0, zp=0xC4, abs=0xCC); _add('BIT', zp=0x24, abs=0x2C)
_add('JMP', abs=0x4C, ind=0x6C); _add('JSR', abs=0x20)
for m, o in dict(BPL=0x10, BMI=0x30, BVC=0x50, BVS=0x70, BCC=0x90, BCS=0xB0, BNE=0xD0, BEQ=0xF0).items(): _add(m, rel=o)
for m, o in dict(BRK=0x00, PHP=0x08, CLC=0x18, PLP=0x28, SEC=0x38, RTI=0x40, PHA=0x48, CLI=0x58, RTS=0x60, PLA=0x68,
                 SEI=0x78, DEY=0x88, TXA=0x8A, TYA=0x98, TXS=0x9A, TAY=0xA8, TAX=0xAA, CLV=0xB8, TSX=0xBA, INY=0xC8,
                 DEX=0xCA, CLD=0xD8, INX=0xE8, NOP=0xEA, SED=0xF8).items(): _add(m, imp=o)
SIZE = dict(imp=1, acc=1, imm=2, zp=2, zpx=2, zpy=2, izx=2, izy=2, rel=2, abs=3, abx=3, aby=3, ind=3)

class AsmError(Exception): pass

def assemble(src, size=0x8000, base=0x8000, fill=0xFF):
    """Multi-pass: labels re-resolve every pass; out-of-range branches are relaxed to
    'inverted branch + JMP' (decisions only ever grow, so this converges)."""
    lines = []
    for ln, raw in enumerate(src.split('\n'), 1):
        line = _strip_comment(raw.replace("\t", " "))
        for stmt in [s.strip() for s in line.split(' : ')]:   # ' : ' separates statements on one line
            if stmt: lines.append((ln, stmt))
    syms, decisions, longb = {}, {}, set()
    base0 = base
    for pas in range(1, 30):
        out = bytearray([fill] * size); pc = base = base0
        changed = False; seen = set()
        for idx, (ln, st) in enumerate(lines):
            try:
                m = re.match(r'^([A-Za-z_][\w]*):\s*(.*)$', st)
                if m:
                    name = m.group(1)
                    if name in seen: raise AsmError('duplicate label ' + name)
                    seen.add(name)
                    if syms.get(name) != pc: changed = True
                    syms[name] = pc
                    st = m.group(2).strip()
                    if not st: continue
                m = re.match(r'^([A-Za-z_]\w*)\s*=\s*(.+)$', st)
                if m:
                    syms[m.group(1)] = _eval(m.group(2), syms, pas, pc)[0]; continue
                if st.startswith('.'):
                    d, _, arg = st.partition(' ')
                    d = d.lower(); arg = arg.strip()
                    if d == '.org': pc = _eval(arg, syms, pas, pc)[0]
                    elif d in ('.byte', '.db'):
                        for item in _split_args(arg):
                            if item.startswith('"'):
                                for ch in item[1:-1]: _put(out, base, pc, ord(ch)); pc += 1
                            else: _put(out, base, pc, _eval(item, syms, pas, pc)[0] & 0xFF); pc += 1
                    elif d in ('.word', '.dw'):
                        for item in _split_args(arg):
                            v = _eval(item, syms, pas, pc)[0] & 0xFFFF
                            _put(out, base, pc, v & 0xFF); _put(out, base, pc + 1, v >> 8); pc += 2
                    elif d == '.fill':
                        a = _split_args(arg); cnt = _eval(a[0], syms, pas, pc)[0]
                        val = _eval(a[1], syms, pas, pc)[0] if len(a) > 1 else 0
                        for _ in range(cnt): _put(out, base, pc, val); pc += 1
                    else: raise AsmError('unknown directive ' + d)
                    continue
                mn, _, opnd = st.partition(' ')
                mn = mn.upper(); opnd = opnd.strip()
                mode, expr = _mode(mn, opnd)
                val, known = (0, True) if expr is None else _eval(expr, syms, pas, pc)
                if mode in ('zp?', 'zpx?', 'zpy?'):
                    if idx not in decisions:
                        small = known and 0 <= val < 256 and (mn, mode[:-1]) in OPS
                        decisions[idx] = mode[:-1] if small else {'zp?': 'abs', 'zpx?': 'abx', 'zpy?': 'aby'}[mode]
                    mode = decisions[idx]
                if (mn, mode) not in OPS: raise AsmError('bad addressing mode %s %s' % (mn, opnd))
                op = OPS[(mn, mode)]
                if mode == 'rel':
                    off = val - (pc + 2)
                    if idx not in longb and known and not -128 <= off <= 127:
                        longb.add(idx); changed = True
                    if idx in longb:   # inverted branch over a JMP
                        _put(out, base, pc, op ^ 0x20); _put(out, base, pc + 1, 3)
                        _put(out, base, pc + 2, 0x4C); _put(out, base, pc + 3, val & 0xFF); _put(out, base, pc + 4, (val >> 8) & 0xFF)
                        pc += 5; continue
                    _put(out, base, pc, op); _put(out, base, pc + 1, off & 0xFF); pc += 2; continue
                n = SIZE[mode]
                _put(out, base, pc, op)
                if n == 2: _put(out, base, pc + 1, val & 0xFF)
                elif n == 3: _put(out, base, pc + 1, val & 0xFF); _put(out, base, pc + 2, (val >> 8) & 0xFF)
                pc += n
            except AsmError as e:
                raise AsmError('line %d: %s: %s' % (ln, st, e))
        if pas > 1 and not changed:
            return bytes(out), syms
    raise AsmError('did not converge')

def _strip_comment(s):
    q = False; r = ''
    for ch in s:
        if ch == '"': q = not q
        if ch == ';' and not q: break
        r += ch
    return r

def _put(out, base, pc, v):
    i = pc - base
    if 0 <= i < len(out): out[i] = v & 0xFF
    else: raise AsmError('address $%04X outside bank' % pc)

def _split_args(s):
    parts, cur, q = [], '', False
    for ch in s:
        if ch == '"': q = not q
        if ch == ',' and not q: parts.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): parts.append(cur.strip())
    return parts

def _mode(mn, o):
    if o == '': return ('acc' if (mn, 'acc') in OPS else 'imp'), None
    if o.upper() == 'A' and (mn, 'acc') in OPS: return 'acc', None
    if o.startswith('#'): return 'imm', o[1:]
    m = re.match(r'^\((.+),\s*[Xx]\)$', o)
    if m: return 'izx', m.group(1)
    m = re.match(r'^\((.+)\),\s*[Yy]$', o)
    if m: return 'izy', m.group(1)
    if mn == 'JMP' and o.startswith('(') and o.endswith(')'): return 'ind', o[1:-1]
    m = re.match(r'^(.+),\s*([XxYy])$', o)
    if m: return ('zpx?' if m.group(2) in 'Xx' else 'zpy?'), m.group(1)
    if (mn, 'rel') in OPS: return 'rel', o
    if mn in ('JMP', 'JSR'): return 'abs', o
    return 'zp?', o

def _eval(e, syms, pas, pc):
    known = [True]
    def sym(m):
        name = m.group(0)
        if name in syms: return str(syms[name])
        if pas > 2: raise AsmError('undefined symbol ' + name)
        known[0] = False; return '4660'
    s = e.strip()
    s = re.sub(r"'(.)'", lambda m: str(ord(m.group(1))), s)
    s = re.sub(r'\$([0-9A-Fa-f]+)', lambda m: str(int(m.group(1), 16)), s)
    s = re.sub(r'%([01]+)', lambda m: str(int(m.group(1), 2)), s)
    s = re.sub(r'\*', str(pc), s) if s == '*' else s
    s = re.sub(r'[A-Za-z_]\w*', sym, s)
    lo = hi = False
    if s.startswith('<'): lo, s = True, s[1:]
    elif s.startswith('>'): hi, s = True, s[1:]
    try: v = int(eval(s.replace('/', '//'), {'__builtins__': {}}))
    except Exception: raise AsmError('bad expression ' + e)
    if lo: v &= 0xFF
    if hi: v = (v >> 8) & 0xFF
    return v, known[0]

def ines(prg, chr_=b'', mapper=0, mirror_v=False, battery=False, chr_ram=False):
    h = bytearray(b'NES\x1a') + bytes([len(prg) // 16384, len(chr_) // 8192,
        ((mapper & 15) << 4) | (1 if mirror_v else 0) | (2 if battery else 0), mapper & 0xF0]) + bytes(8)
    return bytes(h) + prg + chr_
