/* devmem.c - device-aware translation test (docs/05 "kernel.dll").
 *
 * Phase 1 runs as an ordinary guest: a value is written to TEA (a plain
 * 32-bit CCN register that holds whatever is written) through the
 * pre-swapped accessor, so the hardware register holds exactly that value.
 * Phase 2 asks the translator to translate this whole guest device-aware
 * (as it does for kernel.dll): from then on plain C accesses to P4 must
 * reach the registers unswapped while ordinary memory keeps its
 * little-endian view, T survives the address test, the delay-slot and
 * post-increment/pre-decrement forms work, and PTEH/MMUCR writes are seen
 * by the translator's address-space bookkeeping. */
#include "hyper.h"
#include "mmio.h"

#define TEA   0xFF00000Cu
#define PTEH  0xFF000000u
#define MMUCR 0xFF000010u

static int fails;
static void check(const char *name, int ok)
{
    hc_puts(ok ? "PASS " : "FAIL "); hc_puts(name); hc_putc('\n');
    if (!ok) fails++;
}

extern unsigned dev_slot_load(unsigned base);                 /* bra with mov.l @(12,r1),r0 in the slot */
extern void     dev_slot_store(unsigned base, unsigned v);    /* jsr with mov.l r5,@(12,r4) in the slot */
extern unsigned dev_postinc(unsigned *pbase);                 /* mov.l @r1+,r0 */
extern void     dev_predec(unsigned *pbase, unsigned v);      /* mov.l r5,@-r1 */
extern unsigned dev_t_after_load(unsigned base, unsigned t);  /* sett/clrt; mov.l @r4,r1; movt r0 */
extern unsigned dev_r0idx(unsigned base, unsigned idx);       /* mov.l @(r0,r4),r0 */

struct s { unsigned char b; unsigned short h; unsigned w; };

void guest_main(void)
{
    volatile unsigned *tea = (volatile unsigned *)TEA;
    volatile struct s st;
    unsigned v, inval0, asid0, dev0;

    hc_puts("devmem test: phase 1 (normal translation)\n");
    mmio_w32(TEA, 0x12345678u);
    check("pre-swapped write reads back pre-swapped", mmio_r32(TEA) == 0x12345678u);
    check("a plain read of the register is byte-reversed under normal translation", *tea == 0x78563412u);

    hc_puts("phase 2: device-aware translation of the whole guest\n");
    inval0 = hc_getstat(2); asid0 = hc_getstat(3); dev0 = hc_getstat(6);
    hc_devrange(GUEST_RAM_LO, GUEST_RAM_LO + 0x100000u);      /* takes effect at the next lookup: the call below */

    check("plain read of TEA now returns the hardware value", *tea == 0x12345678u);
    *tea = 0x0BADF00Du;
    check("plain write / plain read round trip", *tea == 0x0BADF00Du);
    check("a pre-swapped accessor is now wrong (as the OAL would be inside kernel.dll)", mmio_r32(TEA) == 0x0DF0AD0Bu);

    st.b = 1; st.h = 0x1234; st.w = 0x89ABCDEFu;
    check("ordinary memory keeps its little-endian view",
          st.w == 0x89ABCDEFu && st.h == 0x1234 && ((volatile unsigned char *)&st.w)[0] == 0xEF);
    v = dev_t_after_load(TEA, 1);
    check("T=1 survives a device load", v == 1);
    v = dev_t_after_load(TEA, 0);
    check("T=0 survives a device load", v == 0);
    v = dev_t_after_load((unsigned)&st.w, 1);
    check("T=1 survives a memory load", v == 1);

    check("device load in a bra delay slot", dev_slot_load(0xFF000000u) == 0x0BADF00Du);
    dev_slot_store(0xFF000000u, 0x600DCAFEu);
    check("device store in a jsr delay slot", *tea == 0x600DCAFEu);
    {
        unsigned base = TEA;
        check("mov.l @Rm+,Rn on a register", dev_postinc(&base) == 0x600DCAFEu && base == TEA + 4);
        base = TEA + 4;
        dev_predec(&base, 0x11112222u);
        check("mov.l Rm,@-Rn on a register", base == TEA && *tea == 0x11112222u);
        check("mov.l @(r0,Rm),Rn on a register", dev_r0idx(0xFF000000u, 0x0C) == 0x11112222u);
    }

    /* address-space bookkeeping sees the kernel's register writes */
    *(volatile unsigned *)PTEH = 5;
    check("PTEH write switches the translator's ASID", hc_getstat(3) == asid0 + 1 && (*(volatile unsigned *)PTEH & 0xFF) == 5);
    *(volatile unsigned *)PTEH = 0;
    {
        unsigned before = hc_getstat(6), mmucr0 = *(volatile unsigned *)MMUCR, after;
        *(volatile unsigned *)MMUCR = mmucr0 | 4;              /* TI: flush the TLB (no user blocks here to drop) */
        after = *(volatile unsigned *)MMUCR;
        check("MMUCR.TI write goes through the device path and TI self-clears",
              (hc_getstat(6) == before + 1 || hc_getstat(6) == before + 3) && (after & 4) == 0 && (after & 1) == (mmucr0 & 1));
    }
    (void)inval0;
    /* Inline reads bypass C; only writes need the address-space hooks. */
    check("device helper accesses were counted", hc_getstat(6) > dev0);
    /* the OAL's whole-cache flush walks the operand-cache address array; casio-emu
     * models no operand cache and accepts the array through dbt/emu-patch/add_oc_arrays.py */
    *(volatile unsigned *)0xF4000000u = 0;
    *(volatile unsigned *)0xF4007FE0u = 0;
    check("operand-cache address array writes are accepted (emulator patch)", *(volatile unsigned *)0xF4000000u == 0);

    hc_stats();
    hc_puts(fails ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    hc_exit(fails);
}
