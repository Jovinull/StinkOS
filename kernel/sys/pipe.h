/* Anonymous pipes -- byte-stream IPC primitive between kernel threads.
 *
 * Each pipe owns a 4 KiB circular buffer and tracks two endpoints: read and
 * write. Endpoints are exposed to userland through "pipe handles" that pack
 * the pipe index and end bit into one int (handle = (idx << 1) | end_bit,
 * end_bit 0=read 1=write), so a single SYS_PIPE_CLOSE can release either end
 * without a second arg.
 *
 * Blocking model: read on empty blocks until a writer pushes bytes or every
 * write end is closed (returns 0 = EOF); write on full blocks until a reader
 * drains or every read end is closed (returns -1 = EPIPE).
 */
#ifndef PIPE_H
#define PIPE_H

#define PIPE_MAX     8           /* number of concurrent pipes */
#define PIPE_BUFSZ   4096        /* per-pipe circular buffer capacity */

/* Allocate a new pipe and return read/write handles via *rd_handle / *wr_handle.
 * Returns 0 on success, -1 if the pipe table is full. */
int  pipe_alloc(int *rd_handle, int *wr_handle);

/* Close one endpoint. The pipe is fully released only after both ends are
 * closed. Returns 0 on success, -1 if the handle is invalid. */
int  pipe_close(int handle);

/* Read up to `n` bytes into `dst`. Blocks until bytes are available or all
 * writers have closed. Returns the byte count actually read (0 = EOF), or -1
 * on a bad handle (not a read end, unknown index). */
int  pipe_read(int handle, void *dst, unsigned int n);

/* Write `n` bytes from `src`. Blocks until space is available or all readers
 * have closed (returns -1 = EPIPE in that case). Returns the byte count
 * actually written, or -1 on a bad handle. */
int  pipe_write(int handle, const void *src, unsigned int n);

#endif
