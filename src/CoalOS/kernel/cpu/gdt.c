#include <kernel/cpu/gdt.h>
#include <kernel/cpu/tss.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/core/types.h>

extern void gdt_flush(uint32_t gdt_ptr);
extern void tss_flush(uint32_t tss_selector);

#define GDT_ENTRIES 7

static struct gdt_entry gdt_entries[GDT_ENTRIES];
static struct gdt_ptr   gp;

static void gdt_set_gate(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[idx].base_low    = (uint16_t)(base & 0xFFFF);
    gdt_entries[idx].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt_entries[idx].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt_entries[idx].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt_entries[idx].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt_entries[idx].access      = access;
}

void gdt_init(void) {
    gp.limit = (uint16_t)(sizeof(gdt_entries) - 1);
    gp.base  = (uint32_t)&gdt_entries[0];

    // Flat 4 GiB segments: access 0x9A/0x92 = ring 0 code/data, 0xFA/0xF2 = ring 3.
    // Granularity 0xC0 = 4 KiB pages, 32-bit.
    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xC0);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xC0);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xC0);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xC0);
    // 32-bit available TSS, byte granularity.
    gdt_set_gate(5, (uint32_t)&tss, sizeof(struct tss_entry) - 1, 0x89, 0x00);
    // TLS segment: ring 3 data, base set per process by set_thread_area.
    gdt_set_gate(6, 0, 0, 0xF2, 0xC0);

    gdt_flush((uint32_t)&gp);
    tss_init();
    tss_flush(GDT_TSS_SELECTOR);
}

void gdt_set_tls_base(uint32_t base, uint32_t limit_pages) {
    gdt_set_gate(6, base, limit_pages, 0xF2, 0xC0);
}
