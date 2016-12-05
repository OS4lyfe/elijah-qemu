/*
 * QEMU live migration via generic fd
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Authors:
 *  Chris Lalancette <clalance@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
#include "qemu-common.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "migration/migration.h"
#include "monitor/monitor.h"
#include "migration/qemu-file.h"
#include "block/block.h"
#include "include/migration/migration.h"

#define DEBUG_MIGRATION_RAW

#ifdef DEBUG_MIGRATION_RAW
#define DPRINTF(fmt, ...) \
    do { printf("migration-raw: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif


void raw_start_outgoing_migration(MigrationState *s, const char *fdname, raw_type type, Error **errp)
{
    // for already created file
    int fd = monitor_get_fd(cur_mon, fdname, errp);
    if (fd == -1) {
        fd = open(fdname, O_CREAT | O_WRONLY | O_TRUNC, 00644);
        if (fd == -1) {
            DPRINTF("raw_migration: failed to open file\n");
            return;
        }
    }
    s->file = qemu_fdopen(fd, "wb");
    set_use_raw(s->file, type);
    if (use_raw_live(s->file))
	qemu_file_enable_blob(s->file);

    migrate_fd_connect(s);
}

static void raw_accept_incoming_migration(void *opaque)
{
    QEMUFile *f = opaque;

    qemu_set_fd_handler(qemu_get_fd(f), NULL, NULL, NULL);
    process_incoming_migration(f);
}

void raw_start_incoming_migration(const char *infd, raw_type type, Error **errp)
{
    int fd;
    int val;
    QEMUFile *f;

    val = strtol(infd, NULL, 0);
    if ((errno == ERANGE && (val == INT_MAX|| val == INT_MIN)) || (val == 0)) {
        DPRINTF("Attempting to start an incoming migration via raw\n");
        fd = open(infd, O_RDONLY);
    } else {
        fd = val;
    }

    f = qemu_fdopen(fd, "rb");
    if(f == NULL) {
        return;
    }

    // read ahead external header file, e.g. libvirt header
    // to have mmap file for memory
    long start_offset = lseek(fd, 0, SEEK_CUR);
    qemu_fseek(f, start_offset, SEEK_CUR);

    set_use_raw(f, type);

    qemu_set_fd_handler(fd, raw_accept_incoming_migration, NULL, f);
}

// copied from migration.c
enum {
    MIG_STATE_ERROR,
    MIG_STATE_SETUP,
    MIG_STATE_CANCELLED,
    MIG_STATE_ACTIVE,
    MIG_STATE_COMPLETED,
};

uint64_t raw_dump_device_state(bool suspend, bool print)
{
    MigrationState _s, *s = &_s;
    char fname[64];
    uint64_t num_pages = 0;

    strcpy(fname, "/tmp/qemu.XXXXXX");
    s->fd = mkostemp(fname, O_CREAT | O_WRONLY | O_TRUNC);

    if (s->fd == -1) {
	DPRINTF("raw_migration: failed to open file\n");
	goto err_after_get_fd;
    }

    s->state = MIG_STATE_ACTIVE;
    s->file = qemu_fdopen(s->fd, "wb");

    num_pages = qemu_savevm_dump_non_live(s->file, suspend, print);

    qemu_fclose(s->file);
    unlink(fname);

    return num_pages*1.1;

// err_after_open:
    close(s->fd);
err_after_get_fd:
    return 0;  // returns 0 pages on error
}
