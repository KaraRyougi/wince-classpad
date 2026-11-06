#!/usr/bin/env python3
"""Check the actual runtime source iterator against a scalar source union."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'dbt/rt/rt.c').read_text()
start = source.index('#define SOURCE_WORDS')
end = source.index('\n#endif\n\nstatic sf_u32 bank_code_bytes', start)
body = source[start:end]
harness = r'''
#include <stdint.h>
#include <stdio.h>
typedef uint32_t sf_u32;
typedef uint8_t sf_u8;
#define MAX_BLOCKS 257
#define CACHE_BANKS 14
#define DBT_INCOMING_FAST_SCAN
'''+body+r'''
int main(void) {
    unsigned round, banks, word, bank, seed=1, comparisons=0;
    for (unsigned v=1; v<65536; ++v)
        if (source_low_bit(v) != (unsigned)__builtin_ctz(v) ||
            source_low_bit(v<<16) != (unsigned)__builtin_ctz(v<<16)) return 1;
    for (round=0; round<3; ++round) {
        for (word=0;word<SOURCE_WORDS;++word) {
            seed=seed*1664525u+1013904223u;
            g_source_ic[word]=round==0?0:round==1?seed:0xffffffffu;
            for(bank=0;bank<CACHE_BANKS;++bank) {
                seed=seed*1664525u+1013904223u;
                g_source_links[bank][word]=round==0?(1u<<((bank+word)&31)):seed;
            }
        }
        for (banks=0;banks<(1u<<CACHE_BANKS);++banks) {
            source_scan scan={0,0,banks,0};
            for(word=0;word<SOURCE_WORDS;++word) {
                unsigned expected=g_source_ic[word];
                for(bank=0;bank<CACHE_BANKS;++bank)
                    if(banks&(1u<<bank)) expected|=g_source_links[bank][word];
                for(unsigned bit=0;bit<32;++bit) if(expected&(1u<<bit)) {
                    if(source_next(&scan)!=(int)(word*32+bit)) return 2;
                    ++comparisons;
                }
            }
            if(source_next(&scan)!=-1) return 3;
        }
    }
    printf("PASS: %u ordered iterator results, all bank masks; low-bit boundaries\n", comparisons);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
    c=Path(td)/'test.c'; exe=Path(td)/'test'; c.write_text(harness)
    subprocess.run(['cc','-O2','-fsanitize=undefined,address',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
