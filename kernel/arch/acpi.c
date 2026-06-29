/* ACPI static-table discovery: locate the RSDP via the IA-PC scan, then
 * enumerate either the RSDT (ACPI 1.0, 32-bit table pointers) or the
 * XSDT (ACPI 2.0+, 64-bit pointers). Cache the resulting table list so
 * acpi_find_table() is a simple linear scan over signatures.
 *
 * Refs:
 *   - ACPI 6.5 §5.2.5 "Finding the RSDP on IA-PC Systems": search the
 *     16-byte-aligned ranges [EBDA, EBDA+0x400) then [0xE0000, 0xFFFFF)
 *     for the 8-byte signature "RSD PTR " and validate the descriptor
 *     checksum.
 *   - ACPI 6.5 §5.2.6 "System Description Table Header": the 36-byte
 *     header that precedes every SDT, with a 4-char signature, total
 *     length, revision and a single-byte sum-to-zero checksum over the
 *     entire table.
 *   - ACPI 6.5 §5.2.7 (RSDT) / §5.2.8 (XSDT): the root table is an
 *     SDT whose body is an array of physical pointers to other SDTs.
 *
 * No AML interpreter, no DSDT/SSDT bytecode evaluation -- those are
 * deferred indefinitely (see TODO.md "AML interpreter -- FUTURE").
 */
#include "acpi.h"
#include "memlayout.h"
#include "serial.h"

#define MAX_TABLES        32
#define RSDP_SIG_LEN      8
#define SDT_HDR_SIZE      36
#define BIOS_AREA_LO      0x000E0000u
#define BIOS_AREA_HI      0x00100000u
#define EBDA_PTR_PHYS     0x0000040Eu     /* BDA word: EBDA segment >> 4 */
#define EBDA_SCAN_LEN     0x400u           /* spec says first 1 KiB of EBDA */

struct rsdp_v1 {
	char     signature[8];                 /* "RSD PTR " */
	unsigned char  checksum;
	char     oem_id[6];
	unsigned char  revision;
	unsigned int   rsdt_phys;
} __attribute__((packed));

struct rsdp_v2 {
	struct rsdp_v1 base;
	unsigned int   length;
	unsigned long long xsdt_phys;
	unsigned char  ext_checksum;
	unsigned char  reserved[3];
} __attribute__((packed));

struct sdt_hdr {
	char     signature[4];
	unsigned int   length;
	unsigned char  revision;
	unsigned char  checksum;
	char     oem_id[6];
	char     oem_tableid[8];
	unsigned int   oem_revision;
	unsigned int   creator_id;
	unsigned int   creator_revision;
} __attribute__((packed));

/* FADT layout (ACPI 6.5 §5.2.9 Table 5.9). Only the fields up through
 * PM1a_CNT_BLK matter for S5 shutdown; further fields (PM_TMR_BLK,
 * GPE blocks, generic address structures in revision 3+) are read by
 * later subsystems if ever needed. */
struct fadt {
	struct sdt_hdr h;
	unsigned int   firmware_ctrl;
	unsigned int   dsdt_phys;
	unsigned char  reserved0;
	unsigned char  preferred_pm_profile;
	unsigned short sci_int;
	unsigned int   smi_cmd;
	unsigned char  acpi_enable;
	unsigned char  acpi_disable;
	unsigned char  s4bios_req;
	unsigned char  pstate_cnt;
	unsigned int   pm1a_evt_blk;
	unsigned int   pm1b_evt_blk;
	unsigned int   pm1a_cnt_blk;
	unsigned int   pm1b_cnt_blk;
	unsigned int   pm2_cnt_blk;
	unsigned int   pm_tmr_blk;
} __attribute__((packed));

#define S5_SLP_TYPa  5u                  /* per AML \_S5_ on essentially every PC */
#define S5_SLP_EN    (1u << 13)          /* PM1_CNT bit 13: arm the sleep transition */

/* MADT (signature "APIC") header + entry types (ACPI 6.5 §5.2.12). */
struct madt_hdr {
	struct sdt_hdr h;
	unsigned int   lapic_phys;
	unsigned int   flags;                /* bit 0: PCAT_COMPAT (8259 present) */
} __attribute__((packed));

struct madt_entry_hdr {
	unsigned char type;
	unsigned char length;
} __attribute__((packed));

#define MADT_TYPE_LAPIC   0u
#define MADT_TYPE_IOAPIC  1u

struct madt_lapic {
	struct madt_entry_hdr h;
	unsigned char  acpi_processor_id;
	unsigned char  apic_id;
	unsigned int   flags;                /* bit 0: Enabled */
} __attribute__((packed));

struct madt_ioapic {
	struct madt_entry_hdr h;
	unsigned char  ioapic_id;
	unsigned char  reserved;
	unsigned int   ioapic_phys;
	unsigned int   gsi_base;
} __attribute__((packed));

static int g_available;
static const struct sdt_hdr *g_tables[MAX_TABLES];
static unsigned int          g_table_count;
static unsigned short        g_pm1a_cnt_port;        /* 0 = no FADT */
static unsigned int          g_cpu_count;
static unsigned int          g_lapic_base;
static unsigned int          g_ioapic_base;

static inline void outw(unsigned short port, unsigned short val)
{
	__asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static int memeq(const char *a, const char *b, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		if (a[i] != b[i]) return 0;
	return 1;
}

/* Sum-to-zero checksum over `n` bytes. Per ACPI §5.2.5/§5.2.6 every
 * table (RSDP, SDT) must satisfy this. A bad checksum almost always
 * means we walked into garbage memory, not a corrupted table -- skip
 * the entry rather than trust it. */
static int sum_zero(const void *p, unsigned int n)
{
	const unsigned char *b = (const unsigned char *)p;
	unsigned char s = 0;
	for (unsigned int i = 0; i < n; i++) s = (unsigned char)(s + b[i]);
	return s == 0;
}

static int rsdp_valid(const struct rsdp_v1 *r)
{
	static const char sig[8] = { 'R','S','D',' ','P','T','R',' ' };
	if (!memeq(r->signature, sig, 8)) return 0;
	if (!sum_zero(r, sizeof(*r)))     return 0;
	if (r->revision >= 2) {
		const struct rsdp_v2 *v2 = (const struct rsdp_v2 *)r;
		if (!sum_zero(v2, v2->length)) return 0;
	}
	return 1;
}

/* Scan [phys_lo, phys_hi) on 16-byte boundaries for an RSDP. Returns a
 * kernel-virt pointer to the descriptor or NULL. The whole scan range
 * fits in the high-half direct map (low phys < 1 MiB << 256 MiB), so a
 * straight P2V deref is safe. */
static const struct rsdp_v1 *scan_for_rsdp(unsigned int phys_lo,
                                           unsigned int phys_hi)
{
	for (unsigned int p = phys_lo; p + sizeof(struct rsdp_v1) <= phys_hi;
	     p += 16) {
		const struct rsdp_v1 *r =
		    (const struct rsdp_v1 *)P2V(p);
		if (rsdp_valid(r))
			return r;
	}
	return 0;
}

static const struct rsdp_v1 *find_rsdp(void)
{
	/* EBDA segment word lives at phys 0x40E in the BIOS Data Area; the
	 * segment value shifted left 4 gives the linear address of the
	 * EBDA. On modern firmware EBDA may be absent (BDA word = 0) -- in
	 * that case fall through to the BIOS area scan. */
	const unsigned short *bda_word =
	    (const unsigned short *)P2V(EBDA_PTR_PHYS);
	unsigned int ebda_phys = ((unsigned int)*bda_word) << 4;
	if (ebda_phys >= 0x80000u && ebda_phys < BIOS_AREA_LO) {
		const struct rsdp_v1 *r =
		    scan_for_rsdp(ebda_phys, ebda_phys + EBDA_SCAN_LEN);
		if (r) return r;
	}
	return scan_for_rsdp(BIOS_AREA_LO, BIOS_AREA_HI);
}

static const struct sdt_hdr *table_at_phys(unsigned int phys)
{
	const struct sdt_hdr *h = (const struct sdt_hdr *)P2V(phys);
	if (h->length < SDT_HDR_SIZE) return 0;
	if (!sum_zero(h, h->length))  return 0;
	return h;
}

static void log_hex(unsigned int v)
{
	serial_write("0x");
	serial_write_hex(v);
}

static void log_table(const struct sdt_hdr *h)
{
	serial_write("acpi: ");
	for (int i = 0; i < 4; i++) serial_putc(h->signature[i]);
	serial_write(" @ ");
	log_hex((unsigned int)((unsigned int)h - KERNBASE));
	serial_write(" len=");
	serial_write_dec(h->length);
	serial_putc('\n');
}

void acpi_init(void)
{
	if (g_available) return;
	const struct rsdp_v1 *rsdp = find_rsdp();
	if (!rsdp) {
		serial_write("acpi: RSDP not found, legacy fallback paths only\n");
		return;
	}
	serial_write("acpi: RSDP @ ");
	log_hex((unsigned int)((unsigned int)rsdp - KERNBASE));
	serial_write(" rev=");
	serial_write_dec(rsdp->revision);
	serial_putc('\n');

	/* Pick root table: XSDT if revision >= 2 and the pointer is set,
	 * otherwise RSDT. On real hardware XSDT pointers can sit above
	 * 4 GiB phys; our PMM stays under 32 MiB so we never see that, but
	 * log a warning if any high bits are set and fall back to RSDT. */
	const struct sdt_hdr *root = 0;
	int use_xsdt = 0;
	if (rsdp->revision >= 2) {
		const struct rsdp_v2 *v2 = (const struct rsdp_v2 *)rsdp;
		if (v2->xsdt_phys != 0) {
			if ((v2->xsdt_phys >> 32) != 0) {
				serial_write("acpi: XSDT > 4 GiB, using RSDT\n");
			} else {
				root = table_at_phys((unsigned int)v2->xsdt_phys);
				use_xsdt = (root != 0);
			}
		}
	}
	if (!root) root = table_at_phys(rsdp->rsdt_phys);
	if (!root) {
		serial_write("acpi: root table checksum bad, giving up\n");
		return;
	}

	unsigned int entries =
	    (root->length - SDT_HDR_SIZE) / (use_xsdt ? 8u : 4u);
	const unsigned char *body = (const unsigned char *)root + SDT_HDR_SIZE;
	serial_write(use_xsdt ? "acpi: XSDT entries=" : "acpi: RSDT entries=");
	serial_write_dec(entries);
	serial_putc('\n');

	for (unsigned int i = 0; i < entries; i++) {
		unsigned int phys;
		if (use_xsdt) {
			/* Unaligned 8-byte load split into two 4-byte pieces
			 * so we don't trip a #GP on x86 if the compiler picked
			 * an instruction that requires alignment. */
			unsigned int lo, hi;
			const unsigned char *e = body + i * 8u;
			for (int b = 0; b < 4; b++)
				((unsigned char *)&lo)[b] = e[b];
			for (int b = 0; b < 4; b++)
				((unsigned char *)&hi)[b] = e[4 + b];
			if (hi != 0) {
				serial_write("acpi: skip high-phys entry\n");
				continue;
			}
			phys = lo;
		} else {
			unsigned int v;
			const unsigned char *e = body + i * 4u;
			for (int b = 0; b < 4; b++)
				((unsigned char *)&v)[b] = e[b];
			phys = v;
		}
		const struct sdt_hdr *t = table_at_phys(phys);
		if (!t) continue;
		log_table(t);
		if (g_table_count < MAX_TABLES)
			g_tables[g_table_count++] = t;
	}
	g_available = 1;

	/* Cache PM1a_CNT_BLK for S5 shutdown so sys_shutdown doesn't have
	 * to walk the table list on every call. PM1a is mandatory per
	 * spec; PM1b is optional and we ignore it. */
	const struct fadt *fadt = (const struct fadt *)acpi_find_table("FACP");
	if (fadt && fadt->pm1a_cnt_blk) {
		g_pm1a_cnt_port = (unsigned short)fadt->pm1a_cnt_blk;
		serial_write("acpi: PM1a_CNT_BLK port=");
		log_hex(g_pm1a_cnt_port);
		serial_putc('\n');
	}

	/* MADT walk: count enabled CPUs and grab LAPIC + first IOAPIC base.
	 * No SMP wiring today -- these values are consumed by future SMP
	 * bring-up and by `bootdiag` for visibility. Single-CPU boxes still
	 * see g_cpu_count = 1 because the BSP advertises itself. */
	const struct madt_hdr *madt =
	    (const struct madt_hdr *)acpi_find_table("APIC");
	if (madt) {
		g_lapic_base = madt->lapic_phys;
		const unsigned char *p = (const unsigned char *)madt + sizeof(*madt);
		const unsigned char *end =
		    (const unsigned char *)madt + madt->h.length;
		while (p + sizeof(struct madt_entry_hdr) <= end) {
			const struct madt_entry_hdr *e =
			    (const struct madt_entry_hdr *)p;
			if (e->length == 0 || p + e->length > end) break;
			if (e->type == MADT_TYPE_LAPIC &&
			    e->length >= sizeof(struct madt_lapic)) {
				const struct madt_lapic *l =
				    (const struct madt_lapic *)e;
				if (l->flags & 1u) g_cpu_count++;
			} else if (e->type == MADT_TYPE_IOAPIC &&
			           e->length >= sizeof(struct madt_ioapic) &&
			           g_ioapic_base == 0) {
				const struct madt_ioapic *io =
				    (const struct madt_ioapic *)e;
				g_ioapic_base = io->ioapic_phys;
			}
			p += e->length;
		}
		serial_write("acpi: cpus=");
		serial_write_dec(g_cpu_count);
		serial_write(" lapic=");
		log_hex(g_lapic_base);
		serial_write(" ioapic=");
		log_hex(g_ioapic_base);
		serial_putc('\n');
	}
}

unsigned int acpi_cpu_count(void)   { return g_cpu_count; }
unsigned int acpi_lapic_base(void)  { return g_lapic_base; }
unsigned int acpi_ioapic_base(void) { return g_ioapic_base; }

int acpi_shutdown(void)
{
	if (!g_pm1a_cnt_port) return -1;
	serial_write("acpi: writing S5 to PM1a_CNT_BLK\n");
	outw(g_pm1a_cnt_port, (unsigned short)((S5_SLP_TYPa << 10) | S5_SLP_EN));
	/* Power should drop before we return; if it doesn't (wrong
	 * SLP_TYPa for this platform), let the caller try its fallback. */
	for (volatile int i = 0; i < 1000000; i++) ;
	return 0;
}

int acpi_available(void) { return g_available; }

const void *acpi_find_table(const char *sig4)
{
	if (!g_available) return 0;
	for (unsigned int i = 0; i < g_table_count; i++) {
		if (memeq(g_tables[i]->signature, sig4, 4))
			return g_tables[i];
	}
	return 0;
}
