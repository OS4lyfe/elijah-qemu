#ifndef QEMU_CLOUDLET_H
#define QEMU_CLOUDLET_H

#include "qemu-common.h"

int cloudlet_init(const char *logfile_path);
int cloudlet_end(void);

int printlog(bool flush, const char* format, ...);

enum cloudlet_raw {
    CLOUDLET_RAW_OFF = 0,
    CLOUDLET_RAW_SUSPEND,
    CLOUDLET_RAW_LIVE
};

extern enum cloudlet_raw cloudlet_raw_mode;

#endif /* QEMU_CLOUDLET_H */
