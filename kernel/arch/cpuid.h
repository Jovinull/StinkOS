/* CPUID-derived feature detection. Wraps the raw CPUID asm and exposes
 * one inline-able predicate per feature the kernel actually depends on.
 *
 * Refs:
 *   - Intel SDM Vol 2A "CPUID -- CPU Identification" + Vol 3A Table
 *     3-8 (basic feature flags) and §3.1.2 (extended function 0x80000001).
 *   - serenity Kernel/Arch/x86_64/CPUID.cpp -- same feature-bit table
 *     read pattern, just wrapped in C++. */
#ifndef KERNEL_ARCH_CPUID_H
#define KERNEL_ARCH_CPUID_H

/* Required for v0.5 PAE+W^X work. PAE lives in EAX=1 EDX bit 6.
 * NX (also called XD by Intel) lives in EAX=0x80000001 EDX bit 20.
 * Both predicates are safe to call during early boot (no MMU needed). */
int cpuid_has_pae(void);
int cpuid_has_nx(void);

/* Logs a one-line serial summary of the features the kernel cares about
 * so a boot trace shows whether PAE / NX support is present on this CPU.
 * Call once from kmain after serial is up. */
void cpuid_log_features(void);

#endif
