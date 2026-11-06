#!/usr/bin/env python3
"""Differentially check actual recovery code with eager/lazy metadata reads."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'dbt/rt/rt.c').read_text()
start = s.index('static void apply_resume_state(')
body = s[start:s.index('\n/* Debug trace', start)]
s = (root/'dbt/src/translate.c').read_text()
pending = s[s.index('int dbt_block_pending_spills('):s.index('\nint dbt_block_entry(')]
program = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define dbt_get_meta original_get_meta
#include "translate.h"
#undef dbt_get_meta
static unsigned reads;
static void dbt_get_meta(const dbt_block *b,int i,dbt_imeta *m) {++reads;original_get_meta(b,i,m);}
typedef struct { sf_u32 r[16], slots[16], ssr, mach, macl, pr, gbr; } dbt_gctx;
static struct { unsigned resumes,resumes_committed; } dbt_stat;
static sf_u32 dbt_shadow_vbr;
static void dbt_halt(const char *message) {fputs(message,stderr);abort();}
static sf_u32 op_swapb(sf_u32 x) {return (x&0xffff0000u)|((x&255)<<8)|((x>>8)&255);}
static sf_u32 op_swapw(sf_u32 x) {return (x<<16)|(x>>16);}
static sf_u32 op_extsw(sf_u32 x) {return (x&0x8000)?x|0xffff0000u:x&0xffff;}
''' + pending + '\n' + body.replace('apply_resume_state(', 'reference(') + '\n#define DBT_LAZY_RESUME_META\n' + body.replace('apply_resume_state(', 'candidate(') + r'''
int main(void) {
 const unsigned fixes[]={FIX_NONE,FIX_SWAP16,FIX_SWAP32,FIX_CR_BASE+CR_MACH,
  FIX_CR_BASE+CR_MACL,FIX_CR_BASE+CR_PR,FIX_CR_BASE+CR_GBR,FIX_CR_BASE+CR_VBR};
 unsigned cases=0;
 for(unsigned packed=0;packed<2;++packed)
 for(unsigned fix=0;fix<sizeof(fixes)/sizeof(fixes[0]);++fix)
 for(unsigned committed=0;committed<2;++committed)
 for(unsigned k=0;k<5;++k)
 for(unsigned t=0;t<2;++t) {
  dbt_block b={0};dbt_imeta m={0};dbt_pmeta pm={0};dbt_spill sp[3]={{0}};
  m.host_off=10;m.commit_rel=4;m.fix_kind=fixes[fix];m.fix_reg=3;
  pm.host_off=m.host_off;pm.commit_rel=m.commit_rel;pm.fix_kind=m.fix_kind;pm.kind_reg=3<<4;
  b.meta=packed?(dbt_imeta*)&pm:&m;b.packed_meta=packed;b.nmeta=1;b.spills=sp;b.nspills=3;
  sp[0].gpr=3;sp[0].slot=0;sp[0].start=2;sp[0].end=24;
  sp[1].gpr=DBT_SPILL_T;sp[1].slot=1;sp[1].start=4;sp[1].end=18;
  sp[2].gpr=5;sp[2].slot=2;sp[2].start=18;sp[2].end=24;
  dbt_gctx a,c;memset(&a,0,sizeof a);
  for(unsigned i=0;i<16;++i){a.r[i]=0x12348000+i;a.slots[i]=0xabcde000+i;}
  a.ssr=0x40001000|t;a.slots[1]=t^1;c=a;
  reads=0;dbt_shadow_vbr=99;dbt_stat.resumes=dbt_stat.resumes_committed=0;
  reference(&b,0,16+2*k,committed,&a);assert(reads==1);
  unsigned vr=dbt_shadow_vbr,r=dbt_stat.resumes,rc=dbt_stat.resumes_committed;
  reads=0;dbt_shadow_vbr=99;dbt_stat.resumes=dbt_stat.resumes_committed=0;
  candidate(&b,0,16+2*k,committed,&c);
  assert(reads==committed && !memcmp(&a,&c,sizeof a) && vr==dbt_shadow_vbr);
  assert(r==dbt_stat.resumes && rc==dbt_stat.resumes_committed);
  ++cases;
 }
 printf("PASS: %u eager/lazy recovery comparisons; packed/unpacked, commit/tail, spill/T and control registers\n",cases);
}
'''
with tempfile.TemporaryDirectory() as td:
    c=Path(td)/'test.c';exe=Path(td)/'test';c.write_text(program)
    subprocess.run(['cc','-O2','-fsanitize=address,undefined','-I'+str(root/'dbt/src'),
                    '-I'+str(root/'fpuemu/src'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
