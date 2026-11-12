/* uipreview.c - the loader's screens rendered on the host, for checking
 * hhk/ui.c against docs/design/loader-luna.html without a calculator:
 *     make -C dbt hhk-preview   ->  <build>/ui-<screen>.ppm
 * Built with the host's cc and -DHHK_HOST (hhk/util.h then takes libc's
 * string functions); the data shown is the mockup's example. */
#include <stdio.h>
#include "ui.h"

static u16 fb[GFX_W * GFX_H];

static void save(const char *dir, const char *name)
{
    char path[256];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s/ui-%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", GFX_W, GFX_H);
    for (i = 0; i < GFX_W * GFX_H; i++) {
        u16 c = fb[i];
        unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3 | (c >> 13)), (unsigned char)(((c >> 5) & 63) << 2 | ((c >> 9) & 3)),
                                 (unsigned char)((c & 31) << 3 | ((c >> 2) & 7)) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", path);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    ui_scan s = { { UF_DIFF, 3355443, "" }, { UF_DIFF, 1468006, "" }, 0x80EE0000u, "holds a CE image" };
    ui_scan s2 = { { UF_SAME, 3355443, "" }, { UF_NONE, 0, "" }, 0x80EE0000u, "holds a CE image" };
    ui_job j = { "Flashing CE Core OS", 5,
                 { "Check NK.bin", "Erase 26 flash sectors", "Write 7 records", "Verify NK.bin", "Start Windows CE" },
                 { UI_DONE, UI_DONE, UI_NOW, UI_TODO, UI_TODO } };
    static const char *const todo[] = {
        "Press EXE to return to HollyHock.",
        "Start wince.hh3 again and choose Flash CE Core OS. NK.bin is still in \\fls0.",
        "If it fails again, copy NK.bin over USB once more.",
    };
    gfx_fb = fb;
    ui_menu(&s, UA_CORE, 0);            save(dir, "menu");
    ui_menu(&s2, UA_BOOT, 0);           save(dir, "menu-noext");
    ui_pressed = UI_HIT_START;
    ui_menu(&s, UA_CORE, 0);            save(dir, "menu-pressed");
    ui_pressed = UI_HIT_CLOSE;
    ui_menu(&s, UA_CORE, 0);            save(dir, "menu-close");
    ui_pressed = UI_HIT_NONE;
    ui_flash(&j, 1, "Record 3 of 7 " GFX_MIDDOT " 1.4 MB of 3.2 MB", 430, 610);
    save(dir, "flash");
    ui_boot(7);                         save(dir, "boot");
    ui_error("Flashing stopped", "Erase failed at 80F00000.",
             "Sectors 1 to 9 were erased. Nothing was written after that, so the old image in flash is no longer complete.",
             todo, 3);
    save(dir, "error");
    return 0;
}
