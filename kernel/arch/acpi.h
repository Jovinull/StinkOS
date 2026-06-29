/* Static ACPI table parsing. We never touch AML (DSDT bytecode); the
 * goal is just to read the firmware-fixed tables -- RSDP, RSDT/XSDT,
 * then on demand FADT (power off), MADT (CPU + IOAPIC enumeration).
 *
 * Refs:
 *   - ACPI 6.5 spec, §5.2.5 (RSDP), §5.2.7 (RSDT), §5.2.8 (XSDT),
 *     §5.2.6 (SDT header + checksum).
 *   - Intel SDM Vol 3A §10.4 (multiprocessor init) -- explains why
 *     MADT is the table you parse to find APIC.
 */
#ifndef KERNEL_ARCH_ACPI_H
#define KERNEL_ARCH_ACPI_H

/* Boot-time RSDP scan + table enumeration. Called once from kmain after
 * paging is up (we deref low-phys via the kernel direct map). Idempotent;
 * a second call is a no-op. */
void acpi_init(void);

/* Nonzero if acpi_init found a valid RSDP and an iterable root table
 * (RSDT for ACPI 1.0, XSDT for 2.0+). When zero, every acpi_find_table()
 * call returns NULL and the kernel falls back to legacy paths. */
int acpi_available(void);

/* Look up a table by its 4-character ASCII signature ("FACP", "APIC",
 * "MCFG", ...). Returns a kernel-virt pointer (KVA via the direct map)
 * or NULL when the table isn't present or its checksum is bad. Bytes
 * past the SDT header layout vary per table; callers cast to the right
 * struct after inspecting the header length. */
const void *acpi_find_table(const char *sig4);

/* Attempt an ACPI S5 (soft-off) shutdown via the FADT-described
 * PM1a_CNT_BLK port: write (SLP_TYPa << 10) | SLP_EN. SLP_TYPa lives in
 * the DSDT's \_S5_ AML package which we don't evaluate; hard-code 5
 * (the value used by essentially every PC firmware -- QEMU, VMware,
 * commodity desktop boards). Returns 0 if the write fired (CPU is
 * expected to lose power before we read this return value); -1 if no
 * FADT was found or PM1a_CNT_BLK is unset, in which case the caller
 * should fall back to the legacy port-0x604 / 0xB004 / VBox path. */
int acpi_shutdown(void);

/* MADT-derived topology. All return 0 when MADT is absent or did not
 * advertise the relevant entry. No SMP wiring today; these are the
 * inputs the future SMP bring-up will need (LAPIC base for IPI/timer,
 * IOAPIC base for real IRQ routing, CPU count for per-core stacks). */
unsigned int acpi_cpu_count(void);
unsigned int acpi_lapic_base(void);
unsigned int acpi_ioapic_base(void);

#endif
