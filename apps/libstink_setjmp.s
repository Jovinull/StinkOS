/* Userland setjmp/longjmp -- the same callee-saved-register save and restore
 * as ksetjmp/klongjmp in usermode_asm.s, but exported with the C standard
 * names so apps can use the regular <setjmp.h> idiom.
 *
 * jmp_buf layout (6 ints, 24 bytes):
 *   offset 0  -> esp
 *   offset 4  -> ebp
 *   offset 8  -> ebx
 *   offset 12 -> esi
 *   offset 16 -> edi
 *   offset 20 -> eip (return address at the setjmp call site)
 *
 * The System V i386 ABI marks eax, ecx, edx as caller-saved, so we don't need
 * to preserve them; the rest are callee-saved and live in the buffer.
 */
.code32

# int setjmp(jmp_buf env)
.global setjmp
setjmp:
	mov 4(%esp), %eax       # env pointer
	mov (%esp), %ecx        # return address (call's pushed eip)
	mov %ecx, 20(%eax)
	mov %esp, 0(%eax)
	mov %ebp, 4(%eax)
	mov %ebx, 8(%eax)
	mov %esi, 12(%eax)
	mov %edi, 16(%eax)
	xor %eax, %eax          # initial return value: 0
	ret

# void longjmp(jmp_buf env, int val)
.global longjmp
longjmp:
	mov 4(%esp), %eax       # env
	mov 8(%esp), %edx       # val
	mov 0(%eax), %esp
	mov 4(%eax), %ebp
	mov 8(%eax), %ebx
	mov 12(%eax), %esi
	mov 16(%eax), %edi
	mov 20(%eax), %ecx
	mov %edx, %eax
	test %eax, %eax
	jne 1f
	mov $1, %eax            # POSIX: setjmp never observes 0 via longjmp
1:
	jmp *%ecx
