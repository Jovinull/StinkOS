/* CPUID-derived feature detection. The CPUID instruction takes a leaf
 * number in EAX and returns four 32-bit feature/info words in
 * EAX/EBX/ECX/EDX. We only need EAX=1 (basic feature flags) and
 * EAX=0x80000001 (extended feature flags, where NX lives).
 *
 * Why two leaves: PAE was specified in the Pentium-era flags table
 * (leaf 1 EDX bit 6); the NX bit was added later for x86_64 / PAE
 * NXE and lives in the AMD-introduced extended-function table
 * (leaf 0x80000001 EDX bit 20). Intel SDM Vol 2A "CPUID" Table 3-8
 * (leaf 1) and §3.1.2 (extended leaves) are the canonical reference.
 *
 * Refs:
 *   - Intel SDM Vol 2A "CPUID -- CPU Identification" Table 3-8 + Vol 3A
 *     §4.1 (CR4.PAE) + §4.6 (IA32_EFER.NXE)
 *   - serenity Kernel/Arch/x86_64/CPUID.cpp -- same per-leaf cpuid() call
 *     pattern; their bitfield names map 1:1 to our EDX_* defines. */
#include "cpuid.h"
#include "serial.h"

#define CPUID_EXT_BASE          0x80000000u
#define CPUID_BASIC_FEATURES    0x00000001u
#define CPUID_EXTENDED_FEATURES 0x80000001u

#define CPUID_EDX_PAE_BIT       (1u << 6)
#define CPUID_EXT_EDX_NX_BIT    (1u << 20)
#define CPUID_EDX_PSE_BIT       (1u << 3)
#define CPUID_EDX_PGE_BIT       (1u << 13)
#define CPUID_EDX_APIC_BIT      (1u << 9)

static inline void cpuid_raw(unsigned int leaf,
                             unsigned int *eax, unsigned int *ebx,
                             unsigned int *ecx, unsigned int *edx)
{
	__asm__ volatile ("cpuid"
	                  : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
	                  : "a"(leaf), "c"(0));
}

/* True if the CPU advertises a given extended-leaf feature. Returns 0
 * (not just "feature absent" but also "this CPU pre-dates extended
 * leaves") when the maximum extended leaf is below the one we need --
 * very old CPUs lack the table entirely. */
static int has_extended_feature(unsigned int leaf, unsigned int edx_mask)
{
	unsigned int max_ext, dummy;
	cpuid_raw(CPUID_EXT_BASE, &max_ext, &dummy, &dummy, &dummy);
	if (max_ext < leaf)
		return 0;
	unsigned int eax, ebx, ecx, edx;
	cpuid_raw(leaf, &eax, &ebx, &ecx, &edx);
	return (edx & edx_mask) != 0;
}

static int has_basic_feature(unsigned int edx_mask)
{
	unsigned int eax, ebx, ecx, edx;
	cpuid_raw(CPUID_BASIC_FEATURES, &eax, &ebx, &ecx, &edx);
	return (edx & edx_mask) != 0;
}

int cpuid_has_pae(void)
{
	return has_basic_feature(CPUID_EDX_PAE_BIT);
}

int cpuid_has_nx(void)
{
	return has_extended_feature(CPUID_EXTENDED_FEATURES, CPUID_EXT_EDX_NX_BIT);
}

static void log_flag(const char *name, int present)
{
	serial_write(name);
	serial_write(present ? "=Y " : "=N ");
}

void cpuid_log_features(void)
{
	serial_write("cpuid: ");
	log_flag("PSE",  has_basic_feature(CPUID_EDX_PSE_BIT));
	log_flag("PAE",  cpuid_has_pae());
	log_flag("PGE",  has_basic_feature(CPUID_EDX_PGE_BIT));
	log_flag("APIC", has_basic_feature(CPUID_EDX_APIC_BIT));
	log_flag("NX",   cpuid_has_nx());
	serial_putc('\n');
}
