#include <assert.h>
#include <stdio.h>
#include "../rt/boot_power.h"
int main(void)
{
    unsigned mode, gates;
    for (mode = 0; mode < 256; ++mode) {
        for (gates = 0; gates < 8; ++gates) {
            volatile unsigned standby = 0x12340000u | mode;
            volatile unsigned modules = 0xA50F1201u |
                ((gates & 1) ? 0x8000u : 0) |
                ((gates & 2) ? 0x40u : 0) |
                ((gates & 4) ? 0x20u : 0);
            unsigned old_standby = standby, old_modules = modules;
            dbt_boot_power(&standby, &modules);
            assert(standby == (old_standby & ~0xB0u));
            assert(modules == (old_modules & ~0x8060u));
        }
    }
    puts("boot power: 2048 inherited states; unrelated register bits preserved PASS");
    return 0;
}
