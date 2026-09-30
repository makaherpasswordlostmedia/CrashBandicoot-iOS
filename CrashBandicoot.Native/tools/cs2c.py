#!/usr/bin/env python3
"""cs2c.py - convert RecompOne.Recompiler's generated C# into C for the native runtime.

The recompiler emits a very regular subset of C# (see InstructionEmitter.cs);
patch files (Launcher/Recomp/Patches/*.patch) add a few hand-written blocks in
the same dialect. This converter is therefore a set of anchored rewrites, not a
C# parser. Anything it does not recognise is left alone and the C compiler will
complain loudly, which is what we want.

usage: cs2c.py <dir-with-generated-cs> <out-dir> [--split N]
"""
import os, re, sys, glob

CASTS = [
    (r'\(byte\)', '(uint8_t)'), (r'\(sbyte\)', '(int8_t)'),
    (r'\(ushort\)', '(uint16_t)'), (r'\(short\)', '(int16_t)'),
    (r'\(uint\)', '(uint32_t)'), (r'\(int\)', '(int32_t)'),
    (r'\(ulong\)', '(uint64_t)'), (r'\(long\)', '(int64_t)'),
]
MEM = [
    (r'\bm\.ReadU8\(', 'RD8('), (r'\bm\.ReadU16\(', 'RD16('), (r'\bm\.ReadU32\(', 'RD32('),
    (r'\bm\.WriteU8\(', 'WR8('), (r'\bm\.WriteU16\(', 'WR16('), (r'\bm\.WriteU32\(', 'WR32('),
    (r'\bm\.ReadWordLeft\(', 'RDWL('), (r'\bm\.ReadWordRight\(', 'RDWR('),
    (r'\bm\.WriteWordLeft\(', 'WRWL('), (r'\bm\.WriteWordRight\(', 'WRWR('),
]

SIG_RE = re.compile(r'^(\s*)(?:public\s+)?(?:static\s+)?void\s+(\w+)\(CpuContext c, IMemory m((?:,\s*[^)]*)?)\)\s*(.*)$')


def sdk_name(dotted):
    parts = dotted.split('.')
    if len(parts) >= 2 and parts[-2].startswith('Lib'):
        return parts[-2] + '_' + parts[-1]
    return parts[-1]


def conv_expr(line):
    for a, b in CASTS:
        line = re.sub(a, b, line)
    for a, b in MEM:
        line = re.sub(a, b, line)
    line = re.sub(r'\bc\.(\w+)', r'c->\1', line)
    line = line.replace('int.MinValue', 'INT32_MIN')
    line = re.sub(r'\bvar (_r) = \(int64_t\)', r'int64_t \1 = (int64_t)', line)
    line = re.sub(r'\bvar (_r) = \(uint64_t\)', r'uint64_t \1 = (uint64_t)', line)
    line = re.sub(r'\bvar (\w+) = ', r'int32_t \1 = ', line)
    line = re.sub(r'Dispatcher\.Call\(c, m, ', 'disp_call(c, ', line)
    line = re.sub(r'\bBios\.Syscall\(', 'bios_syscall(', line)
    line = re.sub(r'\bBios\.Break\(', 'bios_break(', line)
    line = re.sub(r'RecompOne\.Runtime\.Gte\.(\w+)\(', r'Gte_\1(', line)
    line = re.sub(r'\(c, m\)', '(c)', line)
    line = re.sub(r'\(c, m, ', '(c, ', line)
    if 'PreHook.Run' in line:
        raise SystemExit('PreHook is not supported by cs2c yet: ' + line.strip())
    return line


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    src, out = sys.argv[1], sys.argv[2]
    split = 250
    if '--split' in sys.argv:
        split = int(sys.argv[sys.argv.index('--split') + 1])
    os.makedirs(out, exist_ok=True)

    entry_txt = ''
    files = []
    for p in sorted(glob.glob(os.path.join(src, '**', '*.cs'), recursive=True)):
        name = os.path.basename(p)
        if name == 'Entry.cs':
            entry_txt = open(p, encoding='utf-8').read(); continue
        if name == 'Stubs.cs':
            continue
        files.append(p)
    if not entry_txt:
        raise SystemExit('Entry.cs not found in ' + src)

    funcs = []          # list of C text chunks (one per function)
    protos = []         # "void name(CpuContext *c...);"
    overlays = {}       # overlay name -> dict
    classes = set()

    for p in files:
        txt = open(p, encoding='utf-8').read()
        # dispatch tables ------------------------------------------------
        for m in re.finditer(r'class (\w+)DispatchTable : IOverlay\s*\{(.*?)\n\}', txt, re.S):
            body = m.group(2)
            nm = re.search(r'Name => "([^"]*)"', body).group(1)
            lba = int(re.search(r'LbaStart => (-?\d+)', body).group(1))
            base = re.search(r'Base => (0x[0-9A-Fa-f]+)u', body).group(1)
            size = re.search(r'Size => (0x[0-9A-Fa-f]+)u', body).group(1)
            ents = re.findall(r'\[(0x[0-9A-Fa-f]+)u\] = (\w+)\.(\w+),', body)
            overlays[nm] = dict(lba=lba, base=base, size=size, ents=ents)
        # function classes ----------------------------------------------
        for m in re.finditer(r'public static partial class (\w+)\s*\{\n(.*?)\n\}\n', txt, re.S):
            classes.add(m.group(1))
            body = m.group(2)
            cur = []
            for line in body.split('\n'):
                if 'MethodImpl' in line:
                    continue
                if not line.strip():
                    continue
                cur.append(line)
            # regroup into functions
            i = 0
            while i < len(cur):
                sm = SIG_RE.match(cur[i])
                if not sm:
                    i += 1; continue
                ind, name, extra, tail = sm.groups()
                params = 'CpuContext *c' + extra
                protos.append('void %s(%s);' % (name, params))
                tail = tail.strip()
                if tail.startswith('=>'):
                    call = tail[2:].strip().rstrip(';')
                    call = re.sub(r'^([\w\.]+)\(', lambda mm: (sdk_name(mm.group(1)) if '.' in mm.group(1) else mm.group(1)) + '(', call)
                    call = conv_expr(call)
                    funcs.append('void %s(%s) { %s; }\n' % (name, params, call))
                    i += 1; continue
                if tail.startswith('{') and tail.rstrip().endswith('}') and len(tail) <= 4:
                    funcs.append('void %s(%s) { (void)c; }\n' % (name, params)); i += 1; continue
                # multi-line body: next line is "{"
                buf = ['void %s(%s)' % (name, params)]
                i += 1
                depth = 0
                started = False
                while i < len(cur):
                    l = cur[i]
                    depth += l.count('{') - l.count('}')
                    buf.append(conv_expr(l))
                    i += 1
                    if l.strip() == '{' and not started:
                        started = True
                    if started and depth == 0:
                        break
                funcs.append('\n'.join(buf) + '\n')

    if len(classes) != 1:
        raise SystemExit('expected exactly one function class (no overlays with duplicate symbols), got %s' % sorted(classes))
    cls = next(iter(classes))
    funcs = [re.sub(r'\b%s\.(\w+)\(' % re.escape(cls), r'\1(', f) for f in funcs]
    print('[cs2c] %d functions, %d overlays' % (len(funcs), len(overlays)))

    # ---- header ---------------------------------------------------------
    with open(os.path.join(out, 'recomp_decl.h'), 'w') as f:
        f.write('/* generated by cs2c.py - do not edit */\n#ifndef RECOMP_DECL_H\n#define RECOMP_DECL_H\n#include "psx.h"\n')
        f.write('#pragma clang diagnostic ignored "-Wunused-label"\n#pragma clang diagnostic ignored "-Wunused-parameter"\n')
        f.write('\n'.join(sorted(set(protos))) + '\n#endif\n')

    # ---- function files ---------------------------------------------------
    nfiles = 0
    for k in range(0, len(funcs), split):
        with open(os.path.join(out, 'recomp_%03d.c' % nfiles), 'w') as f:
            f.write('/* generated by cs2c.py - do not edit */\n#include "recomp_decl.h"\n#include <stdint.h>\n\n')
            f.write('\n'.join(funcs[k:k + split]))
        nfiles += 1

    # ---- tables + entry ------------------------------------------------
    ent = entry_txt
    boot = re.search(r'LoadToMemory\("([^"]+)", (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+), (\d+)\)', ent)
    gp = re.search(r'c\.GP = (0x[0-9A-Fa-f]+)u', ent).group(1)
    sp = re.search(r'c\.SP = (0x[0-9A-Fa-f]+)u', ent).group(1)
    main_call = re.search(r'^\s*([\w\.]+)\(c, m\);\s*$', ent, re.M)
    disp_pc = re.search(r'Dispatcher\.Call\(c, m, (0x[0-9A-Fa-f]+)u\)', ent)
    with open(os.path.join(out, 'tables.c'), 'w') as f:
        f.write('/* generated by cs2c.py - do not edit */\n#include "recomp_decl.h"\n\n')
        regs = []
        for nm, o in overlays.items():
            ident = re.sub(r'\W', '_', nm)
            f.write('static const FuncEntry funcs_%s[] = {\n' % ident)
            for addr, cls, fn in o['ents']:
                f.write('    {%su, %s},\n' % (addr, fn))
            f.write('};\n')
            f.write('static const OverlayDesc ov_%s = { "%s", %d, %s, %s, funcs_%s, %d };\n\n' %
                    (ident, nm, o['lba'], o['base'], o['size'], ident, len(o['ents'])))
            regs.append('    disp_register(&ov_%s);' % ident)
        f.write('void game_entry_run(void)\n{\n')
        f.write('\n'.join(regs) + '\n')
        f.write('    cd_load_to_memory("%s", %su, %s, %s);\n' % (boot.group(1), boot.group(2), boot.group(3), boot.group(4)))
        f.write('    disp_load("main");\n')
        f.write('    memset(&g_cpu, 0, sizeof g_cpu);\n')
        f.write('    g_cpu.GP = %su; g_cpu.SP = %su; g_cpu.FP = g_cpu.SP; g_cpu.RA = 0u;\n' % (gp, sp))
        if main_call and 'Dispatcher' not in main_call.group(1):
            call = main_call.group(1).split('.')[-1]
            f.write('    %s(&g_cpu);\n' % call)
        else:
            f.write('    disp_call(&g_cpu, %su);\n' % disp_pc.group(1))
        f.write('}\n')
    print('[cs2c] wrote %d function files' % nfiles)
    return 0


if __name__ == '__main__':
    sys.exit(main())
