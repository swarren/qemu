/*
 * NBD Internal Declarations
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef NBD_INTERNAL_H
#define NBD_INTERNAL_H
#include "block/nbd.h"
#include "sysemu/block-backend.h"

#include "qemu/coroutine.h"

#include <errno.h>
#include <string.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#if defined(__sun__) || defined(__HAIKU__)
#include <sys/ioccom.h>
#endif
#include <ctype.h>
#include <inttypes.h>

#ifdef __linux__
#include <linux/fs.h>
#endif

#include "qemu/sockets.h"
#include "qemu/queue.h"
#include "qemu/main-loop.h"

/* #define DEBUG_NBD */

#ifdef DEBUG_NBD
#define TRACE(msg, ...) do { \
    LOG(msg, ## __VA_ARGS__); \
} while(0)
#else
#define TRACE(msg, ...) \
    do { } while (0)
#endif

#define LOG(msg, ...) do { \
    fprintf(stderr, "%s:%s():L%d: " msg "\n", \
            __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__); \
} while(0)

/* This is all part of the "official" NBD API.
 *
 * The most up-to-date documentation is available at:
 * https://github.com/yoe/nbd/blob/master/doc/proto.txt
 */

#define NBD_REQUEST_SIZE        (4 + 4 + 8 + 8 + 4)
#define NBD_REPLY_SIZE          (4 + 4 + 8)
#define NBD_REQUEST_MAGIC       0x25609513
#define NBD_REPLY_MAGIC         0x67446698
#define NBD_OPTS_MAGIC          0x49484156454F5054LL
#define NBD_CLIENT_MAGIC        0x0000420281861253LL
#define NBD_REP_MAGIC           0x3e889045565a9LL

#define NBD_SET_SOCK            _IO(0xab, 0)
#define NBD_SET_BLKSIZE         _IO(0xab, 1)
#define NBD_SET_SIZE            _IO(0xab, 2)
#define NBD_DO_IT               _IO(0xab, 3)
#define NBD_CLEAR_SOCK          _IO(0xab, 4)
#define NBD_CLEAR_QUE           _IO(0xab, 5)
#define NBD_PRINT_DEBUG         _IO(0xab, 6)
#define NBD_SET_SIZE_BLOCKS     _IO(0xab, 7)
#define NBD_DISCONNECT          _IO(0xab, 8)
#define NBD_SET_TIMEOUT         _IO(0xab, 9)
#define NBD_SET_FLAGS           _IO(0xab, 10)

#define NBD_OPT_EXPORT_NAME     (1)
#define NBD_OPT_ABORT           (2)
#define NBD_OPT_LIST            (3)

/* NBD errors are based on errno numbers, so there is a 1:1 mapping,
 * but only a limited set of errno values is specified in the protocol.
 * Everything else is squashed to EINVAL.
 */
#define NBD_SUCCESS    0
#define NBD_EPERM      1
#define NBD_EIO        5
#define NBD_ENOMEM     12
#define NBD_EINVAL     22
#define NBD_ENOSPC     28

static inline ssize_t read_sync(int fd, void *buffer, size_t size)
{
    /* Sockets are kept in blocking mode in the negotiation phase.  After
     * that, a non-readable socket simply means that another thread stole
     * our request/reply.  Synchronization is done with recv_coroutine, so
     * that this is coroutine-safe.
     */
    return nbd_wr_sync(fd, buffer, size, true);
}

static inline ssize_t write_sync(int fd, void *buffer, size_t size)
{
    int ret;
    do {
        /* For writes, we do expect the socket to be writable.  */
        ret = nbd_wr_sync(fd, buffer, size, false);
    } while (ret == -EAGAIN);
    return ret;
}

#endif
