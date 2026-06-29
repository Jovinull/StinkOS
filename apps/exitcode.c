/* Validates that sys_exit_code(N) propagates through sys_wait() so a
 * parent process actually sees its child's chosen exit code.
 *
 * Output keys for tools/smoke-exitcode.py:
 *   "exitcode: child forked"
 *   "exitcode: wait returned 42"
 *   "exitcode: PASS" */
#include "libstink.h"

void main(void)
{
	int pid = sys_fork();
	if (pid < 0) {
		sys_log("exitcode: FAIL fork");
		return;
	}
	if (pid == 0) {
		/* Child path: exit with a known non-zero code. */
		sys_exit_code(42);
		/* unreachable */
		sys_log("exitcode: FAIL child past sys_exit_code");
		sys_exit();
	}
	sys_log("exitcode: child forked");

	int rc = sys_wait();
	sys_printf("exitcode: wait returned %d\n", rc);
	if (rc == 42)
		sys_log("exitcode: PASS");
	else
		sys_log("exitcode: FAIL wrong code");
}
