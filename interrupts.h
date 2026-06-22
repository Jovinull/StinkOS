/* IDT, PIC remap and PIT timer. */
#ifndef INTERRUPTS_H
#define INTERRUPTS_H

/* Register snapshot pushed by the assembly stubs (low address first). */
struct regs {
	unsigned int ds;
	unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;   /* pusha    */
	unsigned int int_no, err_code;                         /* stub     */
	unsigned int eip, cs, eflags, useresp, ss;             /* cpu      */
};

void interrupts_init(void);          /* build + load the IDT, remap the PIC */
void pit_init(unsigned int hz);      /* program the PIT to fire IRQ0 at hz  */

#endif
