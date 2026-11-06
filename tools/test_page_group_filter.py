#!/usr/bin/env python3
"""Compare actual grouped and scalar page filters for all aligned 32-bit VPNs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'dbt/rt/rt.c').read_text()

def function(name):
    # Find the definition's start without including the preceding function.
    start = source.rfind('\n', 0, source.index(name)) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

page_hash = function('user_page_bit(')
filter_body = function('may_have_user_code(')
program = '''#include <stdint.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t sf_u32;
static sf_u32 g_user_pages[256];
''' + page_hash + '\n' + filter_body.replace('may_have_user_code', 'scalar_filter', 1)
program += '\n#define DBT_PAGE_GROUP_FILTER 1\n' + filter_body.replace('may_have_user_code', 'group_filter', 1)
program += r'''
int main(void) {
    uint64_t checked=0;
    uint32_t random=0x12345678u;
    for(unsigned pattern=0;pattern<18;pattern++) {
        for(unsigned word=0;word<256;word++) {
            random ^= random<<13; random ^= random>>17; random ^= random<<5;
            g_user_pages[word]=pattern<16 ? 0x11111111u*pattern : random;
        }
        for(unsigned shift=10;shift<=12;shift+=2) {
            for(uint64_t vpn=0;vpn<((uint64_t)1<<32);vpn+=(uint64_t)1<<shift) {
                int expected=scalar_filter((uint32_t)vpn,shift);
                int actual=group_filter((uint32_t)vpn,shift);
                if(actual!=expected) {
                    fprintf(stderr,"mismatch vpn=%08x shift=%u pattern=%u\n",(uint32_t)vpn,shift,pattern);
                    return 1;
                }
                checked++;
            }
        }
        for(unsigned shift=16;shift<=20;shift+=4)
            for(uint64_t vpn=0;vpn<((uint64_t)1<<32);vpn+=(uint64_t)1<<shift) {
                assert(group_filter((uint32_t)vpn,shift)==scalar_filter((uint32_t)vpn,shift));
                checked++;
            }
    }
    printf("PASS: %llu aligned VPN/bitmap comparisons, including wrap boundaries\n",(unsigned long long)checked);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='page-group-filter-') as directory:
    c = Path(directory)/'test.c'
    binary = Path(directory)/'test'
    c.write_text(program)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-fsanitize=undefined','-o',str(binary),str(c)],check=True)
    subprocess.run([str(binary)],check=True)
