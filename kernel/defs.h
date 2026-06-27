/* Central kernel umbrella header.
 *
 * Pulls in every kernel subsystem interface so a translation unit can write a
 * single `#include "defs.h"` instead of a long list of per-module headers.
 * The per-subsystem headers under kernel/<area>/ remain the source of truth --
 * each is self-contained and can still be included on its own; this file only
 * aggregates them for the kernel's broad consumers (the boot path, the syscall
 * dispatch) that legitimately touch most subsystems.
 */
#ifndef DEFS_H
#define DEFS_H

/* arch */
#include "io.h"
#include "gdt.h"
#include "pmm.h"
#include "paging.h"

/* core / system */
#include "interrupts.h"
#include "syscall.h"
#include "proc.h"
#include "pipe.h"
#include "timer.h"
#include "klog.h"
#include "bootdiag.h"
#include "elf.h"

/* drivers: video */
#include "vbe.h"
#include "fb.h"
#include "font.h"

/* drivers: input */
#include "keyboard.h"
#include "mouse.h"

/* drivers: storage */
#include "ata.h"

/* drivers: audio */
#include "speaker.h"
#include "audio.h"
#include "dma.h"

/* drivers: misc */
#include "serial.h"
#include "rtc.h"

/* drivers: net */
#include "pci.h"
#include "e1000.h"
#include "net.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "dhcp.h"
#include "dns.h"

/* filesystem */
#include "fs.h"
#include "vfs.h"
#include "mbr.h"

/* ui */
#include "menu.h"

#endif
