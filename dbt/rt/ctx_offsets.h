/* Byte offsets into dbt_gctx, shared by rt.h (checked with static asserts) and stubs.S */
#define CTX_SR      0
#define CTX_PR      4
#define CTX_GBR     8
#define CTX_MACH    12
#define CTX_MACL    16
#define CTX_R0      20          /* r[16] at 20..80 */
#define CTX_REND    84          /* one past r[15]; slots start here */
#define CTX_SLOTS   84          /* A0 X0 X1 Y0 Y1 */
#define CTX_HOSTSPC 104
#define CTX_SSR     108
#define CTX_EXPEVT  112
#define CTX_TRA     116
#define CTX_TEA     120
#define CTX_INTEVT  124
#define CTX_VECOFF  128
#define CTX_JUMPTO  132
#define CTX_NEWSPC  136
#define CTX_NEWSSR  140
#define CTX_B1      144         /* b1[8]: the other register bank (bank 1 when the guest ran in bank 0) */
#define CTX_B1END   176
#define CTX_SWSPC   176         /* bank-switching ldc sr: guest SPC, SSR and the host target for the trampoline */
#define CTX_SWSSR   180
#define CTX_SWTARGET 184
#define CTX_SIZE    188
