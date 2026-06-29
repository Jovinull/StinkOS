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

#endif
