/* W^X adversarial test. Puts a single-byte shellcode (`ret` = 0xC3) into
 * a writable global (.data, mapped RW-NX) and calls it via a function
 * pointer. Pre-W^X this would succeed; post-W^X the CPU page-faults on
 * the instruction fetch because the .data page lacks execute permission
 * (NX bit set on PAE PTE bit 63).
 *
 * The kernel kills the process on the resulting #PF (eip inside the
 * buffer, cr2 == eip). test-headless asserts both the "wxattack: about
 * to jump" log line (proves we made it through .data write + jump prep)
 * AND the matching fault, proving the W^X gate fired. */
#include "libstink.h"

/* Forced into .data via the explicit initializer; the linker keeps any
 * variable with a non-zero initializer in .data rather than .bss. */
static unsigned char shellcode[16] = { 0xC3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

typedef void (*fn_t)(void);

void main(void)
{
	sys_log("wxattack: about to jump into .data");
	fn_t fn = (fn_t)&shellcode[0];
	fn();
	/* If we reach here W^X failed -- kernel should have killed us. */
	sys_log("wxattack: REACHED (W^X NOT enforced!)");
}
