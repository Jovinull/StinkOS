/* Boot-time Power-On Self-Test diagnostics.
 *
 * kmain records each subsystem's outcome as it initialises. Every result is
 * logged to the serial console immediately (so the diagnostic survives even
 * with no framebuffer) and, once video is up, rendered as a BIOS-style POST
 * panel for a moment before the start menu takes over. This is what lets a
 * silently-absent driver (e.g. no NIC, no sound card) become visible to the
 * user instead of failing unnoticed deep in the boot chain.
 */
#ifndef BOOTDIAG_H
#define BOOTDIAG_H

enum boot_status {
	BOOT_OK     = 0,    /* initialised and present                      */
	BOOT_FAIL   = 1,    /* a subsystem that should work did not          */
	BOOT_ABSENT = 2,    /* optional hardware not present (not an error)  */
};

/* Record one subsystem result and emit a "post: <name> <status>" serial line. */
void bootdiag_add(const char *name, enum boot_status status);

/* Render the collected results as a POST panel on the framebuffer and hold it
 * briefly so it is readable. Requires interrupts enabled (it sleeps on the PIT)
 * and a live framebuffer; call after video init, just before the menu. */
void bootdiag_show(void);

#endif
