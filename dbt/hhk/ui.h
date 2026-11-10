/* ui.h - the loader's screens in the Windows XP Luna style (hhk/ui.c, after
 * docs/design/loader-luna.html): the action menu, flashing progress, the
 * start screen and the error screen. Each call draws a whole screen (or,
 * for ui_flash, the parts that change) into gfx_fb; the caller shows it. */
#ifndef HHK_UI_H
#define HHK_UI_H
#include "gfx.h"

/* what the loader found for one image file */
enum { UF_NONE, UF_BAD, UF_SAME, UF_DIFF, UF_NOREGION };
typedef struct {
    int state;
    u32 size;                       /* file bytes */
    char why[48];                   /* UF_BAD: the flasher's reason */
} ui_file;

/* the four actions, in menu order */
enum { UA_CORE, UA_APPS, UA_BOTH, UA_BOOT, UA_COUNT };

typedef struct {
    ui_file nk, ext;
    u32 nk_base;                    /* NK window (P1) */
    const char *nk_window;          /* "holds a CE image", ... */
} ui_scan;

int  ui_action_enabled(const ui_scan *s, int a);
void ui_menu(const ui_scan *s, int sel, int checking);

/* flashing: a list of steps, one of them running */
#define UI_MAX_STEPS 12
typedef struct {
    const char *title;
    int n;
    char label[UI_MAX_STEPS][40];
    int state[UI_MAX_STEPS];        /* UI_TODO .. UI_FAIL */
} ui_job;
enum { UI_TODO, UI_NOW, UI_DONE, UI_FAILED };
void ui_flash(const ui_job *j, int full, const char *detail, int step_permille, int all_permille, int secs_left);

void ui_boot(void);
/* the error screen: title in the header, headline next to the icon, detail
 * under it, then up to three numbered things to do */
void ui_error(const char *title, const char *headline, const char *detail, const char *const *todo, int ntodo);
#endif
