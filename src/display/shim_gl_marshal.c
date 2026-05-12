#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>

#include "spi_shim.h"
#include "display/gl_proto.h"

static int             g_marshal_active   = -1;
static int             g_marshal_fd       = -1;
static pthread_mutex_t g_marshal_lock     = PTHREAD_MUTEX_INITIALIZER;

static int gl_marshal_connect_locked(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    size_t name_len = strlen(GL_PROTO_SOCKET_NAME);
    memcpy(addr.sun_path + 1, GL_PROTO_SOCKET_NAME, name_len);
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (connect(fd, (struct sockaddr*)&addr, addrlen) == 0) {
            return fd;
        }
        if (attempt < 2) {
            const struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    SHIM_LOG("[gl_marshal] connect(@%s) failed: %s — falling back to dlsym path",
             GL_PROTO_SOCKET_NAME, strerror(errno));
    close(fd);
    return -1;
}

int shim_gl_marshal_active(void) {
    if (g_marshal_active != -1) return g_marshal_active;
    pthread_mutex_lock(&g_marshal_lock);
    if (g_marshal_active == -1) {
        const char *env = getenv("SPIKE3_GL_DAEMON");
        if (!env || env[0] != '1') {
            g_marshal_active = 0;
        } else {
            g_marshal_fd = gl_marshal_connect_locked();
            if (g_marshal_fd < 0) {
                g_marshal_active = 0;
            } else {
                SHIM_LOG("[gl_marshal] connected to @%s on fd %d",
                         GL_PROTO_SOCKET_NAME, g_marshal_fd);
                g_marshal_active = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_marshal_lock);
    return g_marshal_active;
}

static int write_all_locked(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    while (len > 0) {
        ssize_t n = send(g_marshal_fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all_locked(void *buf, size_t len) {
    uint8_t *p = (uint8_t*)buf;
    while (len > 0) {
        ssize_t n = recv(g_marshal_fd, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void marshal_drop_connection_locked(const char *reason) {
    if (g_marshal_fd >= 0) {
        close(g_marshal_fd);
        g_marshal_fd = -1;
    }
    g_marshal_active = 0;
    SHIM_LOG("[gl_marshal] connection dropped (%s) — falling back to dlsym for remainder of run",
             reason ? reason : "unknown");
}

int shim_gl_marshal_send_async(uint16_t op, const void *args, uint32_t args_len) {
    if (!shim_gl_marshal_active()) return -1;
    struct gl_msg_header hdr = { .op = op, .flags = 0, .payload_len = args_len };
    pthread_mutex_lock(&g_marshal_lock);
    int rc = -1;
    if (g_marshal_fd >= 0) {
        if (write_all_locked(&hdr, sizeof(hdr)) == 0 &&
            (args_len == 0 || write_all_locked(args, args_len) == 0)) {
            rc = 0;
        } else {
            marshal_drop_connection_locked("write failed");
        }
    }
    pthread_mutex_unlock(&g_marshal_lock);
    return rc;
}

int shim_gl_marshal_send_sync(uint16_t op,
                               const void *args, uint32_t args_len,
                               void *reply_payload, uint32_t reply_max,
                               uint32_t *out_reply_len) {
    if (out_reply_len) *out_reply_len = 0;
    if (!shim_gl_marshal_active()) return -1;

    struct gl_msg_header hdr = { .op = op, .flags = GL_FLAG_SYNC, .payload_len = args_len };
    pthread_mutex_lock(&g_marshal_lock);
    int rc = -1;
    if (g_marshal_fd >= 0) {
        if (write_all_locked(&hdr, sizeof(hdr)) != 0 ||
            (args_len > 0 && write_all_locked(args, args_len) != 0)) {
            marshal_drop_connection_locked("sync write failed");
            goto out;
        }
        struct gl_msg_header reply;
        if (read_all_locked(&reply, sizeof(reply)) != 0) {
            marshal_drop_connection_locked("sync reply read failed");
            goto out;
        }
        uint32_t to_read = reply.payload_len;
        if (to_read > reply_max) to_read = reply_max;
        if (to_read > 0) {
            if (read_all_locked(reply_payload, to_read) != 0) {
                marshal_drop_connection_locked("sync reply payload read failed");
                goto out;
            }
        }
        if (reply.payload_len > to_read) {
            uint32_t remaining = reply.payload_len - to_read;
            uint8_t scratch[256];
            while (remaining > 0) {
                uint32_t chunk = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
                if (read_all_locked(scratch, chunk) != 0) {
                    marshal_drop_connection_locked("sync reply drain failed");
                    goto out;
                }
                remaining -= chunk;
            }
        }
        if (out_reply_len) *out_reply_len = to_read;
        rc = (int)reply.op;
    }
out:
    pthread_mutex_unlock(&g_marshal_lock);
    return rc;
}
