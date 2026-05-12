/* Socket logging hooks. Connections and name resolution pass through unchanged. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>

#include "spi_shim.h"

static int shim_offline_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *s = getenv("SPIKE3_OFFLINE");
        cached = (s && s[0] == '1') ? 1 : 0;
    }
    return cached;
}

/* True if the sockaddr is a "local" destination (loopback IPv4/IPv6,
 * AF_UNIX, or any non-INET family). Used purely for log labelling —
 * we never gate the connect() call on this. Conservative on bad args:
 * treat as local so the categorisation can't itself raise alarm. */
static int shim_addr_is_local(const struct sockaddr *addr,
                              socklen_t addrlen) {
    if (!addr) return 1;
    sa_family_t fam = addr->sa_family;

    if (fam == AF_INET) {
        if (addrlen < (socklen_t)sizeof(struct sockaddr_in)) return 1;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        uint32_t ip = ntohl(sin->sin_addr.s_addr);
        return (ip >> 24) == 127;
    }
    if (fam == AF_INET6) {
        if (addrlen < (socklen_t)sizeof(struct sockaddr_in6)) return 1;
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        const uint8_t *b = sin6->sin6_addr.s6_addr;
        int loopback6 = 1;
        for (int i = 0; i < 15; i++) if (b[i]) { loopback6 = 0; break; }
        if (loopback6 && b[15] == 1) return 1;
        /* IPv4-mapped loopback: ::ffff:127.x.x.x */
        if (b[10] == 0xff && b[11] == 0xff && b[12] == 127) {
            int v4mapped_prefix = 1;
            for (int i = 0; i < 10; i++) if (b[i]) { v4mapped_prefix = 0; break; }
            if (v4mapped_prefix) return 1;
        }
        return 0;
    }
    return 1;
}

static void shim_addr_to_str(const struct sockaddr *addr, socklen_t addrlen,
                             char *buf, size_t buflen) {
    if (!addr) {
        snprintf(buf, buflen, "<null>");
        return;
    }
    sa_family_t fam = addr->sa_family;
    if (fam == AF_INET && addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        char ipstr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sin->sin_addr, ipstr, sizeof(ipstr));
        snprintf(buf, buflen, "%s:%u", ipstr, ntohs(sin->sin_port));
        return;
    }
    if (fam == AF_INET6 && addrlen >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        char ipstr[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &sin6->sin6_addr, ipstr, sizeof(ipstr));
        snprintf(buf, buflen, "[%s]:%u", ipstr, ntohs(sin6->sin6_port));
        return;
    }
    if (fam == AF_UNIX) {
        const struct sockaddr_un *sun = (const struct sockaddr_un *)addr;
        snprintf(buf, buflen, "AF_UNIX:%.*s",
                 (int)(buflen > 16 ? buflen - 16 : 0), sun->sun_path);
        return;
    }
    snprintf(buf, buflen, "AF_%u(unsupported)", (unsigned)fam);
}

typedef int (*real_connect_t)(int, const struct sockaddr *, socklen_t);
static real_connect_t shim_real_connect = NULL;

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!shim_real_connect)
        shim_real_connect = (real_connect_t)dlsym(RTLD_NEXT, "connect");

    if (!shim_offline_enabled()) {
        if (shim_real_connect) return shim_real_connect(sockfd, addr, addrlen);
        errno = ENOSYS; return -1;
    }

    char dst[160];
    shim_addr_to_str(addr, addrlen, dst, sizeof(dst));
    int local = shim_addr_is_local(addr, addrlen);
    SHIM_LOG("[net] connect fd=%d -> %s [%s]",
             sockfd, dst, local ? "local" : "external");
    if (shim_real_connect) return shim_real_connect(sockfd, addr, addrlen);
    errno = ENOSYS; return -1;
}

typedef int (*real_getaddrinfo_t)(const char *, const char *,
                                  const struct addrinfo *,
                                  struct addrinfo **);
static real_getaddrinfo_t shim_real_getaddrinfo = NULL;

/* True if `node` resolves to a loopback target by syntax alone.
 * Used only for log labelling — never gates the call. */
static int shim_node_is_local(const char *node) {
    if (!node) return 1;
    if (strcmp(node, "localhost") == 0 ||
        strcmp(node, "ip6-localhost") == 0 ||
        strcmp(node, "ip6-loopback") == 0) return 1;
    for (const char *p = node; *p; p++) {
        char c = *p;
        int ok = (c >= '0' && c <= '9') ||
                 (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F') ||
                 c == ':' || c == '.' || c == '%';
        if (!ok) return 0;
    }
    return 1;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!shim_real_getaddrinfo)
        shim_real_getaddrinfo = (real_getaddrinfo_t)
            dlsym(RTLD_NEXT, "getaddrinfo");

    if (!shim_offline_enabled()) {
        if (shim_real_getaddrinfo)
            return shim_real_getaddrinfo(node, service, hints, res);
        return EAI_SYSTEM;
    }

    int local = shim_node_is_local(node);
    SHIM_LOG("[net] getaddrinfo node=%s service=%s [%s]",
             node ? node : "<null>",
             service ? service : "<null>",
             local ? "local" : "external");

    if (shim_real_getaddrinfo)
        return shim_real_getaddrinfo(node, service, hints, res);
    return EAI_SYSTEM;
}
