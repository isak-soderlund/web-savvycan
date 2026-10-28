// LD_PRELOAD shim for screenshot runs: SavvyCAN's "Network connection (GVRET)" always connects to
// TCP port 23 (a privileged port); this sends connect() to port 23 to $WSC_PORT23 (default 2323),
// where the fake rc1 (tests/desktop/rc1_pty.py --tcp) listens. No system change needed.
//   gcc -shared -fPIC -O1 port23_redirect.c -o port23_redirect.so -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real_connect)(int, const struct sockaddr *, socklen_t);
    if (!real_connect) real_connect = dlsym(RTLD_NEXT, "connect");
    if (addr && addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in in = *(const struct sockaddr_in *)addr;
        if (ntohs(in.sin_port) == 23) {
            const char *p = getenv("WSC_PORT23");
            in.sin_port = htons(p ? atoi(p) : 2323);
            return real_connect(fd, (const struct sockaddr *)&in, sizeof in);
        }
    }
    return real_connect(fd, addr, len);
}
