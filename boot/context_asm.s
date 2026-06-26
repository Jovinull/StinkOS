# Cooperative context switch between two kernel stacks.
#
# void context_switch(unsigned int *old_esp_ptr, unsigned int new_esp)
#
# Saves the System V i386 callee-saved registers (EBX, ESI, EDI, EBP) and
# EFLAGS onto the current stack, then stores the resulting ESP into
# *old_esp_ptr and reloads ESP from `new_esp`. Pops the same five values back
# off the new stack and rets, so the caller resumes at the matching site in
# whichever code last called context_switch on that stack. A brand-new stack
# must be pre-built by context_init() in proc.c so the first switch-in pops a
# legitimate entry-point return address.
#
# Stack layout right after the five pushes (top first):
#     EDI         <-- ESP
#     ESI
#     EBX
#     EBP
#     EFLAGS
#     return addr (caller of context_switch)
#     old_esp_ptr (arg 1)
#     new_esp     (arg 2)
.code32

.global context_switch
context_switch:
	pushf
	push %ebp
	push %ebx
	push %esi
	push %edi

	mov 24(%esp), %eax         # &old->esp
	mov %esp, (%eax)           # *old_esp_ptr = current ESP
	mov 28(%esp), %esp         # ESP = new_esp

	pop %edi
	pop %esi
	pop %ebx
	pop %ebp
	popf
	ret
