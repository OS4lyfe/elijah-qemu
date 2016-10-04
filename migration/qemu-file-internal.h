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

#ifndef QEMU_FILE_INTERNAL_H
#define QEMU_FILE_INTERNAL_H 1

#include "qemu-common.h"
#include "qemu/iov.h"
#include "migration/migration.h"

#define IO_BUF_SIZE 32768
#define MAX_IOV_SIZE MIN(IOV_MAX, 64)

#define BLOB_SIZE 4096
typedef uint64_t blob_off_t;
#define ITER_SEQ_BITS 16
#define ITER_SEQ_SHIFT (sizeof(blob_off_t) * 8 - ITER_SEQ_BITS)
#define BLOB_POS_MASK  ((((blob_off_t)1) << ITER_SEQ_SHIFT) - 1)

struct QEMUFile {
    const QEMUFileOps *ops;
    void *opaque;

    int64_t bytes_xfer;
    int64_t xfer_limit;

    int64_t pos; /* start of buffer when writing, end of buffer
                    when reading */
    int buf_index;
    int buf_size; /* 0 when writing */
    uint8_t buf[IO_BUF_SIZE];

    struct iovec iov[MAX_IOV_SIZE];
    unsigned int iovcnt;

    int last_error;
    raw_type use_raw;

    bool       use_blob;
    blob_off_t blob_pos;
    blob_off_t blob_file_size;  /* first 8-byte header has this reported blob file size */
    uint64_t   iter_seq;

    QemuMutex raw_live_state_lock;
    QemuCond raw_live_state_cv;
    bool raw_live_stop_requested;     /* protected by raw_live_state_lock */
    bool raw_live_iterate_requested;  /* protected by raw_live_state_lock */
};

#endif
