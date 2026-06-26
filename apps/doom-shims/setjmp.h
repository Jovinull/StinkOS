/* <setjmp.h> shim. Implementation in apps/libstink_setjmp.s; the env layout
 * holds esp, ebp, ebx, esi, edi, eip in that order -- 6 ints, 24 bytes. */
#ifndef _STINK_SETJMP_H
#define _STINK_SETJMP_H

typedef int jmp_buf[6];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
