/* System-call dispatch (int 0x80) and its file/VFS/exec helpers. The trap
 * layer (trap.c) routes int 0x80 here via syscall_dispatch, and calls
 * app_return when a ring-3 app ends or faults. */
#include "defs.h"
#include "memlayout.h"
#include "acpi.h"
#include "fs.h"
#include "win.h"

/* Copy a userland filename into a NUL-padded 16-byte kernel buffer, validating
 * that the source pointer lies in the app's mapped memory first. */
static int copy_user_name(unsigned int uname, char *kname)
{
	if (!paging_user_range_ok(uname, 16))
		return -1;
	const char *un = (const char *)uname;
	for (int k = 0; k < 16; k++)
		kname[k] = 0;
	for (int k = 0; k < 15 && un[k]; k++)
		kname[k] = un[k];
	return 0;
}

static int fs_syscall_write(unsigned int uname, unsigned int ubuf, unsigned int size)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	if (!paging_user_range_ok(ubuf, size))
		return -1;
	int r = fs_file_write(kname, (const void *)ubuf, size);
	if (r == 0) {
		serial_write("fs: wrote ");
		serial_write(kname);
		serial_putc('\n');
	}
	return r;
}

static int fs_syscall_append(unsigned int uname, unsigned int ubuf, unsigned int size)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	if (!paging_user_range_ok(ubuf, size))
		return -1;
	int r = fs_file_append(kname, (const void *)ubuf, size);
	if (r == 0) {
		serial_write("fs: appended ");
		serial_write(kname);
		serial_putc('\n');
	}
	return r;
}

static int fs_syscall_write_at(unsigned int uname, unsigned int ubuf,
                               unsigned int size, unsigned int offset)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	if (!paging_user_range_ok(ubuf, size))
		return -1;
	int r = fs_file_write_at(kname, (const void *)ubuf, size, offset);
	if (r == 0) {
		serial_write("fs: wrote@ ");
		serial_write(kname);
		serial_putc('\n');
	}
	return r;
}

static int fs_syscall_read(unsigned int uname, unsigned int ubuf, unsigned int maxsize)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	if (!paging_user_range_ok(ubuf, maxsize))
		return -1;
	int n = fs_file_read(kname, (void *)ubuf, maxsize);
	if (n >= 0) {
		serial_write("fs: read ");
		serial_write(kname);
		serial_putc('\n');
	}
	return n;
}

static int fs_syscall_read_at(unsigned int uname, unsigned int ubuf,
                              unsigned int maxsize, unsigned int offset)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	if (!paging_user_range_ok(ubuf, maxsize))
		return -1;
	int n = fs_file_read_at(kname, (void *)ubuf, maxsize, offset);
	if (n >= 0) {
		serial_write("fs: read@ ");
		serial_write(kname);
		serial_putc('\n');
	}
	return n;
}

static int fs_syscall_delete(unsigned int uname)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	int r = fs_file_delete(kname);
	if (r == 0) {
		serial_write("fs: deleted ");
		serial_write(kname);
		serial_putc('\n');
	}
	return r;
}

/* ---- VFS (file descriptor) syscalls ---- */

static int vfs_syscall_open(unsigned int uname, int flags)
{
	char kname[16];
	if (copy_user_name(uname, kname) != 0)
		return -1;
	return vfs_open(kname, flags);
}

static int vfs_syscall_read(int fd, unsigned int ubuf, unsigned int n)
{
	if (!paging_user_range_ok(ubuf, n))
		return -1;
	return vfs_read(fd, (void *)ubuf, n);
}

static int vfs_syscall_write(int fd, unsigned int ubuf, unsigned int n)
{
	if (!paging_user_range_ok(ubuf, n))
		return -1;
	return vfs_write(fd, (const void *)ubuf, n);
}

/* ---- program launch (SYS_EXEC) ---- */

extern void enter_user_mode(unsigned int entry, unsigned int user_stack);

/* A program started with SYS_EXEC (by the shell) hands control back to a fresh
 * shell when it ends, instead of to the graphical menu. This flag records that
 * we are nested inside such a launch. */
static int exec_active;

/* Build "NAME.ELF" from a user-typed name (e.g. "snake" → "SNAKE.ELF") and
 * verify it exists in StinkFS. Fills elf_out (16 bytes, NUL-padded) and
 * returns 0 on success, -1 if the name is too long or the file is not found. */
static int find_app_elf(const char *name, char *elf_out)
{
	/* Convert to uppercase and measure length. StinkFS name field is 16 bytes
	 * (max 15 visible chars + NUL), so a base name can be at most 11 chars
	 * before ".ELF" is appended (11+4=15). */
	char upper[16];
	int k;
	for (k = 0; k < 15 && name[k]; k++) {
		char c = name[k];
		if (c >= 'a' && c <= 'z') c -= 32;
		upper[k] = c;
	}
	upper[k] = '\0';

	/* Accept names that already end in .ELF; otherwise append it. */
	int has_ext = (k >= 4 &&
	               upper[k-4] == '.' && upper[k-3] == 'E' &&
	               upper[k-2] == 'L' && upper[k-1] == 'F');

	char candidate[16];
	if (has_ext) {
		for (int i = 0; i < 16; i++)
			candidate[i] = (i < k) ? upper[i] : 0;
	} else {
		if (k + 4 > 15) return -1;
		for (int i = 0; i < k; i++) candidate[i] = upper[i];
		candidate[k]   = '.'; candidate[k+1] = 'E';
		candidate[k+2] = 'L'; candidate[k+3] = 'F';
		for (int i = k + 4; i < 16; i++) candidate[i] = 0;
	}

	if (fs_file_size(candidate) < 0)
		return -1;
	for (int i = 0; i < 16; i++) elf_out[i] = candidate[i];
	return 0;
}

/* Load the ELF named 'elf_name' from StinkFS and enter ring 3. Does not return
 * on success; returns -1 if the file is missing or the image is malformed.
 *
 * TODO §1 step 3: each exec gets its own page directory now.
 *   1. Allocate a fresh per-process pgdir (kernel mappings copied,
 *      user window empty) via paging_create_user_pgdir.
 *   2. Populate it with empty PTs + code/stack pages via
 *      paging_init_user_pgdir.
 *   3. Activate it (CR3 + global user_pts cache + heap watermark).
 *   4. elf_load now writes into the NEW pgdir.
 *   5. Free the OLD pgdir -- the previous app's code, stack and heap
 *      frames are returned to the PMM in one shot. The "shared user
 *      window" mode goes away.
 *
 * On first ever exec (when proc_current()->cr3 is still 0), the OLD
 * pgdir is the boot pgdir returned by paging_boot_pgdir(); freeing it
 * reclaims the ~1.25 MB of code + stack pages the boot setup mapped.
 *
 * Refs:
 *   - PRIMARY xv6-public/exec.c:11 (exec): allocproc-style pgdir
 *       lifecycle. xv6's exec stages a NEW pgdir, copies argv, ONLY
 *       installs it on success (last action before returning) -- we
 *       install earlier so elf_load can write directly through the
 *       new CR3. Both are valid; ours saves an intermediate buffer.
 *   - CONTRAST linux-0.01/fs/exec.c (do_execve): keeps the same task
 *       struct (no LDT swap), zeroes pages in place. Worth contrasting
 *       because it shows the failure mode we're avoiding: if
 *       elf_load fails after we zero the old image, the user has no
 *       process at all. By staging into a NEW pgdir we can revert.
 */
static int exec_run_by_elf(const char *elf_name)
{
	unsigned int lba, sectors;
	if (fs_file_lba_sectors(elf_name, &lba, &sectors) != 0)
		return -1;

	audio_mix_silence_all();

	struct proc   *cur        = proc_current();
	unsigned int  *old_pgdir  = cur ? (unsigned int *)cur->cr3 : 0;
	if (!old_pgdir)
		old_pgdir = paging_boot_pgdir();

	unsigned int *new_pgdir = paging_create_user_pgdir();
	if (!new_pgdir)
		return -1;
	if (paging_init_user_pgdir(new_pgdir) != 0) {
		paging_destroy_user_pgdir(new_pgdir);
		return -1;
	}
	paging_activate(new_pgdir);

	unsigned int entry;
	if (elf_load(lba, sectors, &entry) != 0) {
		/* Roll back: restore old CR3, drop the half-baked new pgdir. */
		paging_activate(old_pgdir);
		paging_destroy_user_pgdir(new_pgdir);
		return -1;
	}

	if (old_pgdir != new_pgdir)
		paging_destroy_user_pgdir(old_pgdir);
	if (cur)
		cur->cr3 = (unsigned int)new_pgdir;

	enter_user_mode(entry, paging_user_stack_top());
	return -1;                                 /* unreachable */
}

/* Called when the foreground ring-3 app ends (clean SYS_EXIT or a fault). If it
 * was launched by the shell via SYS_EXEC, hand control to a fresh shell;
 * otherwise fall back to the graphical menu. Does not return. */
void app_return(void)
{
	/* Hush any mixer channels owned by the exiting process before the
	 * scheduler picks a new foreground app: otherwise a sound effect the
	 * dead app queued would keep playing into the next one's audio.
	 * Same idea for any TCP socket the app forgot to close: drive an
	 * orderly FIN so the 8-slot connection table doesn't leak. */
	struct proc *cur = proc_current();
	int pid = cur ? cur->pid : 0;
	audio_mix_silence_pid(pid);
	tcp_close_pid(pid);
	win_destroy(pid);

	/* §1 step 5.5: fork()ed child (any PID > 1) is NOT the foreground
	 * process menu launched. menu_exit's klongjmp would crash the
	 * scheduler -- it switches to the menu kernel stack but the child's
	 * CR3 stays loaded. Mark ZOMBIE and yield instead; the parent's
	 * sys_wait reaps. */
	if (cur && cur->pid > 1) {
		cur->state = PROC_ZOMBIE;
		proc_yield();
		for (;;) __asm__ volatile ("hlt");   /* unreachable */
	}
	if (exec_active) {
		exec_active = 0;
		char shell_elf[16] = "SHELL.ELF\0\0\0\0\0\0\0";
		exec_run_by_elf(shell_elf);        /* reload the shell; no return */
		/* shell missing or unloadable: fall through to the menu */
	}
	menu_exit();                               /* does not return */
}

/* System calls: eax = number, ebx = arg. Result returned in eax. */
void syscall_dispatch(struct regs *r)
{
	/* Re-enable interrupts immediately so long-running syscalls (file I/O,
	 * network polls, mixer-aware sleeps) no longer block the PIT for their
	 * whole duration. The CPU cleared IF on the interrupt gate entry; the
	 * sti here is what unblocks preemption inside this syscall. Args are
	 * already snapshotted into struct regs on the kernel stack, so the
	 * scheduler may freely context-switch us out and back without losing
	 * any state. iret restores the caller's EFLAGS (typically IF=1 for
	 * ring-3 callers), so the on-return state is unchanged. */
	__asm__ volatile ("sti");

	switch (r->eax) {
	case 1: {                                  /* SYS_LOG: ebx = string */
		/* Validate + bound: the old version called serial_write on the
		 * raw user pointer, which would page-fault on a bogus address
		 * and could leak kernel memory over the serial port if the
		 * user string lacked a NUL inside the mapped range. Cap the
		 * scan at 256 bytes and refuse anything that doesn't sit in
		 * the app's mapped region. */
		char buf[256];
		unsigned int n = 0;
		if (!paging_user_range_ok(r->ebx, 1)) {
			r->eax = (unsigned int)-1;
			break;
		}
		const char *src = (const char *)r->ebx;
		while (n < sizeof(buf) - 1 &&
		       paging_user_range_ok((unsigned int)(src + n), 1) &&
		       src[n] != '\0') {
			buf[n] = src[n];
			n++;
		}
		buf[n] = '\0';
		serial_write("ring3: ");
		serial_write(buf);
		serial_putc('\n');
		r->eax = 0;
		break;
	}
	case 2:                                    /* SYS_DRAW: ebx=x ecx=y edx=rgb */
		if (!win_redirect_putpixel((int)proc_current()->pid, r->ebx, r->ecx, r->edx))
			fb_putpixel(r->ebx, r->ecx, r->edx);
		r->eax = 0;
		break;
	case 3:                                    /* SYS_GETKEY: -> char or 0 */
		r->eax = (unsigned char)keyboard_getchar();
		break;
	case 4:                                    /* SYS_ALLOC: -> user page or 0 */
		r->eax = paging_user_alloc();
		break;
	case 5: {                                  /* SYS_EXIT: ebx = exit code */
		struct proc *cur = proc_current();
		if (cur)
			cur->exit_code = (int)r->ebx;
		app_return();                      /* does not return */
		break;
	}
	case 41: {                                 /* SYS_EXEC: ebx=name -> -1 if no such app */
		char kname[16];
		if (copy_user_name(r->ebx, kname) != 0) {
			r->eax = (unsigned int)-1;
			break;
		}
		char elf_name[16];
		if (find_app_elf(kname, elf_name) != 0) {
			r->eax = (unsigned int)-1;
			break;
		}
		serial_write("exec: ");
		serial_write(kname);
		serial_putc('\n');
		exec_active = 1;                   /* child returns to a shell, not the menu */
		exec_run_by_elf(elf_name);         /* replaces the caller; returns only on error */
		exec_active = 0;                   /* image was bad: user region is trashed */
		menu_exit();                       /* bail to the menu; does not return */
		break;
	}
	case 42:                                   /* SYS_MAP_FB: -> userland FB base addr */
		paging_map_fb(fb_phys_base());
		r->eax = paging_user_fb_base();
		break;
	case 43: {                                 /* SYS_NETINFO: ebx=*struct net_info -> 0 ok, -1 bad ptr */
		if (!paging_user_range_ok(r->ebx, sizeof(struct net_info))) {
			r->eax = (unsigned int)-1;
			break;
		}
		struct net_info *ni = (struct net_info *)r->ebx;
		ni->ip         = net_get_local_ip();
		ni->mask       = dhcp_get_subnet_mask();
		ni->gateway    = dhcp_get_router();
		ni->dns        = dhcp_get_dns();
		net_get_local_mac(ni->mac);
		ni->dhcp_state = (unsigned char)dhcp_get_state();
		ni->link_up    = (unsigned char)e1000_present();
		r->eax = 0;
		break;
	}
	case 45: {                                 /* SYS_GETPID: -> caller's PID */
		struct proc *cur = proc_current();
		r->eax = (unsigned int)(cur ? cur->pid : 1);
		break;
	}
	case 60:                                   /* SYS_AUDIO_MASTER: ebx=volume(0..256) -> previous master; ebx > 256 = read-only query */
		r->eax = (unsigned int)audio_get_master();
		if ((int)r->ebx >= 0 && r->ebx <= 256u)
			audio_set_master((int)r->ebx);
		break;
	case 61: {                                 /* SYS_SOCK_LISTEN: ebx=local_port -> handle or -1 */
		unsigned short lport = (unsigned short)r->ebx;
		if (lport == 0) {                  /* port 0 has no addressable use */
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)tcp_listen(lport);
		break;
	}
	case 63: {                                 /* SYS_MBR_READ: ebx=drive ecx=*struct mbr_partition[4] -> 0 / -1 */
		if (!paging_user_range_ok(r->ecx, 4 * sizeof(struct mbr_partition))) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)mbr_read((int)r->ebx,
		                                (struct mbr_partition *)r->ecx);
		break;
	}
	case 81: {                                 /* SYS_MBR_WRITE: ebx=drive ecx=*const struct mbr_partition[4] -> 0 / -1 */
		if (!paging_user_range_ok(r->ecx, 4 * sizeof(struct mbr_partition))) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)mbr_write((int)r->ebx,
		                                 (const struct mbr_partition *)r->ecx);
		break;
	}
	case 64: {                                 /* SYS_RTC_READ: ebx=*struct rtc_time -> 0 / -1 */
		if (!paging_user_range_ok(r->ebx, sizeof(struct rtc_time))) {
			r->eax = (unsigned int)-1;
			break;
		}
		rtc_read((struct rtc_time *)r->ebx);
		r->eax = 0;
		break;
	}
	case 65:                                   /* SYS_SHUTDOWN: power off the machine */
		/* Real ACPI S5 first when the FADT advertised a PM1a_CNT_BLK --
		 * works on physical hardware and on QEMU/VMware. The legacy
		 * port-magic paths below stay as fallback for firmware that
		 * doesn't expose ACPI tables (very old BIOS, Multiboot loaders
		 * that strip them) or where SLP_TYPa differs from our hardcoded
		 * 5 (a real AML interpreter would read \_S5_ for the exact
		 * value -- see TODO.md "AML interpreter -- FUTURE"). */
		acpi_shutdown();
		outw(0x604,  0x2000);                  /* QEMU PIIX4 APM */
		outw(0xB004, 0x2000);                  /* Bochs */
		outw(0x4004, 0x3400);                  /* VirtualBox */
		serial_write("shutdown: requested but no compatible path -- halting\n");
		for (;;) __asm__ volatile ("cli; hlt");
	case 71: {                                 /* SYS_SETPRIO: ebx=pid ecx=prio(0..31) -> 0 / -1 */
		int pid  = (int)r->ebx;
		int prio = (int)r->ecx;
		if (prio < 0 || prio > 31) {
			r->eax = (unsigned int)-1;
			break;
		}
		struct proc *target = proc_get(pid);
		if (!target) {
			r->eax = (unsigned int)-1;
			break;
		}
		target->priority = prio;
		r->eax = 0;
		break;
	}
	case 72: {                                 /* SYS_GETPRIO: ebx=pid -> prio or -1 */
		struct proc *target = proc_get((int)r->ebx);
		r->eax = (unsigned int)(target ? target->priority : -1);
		break;
	}
	case 73: {                                 /* SYS_PROC_INFO: ebx=buf ecx=cap -> bytes */
		if (!paging_user_range_ok(r->ebx, r->ecx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = proc_snapshot((char *)r->ebx, r->ecx);
		break;
	}
	case 74: {                                 /* SYS_ARP_INFO: ebx=buf ecx=cap -> bytes */
		if (!paging_user_range_ok(r->ebx, r->ecx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = arp_snapshot((char *)r->ebx, r->ecx);
		break;
	}
	case 75:                                   /* SYS_ARP_FLUSH: drop every cache entry */
		arp_flush();
		r->eax = 0;
		break;
	case 76:                                   /* SYS_SET_KEYMAP: ebx=layout(0=US,1=BR) -> previous */
		r->eax = (unsigned int)keyboard_set_layout((int)r->ebx);
		break;
	case 77:                                   /* SYS_RTC_SET_ALARM: ebx=packed h<<16 | m<<8 | s -> 0/-1 */
		r->eax = (unsigned int)rtc_set_alarm((r->ebx >> 16) & 0xFFu,
		                                     (r->ebx >>  8) & 0xFFu,
		                                      r->ebx        & 0xFFu);
		break;
	case 78:                                   /* SYS_RTC_CLEAR_ALARM -> 0 */
		rtc_clear_alarm();
		r->eax = 0;
		break;
	case 79:                                   /* SYS_RTC_ALARM_FIRED -> 1 if fired (and clears) else 0 */
		r->eax = (unsigned int)rtc_alarm_fired();
		break;
	case 80: {                                 /* SYS_AUDIO_MODE: ebx=mode (0=u8, 1=s16 mono, 2=s16 stereo) -> 0/-1 */
		audio_stop_output();
		int rc;
		switch (r->ebx) {
		case 0: rc = audio_start_output();        break;
		case 1: rc = audio_start_output_16bit();  break;
		case 2: rc = audio_start_output_stereo(); break;
		default: rc = -1; break;
		}
		r->eax = (unsigned int)rc;
		break;
	}
	case 82:                                   /* SYS_AUDIO_QUERY -> current mode or -1 */
		r->eax = (unsigned int)audio_current_mode();
		break;
	case 69: {                                 /* SYS_SUSPEND: ebx=pid -> 0 / -1 */
		int pid = (int)r->ebx;
		if (pid <= 1) {                       /* never freeze the kernel proc */
			r->eax = (unsigned int)-1;
			break;
		}
		struct proc *target = proc_get(pid);
		if (!target || target->state == PROC_ZOMBIE) {
			r->eax = (unsigned int)-1;
			break;
		}
		target->state = PROC_SLEEPING;
		audio_mix_silence_pid(pid);           /* hush its mixer channels */
		r->eax = 0;
		break;
	}
	case 70: {                                 /* SYS_RESUME: ebx=pid -> 0 / -1 */
		int pid = (int)r->ebx;
		struct proc *target = proc_get(pid);
		if (!target || target->state != PROC_SLEEPING) {
			r->eax = (unsigned int)-1;
			break;
		}
		target->state = PROC_READY;
		r->eax = 0;
		break;
	}
	case 68: {                                 /* SYS_KLOG_READ: ebx=buf ecx=cap -> bytes copied */
		if (!paging_user_range_ok(r->ebx, r->ecx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = klog_read((char *)r->ebx, r->ecx);
		break;
	}
	case 67: {                                 /* SYS_BLIT_SCALED: ebx=src ecx=dst_xy(packed) edx=dst_wh(packed) esi=src_wh(packed) -> 0 */
		unsigned int dst_w = (r->edx >> 16) & 0xFFFFu;
		unsigned int dst_h =  r->edx        & 0xFFFFu;
		unsigned int src_w = (r->esi >> 16) & 0xFFFFu;
		unsigned int src_h =  r->esi        & 0xFFFFu;
		/* Integer-overflow guard: src_w * src_h * 4 must fit in a
		 * 32-bit unsigned. Without it a userland pair like 65535x65535
		 * wraps the multiplication, paging_user_range_ok sees a tiny
		 * size and approves, and fb_blit_scaled then reads gigabytes
		 * of attacker-controlled memory. Same pattern guarded in case
		 * 26 (SYS_BLIT) -- this just brings parity. */
		unsigned int bytes = src_w * src_h * sizeof(unsigned int);
		if (src_w == 0 || src_h == 0 || bytes / 4u / src_w != src_h ||
		    !paging_user_range_ok(r->ebx, bytes)) {
			r->eax = (unsigned int)-1;
			break;
		}
		fb_blit_scaled((r->ecx >> 16) & 0xFFFFu, r->ecx & 0xFFFFu,
		               dst_w, dst_h,
		               (const unsigned int *)r->ebx, src_w, src_h);
		r->eax = 0;
		break;
	}
	case 66:                                   /* SYS_REBOOT: warm-reset via the 8042 keyboard controller */
		/* The classic PC reset path: pulse the 8042 reset line low. */
		serial_write("reboot: pulsing 8042 reset line\n");
		while (inb(0x64) & 0x02)              /* wait until input buffer drains */
			;
		outb(0x64, 0xFE);                       /* command 0xFE = CPU reset */
		for (;;) __asm__ volatile ("cli; hlt"); /* unreachable on a real CPU */
	case 59: {                                 /* SYS_MEMINFO: ebx=*meminfo -> 0 / -1 */
		if (!paging_user_range_ok(r->ebx, 3 * sizeof(unsigned int))) {
			r->eax = (unsigned int)-1;
			break;
		}
		unsigned int *m = (unsigned int *)r->ebx;
		m[0] = pmm_total_pages();
		m[1] = pmm_free_pages();
		m[2] = paging_user_brk();
		r->eax = 0;
		break;
	}
	case 58: {                                 /* SYS_NETSTAT: ebx=idx ecx=*tcp_info -> 0 / -1 */
		if (!paging_user_range_ok(r->ecx, sizeof(struct tcp_info))) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)tcp_get_info((int)r->ebx,
		                                    (struct tcp_info *)r->ecx);
		break;
	}
	case 56: {                                 /* SYS_MMAP: ebx=size -> vaddr (0 if OOM) */
		r->eax = paging_user_mmap(r->ebx);
		break;
	}
	case 57: {                                 /* SYS_MUNMAP: ebx=addr ecx=size -> 0 / -1 */
		r->eax = (unsigned int)paging_user_munmap(r->ebx, r->ecx);
		break;
	}
	case 53: {                                 /* SYS_SIGNAL: ebx=sig ecx=handler_addr -> 0 ok / -1 */
		unsigned int sig = r->ebx;
		if (sig >= PROC_NSIG) {
			r->eax = (unsigned int)-1;
			break;
		}
		struct proc *cur = proc_current();
		if (!cur) {
			r->eax = (unsigned int)-1;
			break;
		}
		cur->sig_handlers[sig] = r->ecx;
		r->eax = 0;
		break;
	}
	case 54: {                                 /* SYS_RAISE: ebx=pid ecx=sig -> 0 ok / -1 */
		int pid = (int)r->ebx;
		unsigned int sig = r->ecx;
		if (sig >= PROC_NSIG) {
			r->eax = (unsigned int)-1;
			break;
		}
		struct proc *target = proc_get(pid);
		if (!target) {
			r->eax = (unsigned int)-1;
			break;
		}
		target->pending_signals |= (1u << sig);
		r->eax = 0;
		break;
	}
	case 55: {                                 /* SYS_SIGPOLL: -> next pending signal + handler addr; 0 if none */
		/* Returns a single 32-bit value packing (sig << 24) | (handler_addr & 0x00FFFFFF)
		 * is too lossy; instead we drain one bit and return the signal number, and the
		 * caller can re-query its own handler with a follow-up SYS_SIGNAL(sig, NULL)
		 * pattern -- not done here. For this cooperative model the caller already
		 * registered the handler, so it knows what to dispatch. */
		struct proc *cur = proc_current();
		if (!cur || !cur->pending_signals) {
			r->eax = 0;
			break;
		}
		/* Pick the lowest-numbered pending signal. */
		unsigned int b = cur->pending_signals;
		unsigned int sig = 0;
		while (!(b & 1)) {
			b >>= 1;
			sig++;
		}
		cur->pending_signals &= ~(1u << sig);
		r->eax = sig;
		break;
	}
	case 49: {                                 /* SYS_PIPE: ebx=int fds[2] -> 0 ok, -1 fail */
		if (!paging_user_range_ok(r->ebx, 2 * sizeof(int))) {
			r->eax = (unsigned int)-1;
			break;
		}
		int *fds = (int *)r->ebx;
		r->eax = (unsigned int)pipe_alloc(&fds[0], &fds[1]);
		break;
	}
	case 50: {                                 /* SYS_PIPE_READ: ebx=h ecx=buf edx=n -> bytes or -1 */
		if (!paging_user_range_ok(r->ecx, r->edx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)pipe_read((int)r->ebx, (void *)r->ecx, r->edx);
		break;
	}
	case 51: {                                 /* SYS_PIPE_WRITE: ebx=h ecx=buf edx=n -> bytes or -1 */
		if (!paging_user_range_ok(r->ecx, r->edx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)pipe_write((int)r->ebx, (const void *)r->ecx, r->edx);
		break;
	}
	case 52:                                   /* SYS_PIPE_CLOSE: ebx=h -> 0 or -1 */
		r->eax = (unsigned int)pipe_close((int)r->ebx);
		break;
	case 47: {                                 /* SYS_WAIT: -> exit_code of any reaped child, or -1 */
		for (;;) {
			struct proc *z = proc_find_zombie_child(0);
			if (z) {
				r->eax = (unsigned int)proc_reap(z);
				break;
			}
			if (!proc_has_living_child()) {
				r->eax = (unsigned int)-1;
				break;
			}
			/* No zombie yet but children still alive. Sleep one tick at a
			 * time -- sti+hlt lets the PIT IRQ fire and the scheduler hand
			 * the CPU to whichever child might be ready to exit. */
			__asm__ volatile ("sti; hlt; cli");
		}
		break;
	}
	case 48: {                                 /* SYS_WAITPID: ebx=pid -> exit_code, or -1 */
		int target_pid = (int)r->ebx;
		for (;;) {
			struct proc *z = proc_find_zombie_child(target_pid);
			if (z) {
				r->eax = (unsigned int)proc_reap(z);
				break;
			}
			/* Bail when the requested PID is neither a zombie child nor a
			 * living child -- prevents waitpid() on a foreign PID looping
			 * forever. */
			struct proc *anyone = proc_get(target_pid);
			if (!anyone || anyone->parent_pid != proc_current()->pid) {
				r->eax = (unsigned int)-1;
				break;
			}
			__asm__ volatile ("sti; hlt; cli");
		}
		break;
	}
	case 46: {                                 /* SYS_KILL: ebx=pid -> 0 or -1 */
		int pid = (int)r->ebx;
		if (pid <= 1) {                    /* PID 1 (kinit) is not killable */
			r->eax = (unsigned int)-1;
			break;
		}
		struct proc *target = proc_get(pid);
		if (!target) {
			r->eax = (unsigned int)-1;
			break;
		}
		target->state     = PROC_ZOMBIE;
		target->exit_code = 137;           /* SIGKILL exit-code convention */
		audio_mix_silence_pid(pid);        /* hush its mixer channels */
		tcp_close_pid(pid);                /* reap any sockets it left open */
		win_destroy(pid);                  /* release any compositor window */
		serial_write("proc: killed pid ");
		serial_write_dec(pid);
		serial_putc('\n');
		r->eax = 0;
		break;
	}
	case 44: {                                 /* SYS_PING: ebx=ipv4(net order) ecx=timeout_ms -> rtt_ms or -1 */
		if (!e1000_present() || !ipv4_is_unicast((ipv4_t)r->ebx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		static unsigned short ping_seq;
		unsigned short id  = 0x5453;       /* 'TS' */
		unsigned short seq = ++ping_seq;
		const char payload[] = "stinkos-ping";

		icmp_ping_arm(id, seq);
		if (icmp_send_echo_request((ipv4_t)r->ebx, id, seq,
		                           payload, sizeof(payload) - 1) < 0) {
			r->eax = (unsigned int)-1;
			break;
		}

		unsigned int start = pit_ticks();
		unsigned int limit = (r->ecx + 9) / 10;   /* ms -> 100 Hz ticks, round up */
		if (limit == 0)
			limit = 1;

		int rtt = -1;
		while ((unsigned int)(pit_ticks() - start) <= limit) {
			net_poll_once();               /* poll RX so the reply is seen */
			if (icmp_ping_replied()) {
				rtt = (int)((pit_ticks() - start) * 10);   /* ticks -> ms */
				break;
			}
			/* The syscall arrived through an interrupt gate (IF cleared), so
			 * without this window the PIT would never tick and the timeout
			 * below could never fire -- the same sti+hlt+cli pattern SYS_SLEEP
			 * uses to wait on the timer without losing an IRQ. */
			__asm__ volatile ("sti; hlt; cli");
		}
		r->eax = (unsigned int)rtt;
		break;
	}
	case 6:                                    /* SYS_TICKS: -> PIT ticks */
		r->eax = pit_ticks();
		break;
	case 7:                                    /* SYS_SOUND: ebx = freq (0=off) */
		speaker_play(r->ebx);
		r->eax = 0;
		break;
	case 8:                                    /* SYS_FWRITE: ebx=name ecx=buf edx=size */
		r->eax = (unsigned int)fs_syscall_write(r->ebx, r->ecx, r->edx);
		break;
	case 9:                                    /* SYS_FREAD: ebx=name ecx=buf edx=max */
		r->eax = (unsigned int)fs_syscall_read(r->ebx, r->ecx, r->edx);
		break;
	case 10:                                   /* SYS_FCOUNT: -> number of files */
		r->eax = (unsigned int)fs_file_count();
		break;
	case 11:                                   /* SYS_FINFO: ebx=index ecx=name buf */
		if (!paging_user_range_ok(r->ecx, 16)) {
			r->eax = (unsigned int)-1;
		} else {
			r->eax = (unsigned int)fs_file_info((int)r->ebx, (char *)r->ecx);
			if ((int)r->eax >= 0) {
				serial_write("fs: info ");
				serial_write((const char *)r->ecx);
				serial_putc('\n');
			}
		}
		break;
	case 12:                                   /* SYS_FDELETE: ebx=name */
		r->eax = (unsigned int)fs_syscall_delete(r->ebx);
		break;
	case 13:                                   /* SYS_FAPPEND: ebx=name ecx=buf edx=size */
		r->eax = (unsigned int)fs_syscall_append(r->ebx, r->ecx, r->edx);
		break;
	case 14:                                   /* SYS_FREAD_AT: ebx=name ecx=buf edx=max esi=offset */
		r->eax = (unsigned int)fs_syscall_read_at(r->ebx, r->ecx, r->edx, r->esi);
		break;
	case 15:                                   /* SYS_FWRITE_AT: ebx=name ecx=buf edx=size esi=offset */
		r->eax = (unsigned int)fs_syscall_write_at(r->ebx, r->ecx, r->edx, r->esi);
		break;
	case 16:                                   /* SYS_OPEN: ebx=name ecx=flags -> fd */
		r->eax = (unsigned int)vfs_syscall_open(r->ebx, (int)r->ecx);
		break;
	case 17:                                   /* SYS_CLOSE: ebx=fd */
		r->eax = (unsigned int)vfs_close((int)r->ebx);
		break;
	case 18:                                   /* SYS_READ: ebx=fd ecx=buf edx=n */
		r->eax = (unsigned int)vfs_syscall_read((int)r->ebx, r->ecx, r->edx);
		break;
	case 19:                                   /* SYS_WRITE: ebx=fd ecx=buf edx=n */
		r->eax = (unsigned int)vfs_syscall_write((int)r->ebx, r->ecx, r->edx);
		break;
	case 20:                                   /* SYS_SEEK: ebx=fd ecx=offset edx=whence */
		r->eax = (unsigned int)vfs_seek((int)r->ebx, (int)r->ecx, (int)r->edx);
		break;
	case 21: {                                 /* SYS_DRAWTEXT: ebx=x ecx=y edx=str esi=rgb */
		/* fb_text scans the string until NUL with no length cap, so a
		 * user buffer without a NUL inside its mapped range used to
		 * let the renderer read past validated memory. Copy into a
		 * kernel buffer first, terminating as soon as we leave the
		 * app's region. 128 bytes is wider than the framebuffer can
		 * draw on one line at 8 px/char (1024/8 = 128). */
		char kbuf[128];
		if (!paging_user_range_ok(r->edx, 1)) {
			r->eax = (unsigned int)-1;
			break;
		}
		const char *us = (const char *)r->edx;
		unsigned int n = 0;
		while (n < sizeof(kbuf) - 1 &&
		       paging_user_range_ok((unsigned int)(us + n), 1) &&
		       us[n] != '\0') {
			kbuf[n] = us[n];
			n++;
		}
		kbuf[n] = '\0';
		fb_text((int)r->ebx, (int)r->ecx, kbuf, r->esi);
		r->eax = 0;
		break;
	}
	case 22: {                                 /* SYS_FILLRECT: ebx=(x<<16|y) ecx=(w<<16|h) edx=rgb */
		unsigned int rx = r->ebx >> 16, ry = r->ebx & 0xFFFF;
		unsigned int rw = r->ecx >> 16, rh = r->ecx & 0xFFFF;
		if (!win_redirect_fillrect((int)proc_current()->pid, rx, ry, rw, rh, r->edx))
			fb_rect(rx, ry, rw, rh, r->edx);
		r->eax = 0;
		break;
	}
	case 23: {                                 /* SYS_SLEEP_MS: ebx = milliseconds */
		/* PIT runs at 100 Hz, so one tick is 10 ms. Round up so a request
		 * for any non-zero number of ms always waits at least one tick.
		 * The syscall arrived through an interrupt gate (IF cleared), so
		 * the timer IRQ would never fire if we just spun on `ticks`. Open
		 * a window with sti+hlt: the CPU guarantees IF stays clear for one
		 * instruction after sti, making the pair atomic against IRQs. */
		unsigned int delta = (r->ebx + 9) / 10;
		unsigned int start = pit_ticks();
		while ((unsigned int)(pit_ticks() - start) < delta)
			__asm__ volatile ("sti; hlt; cli");
		r->eax = 0;
		break;
	}
	case 24: {                                 /* SYS_SBRK: ebx = signed delta -> old break, or -1 */
		int delta = (int)r->ebx;
		unsigned int old = paging_user_brk();
		unsigned int target;

		if (delta >= 0) {
			target = old + (unsigned int)delta;
			if (target < old) {                /* address overflow */
				r->eax = (unsigned int)-1;
				break;
			}
		} else {
			unsigned int abs_delta = (unsigned int)(-delta);
			if (abs_delta > old) {             /* underflows below heap base */
				r->eax = (unsigned int)-1;
				break;
			}
			target = old - abs_delta;
		}

		unsigned int got = paging_user_set_brk(target);
		if ((delta > 0 && got < target) ||
		    (delta < 0 && got > target)) {
			r->eax = (unsigned int)-1;         /* OOM or hit USER_HEAP_HI */
		} else {
			r->eax = old;
		}
		break;
	}
	case 25:                                   /* SYS_GETKEYEVENT: -> packed event or 0 */
		r->eax = keyboard_get_event();
		break;
	case 28: {                                 /* SYS_AUDIO_PLAY: ebx=samples ecx=length edx=volume -> handle or -1 */
		if (!paging_user_range_ok(r->ebx, r->ecx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)audio_mix_play((const unsigned char *)r->ebx,
		                                      r->ecx,
		                                      (int)r->edx);
		break;
	}
	case 29:                                   /* SYS_AUDIO_STOP: ebx=handle */
		audio_mix_stop((int)r->ebx);
		r->eax = 0;
		break;
	case 30:                                   /* SYS_AUDIO_SET_VOLUME: ebx=handle ecx=volume */
		audio_mix_set_volume((int)r->ebx, (int)r->ecx);
		r->eax = 0;
		break;
	case 31: {                                 /* SYS_SOCK_CONNECT: ebx=ipv4 ecx=port -> handle */
		/* Reject destinations that don't represent a valid TCP peer.
		 * Letting bcast/mcast/loopback through would either get the
		 * TCB stuck in SYN_SENT forever or splash SYNs onto the LAN
		 * broadcast group. */
		ipv4_t dst = (ipv4_t)r->ebx;
		unsigned short dport = (unsigned short)r->ecx;
		if (!ipv4_is_unicast(dst) || dport == 0) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)tcp_connect(dst, dport);
		break;
	}
	case 32: {                                 /* SYS_SOCK_SEND: ebx=h ecx=buf edx=len */
		if (!paging_user_range_ok(r->ecx, r->edx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)tcp_send((int)r->ebx, (const void *)r->ecx, r->edx);
		break;
	}
	case 33: {                                 /* SYS_SOCK_RECV: ebx=h ecx=buf edx=max */
		if (!paging_user_range_ok(r->ecx, r->edx)) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)tcp_recv((int)r->ebx, (void *)r->ecx, r->edx);
		break;
	}
	case 34:                                   /* SYS_SOCK_CLOSE: ebx=h */
		tcp_close((int)r->ebx);
		r->eax = 0;
		break;
	case 35:                                   /* SYS_SOCK_STATE: ebx=h -> state */
		r->eax = (unsigned int)tcp_get_state((int)r->ebx);
		break;
	case 36: {                                 /* SYS_DNS_REQUEST: ebx=name (string) */
		/* dns_resolve walks the name with no length bound, so we have
		 * to copy it into a kernel buffer first -- otherwise a missing
		 * NUL in the user buffer would let the resolver read past the
		 * mapped region into kernel memory and either crash or leak
		 * bytes via the DNS query payload. The on-wire qname format
		 * caps single labels at 63 bytes; 64 leaves room for one
		 * terminator and matches DNS_CACHE_NAME. */
		char kname[64];
		if (!paging_user_range_ok(r->ebx, 1)) {
			r->eax = (unsigned int)-1;
			break;
		}
		const char *un = (const char *)r->ebx;
		unsigned int n = 0;
		while (n < sizeof(kname) - 1 &&
		       paging_user_range_ok((unsigned int)(un + n), 1) &&
		       un[n] != '\0') {
			kname[n] = un[n];
			n++;
		}
		kname[n] = '\0';
		r->eax = (unsigned int)dns_resolve(kname);
		break;
	}
	case 37: {                                 /* SYS_DNS_POLL: ebx=*ipv4 -> 1 if ready */
		if (!paging_user_range_ok(r->ebx, sizeof(ipv4_t))) {
			r->eax = (unsigned int)-1;
			break;
		}
		if (dns_ready()) {
			*(ipv4_t *)r->ebx = dns_get_ip();
			r->eax = 1;
		} else {
			r->eax = 0;
		}
		break;
	}
	case 38:                                   /* SYS_NET_POLL: -> 1 if frame processed, 0 if idle */
		r->eax = (unsigned int)net_poll_once();
		break;
	case 39: {                                 /* SYS_DISK_INFO: ebx=drive ecx=model[41] edx=*sectors */
		if (!paging_user_range_ok(r->ecx, 41) ||
		    !paging_user_range_ok(r->edx, sizeof(unsigned int))) {
			r->eax = (unsigned int)-1;
			break;
		}
		unsigned int secs = 0;
		int rc = ata_drive_identify((int)r->ebx, (char *)r->ecx, &secs);
		if (rc == 0)
			*(unsigned int *)r->edx = secs;
		r->eax = (unsigned int)rc;
		break;
	}
	case 40: {                                 /* SYS_DISK_COPY: ebx=src_drv ecx=dst_drv edx=count esi=src_lba (dst_lba == src_lba for now) */
		unsigned int src_drv  = r->ebx;
		unsigned int dst_drv  = r->ecx;
		unsigned int count    = r->edx;
		unsigned int src_lba  = r->esi;
		/* Cap one syscall at 4096 sectors (~2 MiB). Without this a
		 * userland app passing count = 0xFFFFFFFF would pin the kernel
		 * inside the synchronous ATA loop until every sector read
		 * failed -- many seconds during which keyboard / network IRQs
		 * still run but no other process gets scheduled. The installer
		 * already chunks at 256 sectors per call. */
		if (count > 4096u)
			count = 4096u;
		unsigned char buf[512];
		unsigned int copied = 0;
		for (unsigned int i = 0; i < count; i++) {
			if (ata_drive_read((int)src_drv, src_lba + i, 1, buf) != 0)
				break;
			if (ata_drive_write((int)dst_drv, src_lba + i, 1, buf) != 0)
				break;
			copied++;
		}
		r->eax = copied;
		break;
	}
	case 27: {                                 /* SYS_GETMOUSE: ebx=*dx ecx=*dy edx=*buttons */
		if (!paging_user_range_ok(r->ebx, sizeof(int)) ||
		    !paging_user_range_ok(r->ecx, sizeof(int)) ||
		    !paging_user_range_ok(r->edx, sizeof(int))) {
			r->eax = (unsigned int)-1;
			break;
		}
		int dx, dy;
		unsigned char buttons;
		mouse_consume_delta(&dx, &dy, &buttons);
		*(int *)r->ebx = dx;
		*(int *)r->ecx = dy;
		*(int *)r->edx = (int)buttons;
		r->eax = 0;
		break;
	}
	case 26: {                                 /* SYS_BLIT: ebx=src, ecx=(x<<16|y), edx=(w<<16|h) */
		unsigned int x = r->ecx >> 16;
		unsigned int y = r->ecx & 0xFFFF;
		unsigned int w = r->edx >> 16;
		unsigned int h = r->edx & 0xFFFF;
		unsigned int bytes = w * h * 4;
		if (w == 0 || h == 0 || bytes / 4 / w != h) {  /* overflow guard */
			r->eax = (unsigned int)-1;
			break;
		}
		if (!paging_user_range_ok(r->ebx, bytes)) {
			r->eax = (unsigned int)-1;
			break;
		}
		if (!win_redirect_blit((int)proc_current()->pid, x, y, w, h,
		                       (const unsigned int *)r->ebx))
			fb_blit(x, y, w, h, (const unsigned int *)r->ebx);
		r->eax = 0;
		break;
	}
	case 84:                                   /* SYS_MOUNT: ebx=(slot<<8)|drive ecx=dir_lba edx=data_lba esi=data_end -> 0/-1 */
		r->eax = (unsigned int)fs_mount_register(
		    (int)((r->ebx >> 8) & 0xFFu),
		    (int)(r->ebx & 0xFFu),
		    r->ecx, r->edx, r->esi);
		break;
	case 83: {                                 /* SYS_FORK -> child PID, 0 in child, -1 on err */
		/* TODO §1 step 4: cooperative fork.
		 *
		 * 1. allocate child PCB + 4 KiB kernel stack
		 * 2. allocate child pgdir (kernel mappings inherited, user empty)
		 * 3. deep-copy parent's user pages into child pgdir (no COW v1)
		 * 4. clone struct regs onto child kstack, force child->eax = 0
		 * 5. pre-build a context_switch frame on child kstack so the
		 *    scheduler's first switch into child ret's into trap_return,
		 *    which pops the clone of struct regs and iret's to user
		 *    at the parent's user EIP (matching ESP/eflags/etc).
		 * 6. parent's eax = child PID; child's eax = 0.
		 *
		 * Refs:
		 *   - PRIMARY xv6-public/proc.c:181 (fork): allocproc + copyuvm +
		 *       clone tf + np->tf->eax = 0. Same five-step shape. xv6
		 *       splits forkret/trapret; we collapse into one
		 *       trap_return tail because StinkOS has no kernel-thread
		 *       initialiser to run before the user resumes.
		 *   - CONTRAST toaruos/kernel/sys/process.c:1392 (fork): uses
		 *       arch_resume_user as the child's IP and clones via
		 *       arch_save_context. Equivalent semantics; their PAL
		 *       (process abstraction layer) makes the asm-coupled
		 *       parts hide behind helpers. We keep it visible because
		 *       it's only ~30 lines and one read-through explains the
		 *       whole control-flow.
		 */
		struct proc *parent = proc_current();
		if (!parent) { r->eax = (unsigned int)-1; break; }

		struct proc *child = proc_alloc(parent->name);
		if (!child)  { r->eax = (unsigned int)-1; break; }

		unsigned int kpage = pmm_alloc();
		if (!kpage) {
			proc_free(child);
			r->eax = (unsigned int)-1;
			break;
		}
		/* P2V: pmm_alloc returns phys; kstack_top is dereferenced as a
		 * virt pointer (regs clone below, TSS.esp0 on context switch).
		 * Higher-half mirror routes the deref away from any user PT. */
		child->kstack_top = P2V(kpage) + 4096u;

		unsigned int *parent_pgdir = (unsigned int *)parent->cr3;
		if (!parent_pgdir) parent_pgdir = paging_boot_pgdir();
		unsigned int *child_pgdir = paging_create_user_pgdir();
		if (!child_pgdir) {
			pmm_free(kpage); proc_free(child);
			r->eax = (unsigned int)-1;
			break;
		}
		if (paging_copy_user_pgdir(child_pgdir, parent_pgdir) != 0) {
			paging_destroy_user_pgdir(child_pgdir);
			pmm_free(kpage); proc_free(child);
			r->eax = (unsigned int)-1;
			break;
		}
		child->cr3 = (unsigned int)child_pgdir;

		/* Lay out child's kernel stack. Top-down:
		 *   [HIGH] struct regs (clone of *r, eax=0)
		 *   [    ] return-addr = trap_return
		 *   [    ] eflags
		 *   [    ] ebp, ebx, esi, edi  (popped by context_switch in that order:
		 *                              edi first, hence sits at the LOW end)
		 *   [LOW]  <- child->esp
		 */
		extern void trap_return(void);
		unsigned char *byte_sp = (unsigned char *)child->kstack_top;
		byte_sp -= sizeof(struct regs);
		struct regs *child_regs = (struct regs *)byte_sp;
		*child_regs      = *r;
		child_regs->eax  = 0;                  /* child sees fork() == 0 */
		unsigned int *sp = (unsigned int *)byte_sp;
		*(--sp) = (unsigned int)trap_return;   /* ret -> trap_return */
		*(--sp) = r->eflags;                   /* eflags (popf) */
		*(--sp) = 0;                           /* ebp */
		*(--sp) = 0;                           /* ebx */
		*(--sp) = 0;                           /* esi */
		*(--sp) = 0;                           /* edi */
		child->esp = (unsigned int)sp;

		/* Inherit fd table + signal handlers; clear pending signals so
		 * the child doesn't immediately re-handle them. */
		for (int i = 0; i < VFS_FD_MAX; i++)
			child->fd_table[i] = parent->fd_table[i];
		for (int i = 0; i < PROC_NSIG; i++)
			child->sig_handlers[i] = parent->sig_handlers[i];
		child->pending_signals = 0;
		child->priority        = parent->priority;

		child->state = PROC_READY;
		r->eax = (unsigned int)child->pid;     /* parent return */
		break;
	}
	/* ── Window / compositor syscalls ──────────────────────────────────── */
	case 85: {                                /* SYS_WIN_CREATE: ebx=w ecx=h */
		int pid = (int)proc_current()->pid;
		r->eax = (unsigned int)win_create(pid, r->ebx, r->ecx);
		break;
	}
	case 86: {                                /* SYS_WIN_SHOW: ebx=x ecx=y edx=*title */
		int pid = (int)proc_current()->pid;
		const char *title = 0;
		if (r->edx && paging_user_range_ok(r->edx, 64))
			title = (const char *)r->edx;
		r->eax = (unsigned int)win_show(pid, (int)r->ebx, (int)r->ecx, title);
		break;
	}
	case 87:                                  /* SYS_WIN_FLUSH */
		win_flush((int)proc_current()->pid);
		r->eax = 0;
		break;
	case 88:                                  /* SYS_WIN_DESTROY */
		win_destroy((int)proc_current()->pid);
		r->eax = 0;
		break;
	case 89: {                                /* SYS_WIN_GET_EVENT: ebx=*win_event */
		if (!r->ebx || !paging_user_range_ok(r->ebx, sizeof(struct win_event))) {
			r->eax = (unsigned int)-1;
			break;
		}
		r->eax = (unsigned int)win_get_event((int)proc_current()->pid,
		                                      (struct win_event *)r->ebx);
		break;
	}
	case 90:                                  /* SYS_WIN_RAISE */
		win_raise((int)proc_current()->pid);
		r->eax = 0;
		break;
	case 91:                                  /* SYS_WIN_MOVE: ebx=x ecx=y */
		win_move((int)proc_current()->pid, (int)r->ebx, (int)r->ecx);
		r->eax = 0;
		break;
	default:
		r->eax = (unsigned int)-1;
		break;
	}
}

