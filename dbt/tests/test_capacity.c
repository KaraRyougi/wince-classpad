/* Rejected large-block attempts must stay inside the supplied code buffer. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "translate.h"
static sf_u32 pattern;
static int fetch(void *unused, sf_u32 pc, sf_u32 *op) {
    (void)unused;
    *op = pattern == 0 ? 0x6122 : pattern == 1 ? 0x2122 :
        pattern == 2 ? 0x8901 : ((pc & 6) ? 0x6122 : 0x8901);
    return 0;
}
int main(void) {
    unsigned cap, attempts = 0, retries = 0;
    dbt_env env;
    memset(&env, 0, sizeof env);
#ifdef DBT_SAME_ASID_PTEH_STORE
    env.current_asid_addr = 0x8CF01008;
#endif
    env.fetch16 = fetch; env.no_borrow = 1;
    env.dev_lo = 0x8c100000; env.dev_hi = 0x8c200000;
    for (pattern = 0; pattern < 4; ++pattern) for (cap = 1; cap <= DBT_BLOCK_WORDS; ++cap) {
        sf_u16 *code = malloc(cap * sizeof *code);
        dbt_imeta meta[DBT_MAX_META]; dbt_spill spills[DBT_MAX_BLOCK_SPILLS];
        dbt_link links[DBT_MAX_LINKS]; dbt_block block;
        sf_u32 fault = 0; int status;
        memset(&block, 0, sizeof block);
        block.meta=meta; block.spills=spills; block.links=links;
        env.max_insns=32;
        do {
            status=dbt_translate_block(&env,0x8c100000,0x8cc80000,code,cap,&block,&fault);
            ++attempts;
            if(status!=DBT_ERR_OVERFLOW || env.max_insns==1) break;
            env.max_insns=(env.max_insns+1)/2; ++retries;
        } while(1);
        assert(status==DBT_OK || status==DBT_ERR_OVERFLOW);
        if(status==DBT_OK) assert(block.host_len<=cap*2);
        free(code);
    }
    assert(retries>0);
    printf("PASS: %u capacity attempts, %u bounded retries\n",attempts,retries);
    return 0;
}
