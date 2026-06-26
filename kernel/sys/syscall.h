/* Interface between the trap layer (trap.c) and the syscall layer (syscall.c). */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "interrupts.h"   /* struct regs */

/* Dispatch one int 0x80: eax = call number, args in ebx/ecx/edx/esi, result
 * written back into eax. */
void syscall_dispatch(struct regs *r);

/* Resume after a ring-3 app ends (clean exit or fault): reload the shell if it
 * was launched via SYS_EXEC, otherwise the graphical menu. Does not return. */
void app_return(void);

#endif
