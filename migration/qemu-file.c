/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <zlib.h>
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/sockets.h"
#include "qemu/coroutine.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/qemu-file-internal.h"
#include "trace.h"

/*
 * Stop a file from being read/written - not all backing files can do this
 * typically only sockets can.
 */
int qemu_file_shutdown(QEMUFile *f)
{
    if (!f->ops->shut_down) {
        return -ENOSYS;
    }
    return f->ops->shut_down(f->opaque, true, true);
}

/*
 * Result: QEMUFile* for a 'return path' for comms in the opposite direction
 *         NULL if not available
 */
QEMUFile *qemu_file_get_return_path(QEMUFile *f)
{
    if (!f->ops->get_return_path) {
        return NULL;
    }
    return f->ops->get_return_path(f->opaque);
}

bool qemu_file_mode_is_not_valid(const char *mode)
{
    if (mode == NULL ||
        (mode[0] != 'r' && mode[0] != 'w') ||
        mode[1] != 'b' || mode[2] != 0) {
        fprintf(stderr, "qemu_fopen: Argument validity check failed\n");
        return true;
    }

    return false;
}

QEMUFile *qemu_fopen_ops(void *opaque, const QEMUFileOps *ops)
{
    QEMUFile *f;

    f = g_new0(QEMUFile, 1);

    f->opaque = opaque;
    f->ops = ops;
    f->use_blob = false;
    f->blob_pos = 0;
    f->blob_file_size = 0;
    f->iter_seq = 0;
    qemu_mutex_init(&f->raw_live_state_lock);
    qemu_cond_init(&f->raw_live_state_cv);
    f->raw_live_stop_requested = false;
    f->raw_live_iterate_requested = false;
    return f;
}

void qemu_file_enable_blob(QEMUFile *f)
{
    f->use_blob = true;
}

bool qemu_file_blob_enabled(QEMUFile *f)
{
    return f->use_blob;
}


/*
 * Get last error for stream f
 *
 * Return negative error value if there has been an error on previous
 * operations, return 0 if no error happened.
 *
 */
int qemu_file_get_error(QEMUFile *f)
{
    return f->last_error;
}

void qemu_file_set_error(QEMUFile *f, int ret)
{
    if (f->last_error == 0) {
        f->last_error = ret;
    }
}

bool qemu_file_is_writable(QEMUFile *f)
{
    return f->ops->writev_buffer || f->ops->put_buffer;
}

/**
 * Flushes QEMUFile buffer
 *
 * If there is writev_buffer QEMUFileOps it uses it otherwise uses
 * put_buffer ops.
 */
void qemu_fflush(QEMUFile *f)
{
    ssize_t ret = 0;

    if (!qemu_file_is_writable(f)) {
        return;
    }

    if (f->ops->writev_buffer) {
        if (f->iovcnt > 0) {
            ret = f->ops->writev_buffer(f->opaque, f->iov, f->iovcnt, f->pos);
        }
    } else {
        if (f->buf_index > 0) {
            ret = f->ops->put_buffer(f->opaque, f->buf, f->pos, f->buf_index);
        }
    }
    if (ret >= 0) {
        f->pos += ret;
    }
    f->buf_index = 0;
    f->iovcnt = 0;
    if (ret < 0) {
        qemu_file_set_error(f, ret);
    }
}

void ram_control_before_iterate(QEMUFile *f, uint64_t flags)
{
    int ret = 0;

    if (f->ops->before_ram_iterate) {
        ret = f->ops->before_ram_iterate(f, f->opaque, flags, NULL);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
    }
}

void ram_control_after_iterate(QEMUFile *f, uint64_t flags)
{
    int ret = 0;

    if (f->ops->after_ram_iterate) {
        ret = f->ops->after_ram_iterate(f, f->opaque, flags, NULL);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
    }
}

void ram_control_load_hook(QEMUFile *f, uint64_t flags, void *data)
{
    int ret = -EINVAL;

    if (f->ops->hook_ram_load) {
        ret = f->ops->hook_ram_load(f, f->opaque, flags, data);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
    } else {
        /*
         * Hook is a hook specifically requested by the source sending a flag
         * that expects there to be a hook on the destination.
         */
        if (flags == RAM_CONTROL_HOOK) {
            qemu_file_set_error(f, ret);
        }
    }
}

size_t ram_control_save_page(QEMUFile *f, ram_addr_t block_offset,
                             ram_addr_t offset, size_t size,
                             uint64_t *bytes_sent)
{
    if (f->ops->save_page) {
        int ret = f->ops->save_page(f, f->opaque, block_offset,
                                    offset, size, bytes_sent);

        if (ret != RAM_SAVE_CONTROL_DELAYED) {
            if (bytes_sent && *bytes_sent > 0) {
                qemu_update_position(f, *bytes_sent);
            } else if (ret < 0) {
                qemu_file_set_error(f, ret);
            }
        }

        return ret;
    }

    return RAM_SAVE_CONTROL_NOT_SUPP;
}

/*
 * Attempt to fill the buffer from the underlying file
 * Returns the number of bytes read, or negative value for an error.
 *
 * Note that it can return a partially full buffer even in a not error/not EOF
 * case if the underlying file descriptor gives a short read, and that can
 * happen even on a blocking fd.
 */
static ssize_t qemu_fill_buffer(QEMUFile *f)
{
    int len;
    int pending;

    assert(!qemu_file_is_writable(f));

    pending = f->buf_size - f->buf_index;
    if (pending > 0) {
        memmove(f->buf, f->buf + f->buf_index, pending);
    }
    f->buf_index = 0;
    f->buf_size = pending;

    len = f->ops->get_buffer(f->opaque, f->buf + pending, f->pos,
                        IO_BUF_SIZE - pending);
    if (len > 0) {
        f->buf_size += len;
        f->pos += len;
    } else if (len == 0) {
        qemu_file_set_error(f, -EIO);
    } else if (len != -EAGAIN) {
        qemu_file_set_error(f, len);
    }

    return len;
}

int qemu_get_fd(QEMUFile *f)
{
    if (f->ops->get_fd) {
        return f->ops->get_fd(f->opaque);
    }
    return -1;
}

void qemu_update_position(QEMUFile *f, size_t size)
{
    f->pos += size;
}

/** Closes the file
 *
 * Returns negative error value if any error happened on previous operations or
 * while closing the file. Returns 0 or positive number on success.
 *
 * The meaning of return value on success depends on the specific backend
 * being used.
 */
int qemu_fclose(QEMUFile *f)
{
    int ret;
    qemu_fflush(f);
    ret = qemu_file_get_error(f);

    qemu_mutex_destroy(&f->raw_live_state_lock);
    qemu_cond_destroy(&f->raw_live_state_cv);

    if (f->ops->close) {
        int ret2 = f->ops->close(f->opaque);
        if (ret >= 0) {
            ret = ret2;
        }
    }
    /* If any error was spotted before closing, we should report it
     * instead of the close() return value.
     */
    if (f->last_error) {
        ret = f->last_error;
    }
    g_free(f);
    trace_qemu_file_fclose();
    return ret;
}

static void add_to_iovec(QEMUFile *f, const uint8_t *buf, size_t size)
{
    /* check for adjacent buffer and coalesce them */
    if (f->iovcnt > 0 && buf == f->iov[f->iovcnt - 1].iov_base +
        f->iov[f->iovcnt - 1].iov_len) {
        f->iov[f->iovcnt - 1].iov_len += size;
    } else {
        f->iov[f->iovcnt].iov_base = (uint8_t *)buf;
        f->iov[f->iovcnt++].iov_len = size;
    }

    if (f->iovcnt >= MAX_IOV_SIZE) {
        qemu_fflush(f);
    }
}

void qemu_put_buffer_async(QEMUFile *f, const uint8_t *buf, size_t size)
{
    if (!f->ops->writev_buffer) {
        qemu_put_buffer(f, buf, size);
        return;
    }

    if (f->last_error) {
        return;
    }

    f->bytes_xfer += size;
    add_to_iovec(f, buf, size);
}

void qemu_put_migration_size(QEMUFile *f, uint64_t num_size)
{
    // no read buffer checking done, and assumes buf->index == 0 etc.
    // i.e., we are the first and sole writer to this QEMUFile
    uint64_t *header;
    header = (uint64_t*)(f->buf + f->buf_index);
    *header = num_size;
    if (f->ops->writev_buffer) {
        add_to_iovec(f, f->buf + f->buf_index, sizeof(uint64_t));
    }
    f->buf_index += sizeof(uint64_t);
    qemu_fflush(f);
}

void qemu_put_buffer(QEMUFile *f, const uint8_t *buf, size_t size)
{
    size_t l;

    if (f->last_error) {
        return;
    }

    while (size > 0) {
	if (f->use_blob && (f->blob_pos % BLOB_SIZE) == 0) {
	    blob_off_t *header = NULL;

	    if (sizeof(blob_off_t) > IO_BUF_SIZE - f->buf_index)
		qemu_fflush(f);

	    if (f->blob_pos & ~BLOB_POS_MASK) {
		fprintf(stderr, "blob offset %" PRIu64 " exceeds limit\n",
			f->blob_pos);
		abort();
	    }

	    header = (blob_off_t*)(f->buf + f->buf_index);
	    *header = f->blob_pos | (f->iter_seq << ITER_SEQ_SHIFT);
            if (f->ops->writev_buffer) {
                add_to_iovec(f, f->buf + f->buf_index, sizeof(blob_off_t));
            }
	    f->buf_index += sizeof(blob_off_t);

	    if (f->buf_index >= IO_BUF_SIZE)
		qemu_fflush(f);
	}
        l = IO_BUF_SIZE - f->buf_index;
        if (l > size) {
            l = size;
        }
	if (f->use_blob && l > (BLOB_SIZE - (f->blob_pos % BLOB_SIZE))) {
	    l = (BLOB_SIZE - (f->blob_pos % BLOB_SIZE));
        }

        memcpy(f->buf + f->buf_index, buf, l);
        f->bytes_xfer += l;
        if (f->ops->writev_buffer) {
            add_to_iovec(f, f->buf + f->buf_index, l);
        }
        f->buf_index += l;
	if (f->use_blob)
	    f->blob_pos += l;
        if (f->buf_index >= IO_BUF_SIZE) {
            qemu_fflush(f);
        }
        if (qemu_file_get_error(f)) {
            break;
        }
        buf += l;
        size -= l;
    }
}

void qemu_put_byte(QEMUFile *f, int v)
{
    if (f->last_error) {
        return;
    }

    if (f->use_blob && (f->blob_pos % BLOB_SIZE) == 0) {
	blob_off_t *header = NULL;
	if (sizeof(blob_off_t) > IO_BUF_SIZE - f->buf_index)
	    qemu_fflush(f);
	if (f->blob_pos & ~BLOB_POS_MASK) {
	    fprintf(stderr, "blob offset %" PRIu64 " exceeds limit\n",
		    f->blob_pos);
	    abort();
	}

	header = (blob_off_t*)(f->buf + f->buf_index);
	*header = f->blob_pos | (f->iter_seq << ITER_SEQ_SHIFT);
        if (f->ops->writev_buffer) {
            add_to_iovec(f, f->buf + f->buf_index, sizeof(blob_off_t));
        }
	f->buf_index += sizeof(blob_off_t);

        if (f->buf_index >= IO_BUF_SIZE)
            qemu_fflush(f);
    }

    f->buf[f->buf_index] = v;
    f->bytes_xfer++;
    if (f->ops->writev_buffer) {
        add_to_iovec(f, f->buf + f->buf_index, 1);
    }
    f->buf_index++;
    if (f->use_blob) {
	f->blob_pos += sizeof(*f->buf);
    }
    if (f->buf_index == IO_BUF_SIZE) {
        qemu_fflush(f);
    }
}

/* Needs to be used with BLOB_SIZE-aligned pos */
void set_blob_pos(QEMUFile *f, uint64_t pos)
{
    void *padding = NULL;

    if (!f->use_blob)
	return;

    if ((f->blob_pos % BLOB_SIZE) != 0) {
	padding = g_malloc0(BLOB_SIZE - (f->blob_pos % BLOB_SIZE));
	qemu_put_buffer(f, padding, BLOB_SIZE - (f->blob_pos % BLOB_SIZE));
	qemu_fflush(f);
	g_free(padding);
    }

    f->blob_pos = pos;
}

uint64_t get_blob_pos(struct QEMUFile *f)
{
    return (uint64_t)f->blob_pos;
}

void reset_iter_seq(struct QEMUFile *f)
{
    f->iter_seq = 0;
}

void inc_iter_seq(struct QEMUFile *f)
{
    f->iter_seq++;
}


void qemu_file_skip(QEMUFile *f, int size)
{
    if (f->buf_index + size <= f->buf_size) {
        f->buf_index += size;
    }
}

/*
 * Read 'size' bytes from file (at 'offset') without moving the
 * pointer and set 'buf' to point to that data.
 *
 * It will return size bytes unless there was an error, in which case it will
 * return as many as it managed to read (assuming blocking fd's which
 * all current QEMUFile are)
 */
size_t qemu_peek_buffer(QEMUFile *f, uint8_t **buf, size_t size, size_t offset)
{
    ssize_t pending;
    size_t index;

    assert(!qemu_file_is_writable(f));
    assert(offset < IO_BUF_SIZE);
    assert(size <= IO_BUF_SIZE - offset);

    /* The 1st byte to read from */
    index = f->buf_index + offset;
    /* The number of available bytes starting at index */
    pending = f->buf_size - index;

    /*
     * qemu_fill_buffer might return just a few bytes, even when there isn't
     * an error, so loop collecting them until we get enough.
     */
    while (pending < size) {
        int received = qemu_fill_buffer(f);

        if (received <= 0) {
            break;
        }

        index = f->buf_index + offset;
        pending = f->buf_size - index;
    }

    if (pending <= 0) {
        return 0;
    }
    if (size > pending) {
        size = pending;
    }

    *buf = f->buf + index;
    return size;
}

/*
 * Read 'size' bytes of data from the file into buf.
 * 'size' can be larger than the internal buffer.
 *
 * It will return size bytes unless there was an error, in which case it will
 * return as many as it managed to read (assuming blocking fd's which
 * all current QEMUFile are)
 */
size_t qemu_get_buffer(QEMUFile *f, uint8_t *buf, size_t size)
{
    size_t pending = size;
    size_t done = 0;

    while (pending > 0) {
        size_t res;
        uint8_t *src;

        res = qemu_peek_buffer(f, &src, MIN(pending, IO_BUF_SIZE), 0);
        if (res == 0) {
            return done;
        }
        memcpy(buf, src, res);
        qemu_file_skip(f, res);
        buf += res;
        pending -= res;
        done += res;
    }
    if (f->use_blob) {
        f->blob_pos += size;
    }
    return done;
}

/*
 * Read 'size' bytes of data from the file.
 * 'size' can be larger than the internal buffer.
 *
 * The data:
 *   may be held on an internal buffer (in which case *buf is updated
 *     to point to it) that is valid until the next qemu_file operation.
 * OR
 *   will be copied to the *buf that was passed in.
 *
 * The code tries to avoid the copy if possible.
 *
 * It will return size bytes unless there was an error, in which case it will
 * return as many as it managed to read (assuming blocking fd's which
 * all current QEMUFile are)
 *
 * Note: Since **buf may get changed, the caller should take care to
 *       keep a pointer to the original buffer if it needs to deallocate it.
 */
size_t qemu_get_buffer_in_place(QEMUFile *f, uint8_t **buf, size_t size)
{
    if (size < IO_BUF_SIZE) {
        size_t res;
        uint8_t *src;

        res = qemu_peek_buffer(f, &src, size, 0);

        if (res == size) {
            qemu_file_skip(f, res);
            *buf = src;
            return res;
        }
    }

    return qemu_get_buffer(f, *buf, size);
}

/*
 * Peeks a single byte from the buffer; this isn't guaranteed to work if
 * offset leaves a gap after the previous read/peeked data.
 */
int qemu_peek_byte(QEMUFile *f, int offset)
{
    int index = f->buf_index + offset;

    assert(!qemu_file_is_writable(f));
    assert(offset < IO_BUF_SIZE);

    if (index >= f->buf_size) {
        qemu_fill_buffer(f);
        index = f->buf_index + offset;
        if (index >= f->buf_size) {
            return 0;
        }
    }
    return f->buf[index];
}

int qemu_get_byte(QEMUFile *f)
{
    int result;

    result = qemu_peek_byte(f, 0);
    qemu_file_skip(f, 1);
    if (f->use_blob) {
        f->blob_pos += 1;
    }
    return result;
}

int64_t qemu_ftell_fast(QEMUFile *f)
{
    int64_t ret = f->pos;
    int i;

    if (f->ops->writev_buffer) {
        for (i = 0; i < f->iovcnt; i++) {
            ret += f->iov[i].iov_len;
        }
    } else {
        ret += f->buf_index;
    }

    return ret;
}

int64_t qemu_ftell(QEMUFile *f)
{
    qemu_fflush(f);
    return f->pos;
}

int64_t qemu_ftell_read(QEMUFile *f)
{
    return f->pos - f->buf_size + f->buf_index;
}

int64_t qemu_fseek(QEMUFile *f, int64_t pos, int whence)
{
    blob_off_t blob_pos = pos;

    if (whence == SEEK_SET) {
        /* nothing to do */
    } else if (whence == SEEK_CUR) {
	if (f->use_blob) {
	    blob_pos = f->blob_pos + pos;
        }
        if (use_raw_live(f) || use_raw_suspend(f)) {
            pos += qemu_ftell_read(f);
        } else {
            pos += qemu_ftell(f);
        }
    } else {
        /* SEEK_END not supported */
        return -1;
    }
    if (f->ops->put_buffer) {
        qemu_fflush(f);
        f->pos = pos;
    } else {
        f->pos = pos;
        f->buf_index = 0;
        f->buf_size = 0;
    }
    if (f->use_blob)
	f->blob_pos = blob_pos;

    return pos;
}

int qemu_file_rate_limit(QEMUFile *f)
{
    if (qemu_file_get_error(f)) {
        return 1;
    }
    if (f->xfer_limit > 0 && f->bytes_xfer > f->xfer_limit) {
        return 1;
    }
    return 0;
}

int64_t qemu_file_get_rate_limit(QEMUFile *f)
{
    return f->xfer_limit;
}

void qemu_file_set_rate_limit(QEMUFile *f, int64_t limit)
{
    f->xfer_limit = limit;
}

void qemu_file_reset_rate_limit(QEMUFile *f)
{
    f->bytes_xfer = 0;
}

void qemu_put_be16(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be32(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 24);
    qemu_put_byte(f, v >> 16);
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be64(QEMUFile *f, uint64_t v)
{
    qemu_put_be32(f, v >> 32);
    qemu_put_be32(f, v);
}

unsigned int qemu_get_be16(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

unsigned int qemu_get_be32(QEMUFile *f)
{
    unsigned int v;
    v = (unsigned int)qemu_get_byte(f) << 24;
    v |= qemu_get_byte(f) << 16;
    v |= qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

uint64_t qemu_get_be64(QEMUFile *f)
{
    uint64_t v;
    v = (uint64_t)qemu_get_be32(f) << 32;
    v |= qemu_get_be32(f);
    return v;
}

/* compress size bytes of data start at p with specific compression
 * level and store the compressed data to the buffer of f.
 */

ssize_t qemu_put_compression_data(QEMUFile *f, const uint8_t *p, size_t size,
                                  int level)
{
    ssize_t blen = IO_BUF_SIZE - f->buf_index - sizeof(int32_t);

    if (blen < compressBound(size)) {
        return 0;
    }
    if (compress2(f->buf + f->buf_index + sizeof(int32_t), (uLongf *)&blen,
                  (Bytef *)p, size, level) != Z_OK) {
        error_report("Compress Failed!");
        return 0;
    }
    qemu_put_be32(f, blen);
    f->buf_index += blen;
    return blen + sizeof(int32_t);
}

/* Put the data in the buffer of f_src to the buffer of f_des, and
 * then reset the buf_index of f_src to 0.
 */

int qemu_put_qemu_file(QEMUFile *f_des, QEMUFile *f_src)
{
    int len = 0;

    if (f_src->buf_index > 0) {
        len = f_src->buf_index;
        qemu_put_buffer(f_des, f_src->buf, f_src->buf_index);
        f_src->buf_index = 0;
    }
    return len;
}

/*
 * Get a string whose length is determined by a single preceding byte
 * A preallocated 256 byte buffer must be passed in.
 * Returns: len on success and a 0 terminated string in the buffer
 *          else 0
 *          (Note a 0 length string will return 0 either way)
 */
size_t qemu_get_counted_string(QEMUFile *f, char buf[256])
{
    size_t len = qemu_get_byte(f);
    size_t res = qemu_get_buffer(f, (uint8_t *)buf, len);

    buf[res] = 0;

    return res == len ? res : 0;
}

/*
 * Set the blocking state of the QEMUFile.
 * Note: On some transports the OS only keeps a single blocking state for
 *       both directions, and thus changing the blocking on the main
 *       QEMUFile can also affect the return path.
 */
void qemu_file_set_blocking(QEMUFile *f, bool block)
{
    if (block) {
        qemu_set_block(qemu_get_fd(f));
    } else {
        qemu_set_nonblock(qemu_get_fd(f));
    }
}
