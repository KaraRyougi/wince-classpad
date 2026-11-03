#!/usr/bin/env python3
"""Make casio-emu accept the SH-4 operand-cache address array (0xF4000000)
and data array (0xF5000000). The emulator models no operand cache (memory is
coherent), so writes are dropped and reads return 0; without this an
address-array walk - the only way to write back and invalidate a 4-way cache
as a whole, which the ClassPad OAL's OEMCacheRangeFlush does for
CACHE_SYNC_DISCARD/WRITEBACK with no range - stops the run with
"Invalid memory write at f4000000". Idempotent; rerun after updating the
emulator, then cmake --build build.

    python3 dbt/emu-patch/add_oc_arrays.py ~/Dev/casio-emu
"""
import sys, os
root = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/Dev/casio-emu')
p = os.path.join(root, 'src/memory/memory.c')
s = open(p).read()
MARK = 'operand-cache arrays (wince-classpad)'
if MARK in s:
    print('already patched'); sys.exit(0)
w_anchor = '  if (sh4IcacheArrayWrite(address, size, value)) {\n'
r_anchor = '    if (sh4IcacheArrayRead(address, size, &cacheValue)) return cacheValue;\n'
assert w_anchor in s and r_anchor in s, 'anchors not found: memory.c changed'
s = s.replace(w_anchor,
    '  /* operand-cache arrays (wince-classpad): no operand cache is modelled, the\n'
    '   * address array (F4) and data array (F5) accept writes and read as 0. */\n'
    '  if ((address & 0xfe000000u) == 0xf4000000u) return;\n' + w_anchor, 1)
s = s.replace(r_anchor,
    r_anchor + '    if ((address & 0xfe000000u) == 0xf4000000u) return 0; /* operand-cache arrays (wince-classpad) */\n', 1)
open(p, 'w').write(s)
print('patched', p)
