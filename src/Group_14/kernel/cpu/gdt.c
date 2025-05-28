#include <kernel/cpu/gdt.h>
#include <kernel/cpu/tss.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/core/types.h>

// Assembly routines to load our GDT and TSS.
extern void gdt_flush(uint32_t gdt_ptr);
extern void tss_flush(uint32_t tss_selector);

// The actual TSS is defined in tss.c as a global variable (tss).
// Here, we only "extern" it if we need to reference it.
extern struct tss_entry tss;

// We define 6 GDT entries: 0: Null, 1: Kernel Code, 2: Kernel Data,
// 3: User Code, 4: User Data, 5: TSS.
static struct gdt_entry gdt_entries[6];
static struct gdt_ptr   gp;

/**
 * gdt_set_gate
 *   Helper function to fill in one GDT entry.
 *
 * @param idx     Which GDT index to fill.
 * @param base    Base address of the segment.
 * @param limit   Segment limit (e.g. 0xFFFFFFFF for 4GB).
 * @param access  Access flags (present bit, ring bits, segment type).
 * @param gran    Granularity (page gran, 32-bit ops, etc.).
 */
static void gdt_set_gate(int idx,
                         uint32_t base,
                         uint32_t limit,
                         uint8_t access,
                         uint8_t gran)
{
    gdt_entries[idx].base_low    = (uint16_t)(base & 0xFFFF);
    gdt_entries[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt_entries[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt_entries[idx].limit_low   = (uint16_t)(limit & 0xFFFF);

    // For the high nibble of the limit plus the granularity bits:
    gdt_entries[idx].granularity =
        (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));

    gdt_entries[idx].access = access;
}

/**
 * gdt_init
 *
 * Sets up our 6–entry GDT (null, kernel code/data, user code/data, TSS),
 * then loads it into the CPU via gdt_flush. Finally, calls tss_init() and
 * tss_flush to load the TSS into TR register.
 */
void gdt_init(void)
{
    // Fill in GDT pointer
    gp.limit = (uint16_t)(sizeof(gdt_entries) - 1);
    gp.base  = (uint32_t)&gdt_entries[0];

    // 0) Null descriptor
    gdt_set_gate(0, 0, 0, 0, 0);

    // 1) Kernel code: base=0, limit=4GB, ring0, code
    //    Access = 0x9A => P=1, DPL=0, S=1 (code/data), type=1010b (executable, readable).
    //    Gran  = 0xCF => G=1 (4k pages), DB=1 (32-bit), limit high=0xF.
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // 2) Kernel data: base=0, limit=4GB, ring0, data
    //    Access = 0x92 => P=1, DPL=0, S=1, type=0010b (writable data).
    //    Gran  = 0xCF => same as code.
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // 3) User code: base=0, limit=4GB, ring3, code
    //    Access = 0xFA => P=1, DPL=3, S=1, type=1010b
    //    Gran  = 0xCF
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // 4) User data: base=0, limit=4GB, ring3, data
    //    Access = 0xF2 => P=1, DPL=3, S=1, type=0010b
    //    Gran  = 0xCF
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    // 5) TSS descriptor
    //    Typically: base -> &tss, limit -> size of TSS - 1
    //    Access = 0x89 => P=1, DPL=0, type=1001b (32-bit TSS (available)).
    //    Gran  = 0x00 => G=0 (bytes), DB=0, L=0, Limit[19:16]=0. Limit fits in low 16 bits.
    uint32_t tss_base  = (uint32_t)&tss;
    uint32_t tss_limit = (sizeof(struct tss_entry) - 1);

    // Use 0x00 for granularity byte as limit is small and DB should be 0
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x00);

    // 1) Load the GDT into GDTR
    gdt_flush((uint32_t)&gp);

    // 2) Initialize TSS structure fields (in tss_init())
    tss_init();

    // 3) Load TSS into TR
    //    TSS descriptor is at index 5 => selector is 5 * 8 = 0x28
    tss_flush(5 * 8);

    terminal_write("GDT and TSS initialized.\n");
}