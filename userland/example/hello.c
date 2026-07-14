#include "neolibc.h"

void _start(void) {
    neo_printf("Hello from NEO-OS userland!\n");
    char buf[32];
    neo_strcpy(buf, "NEO ML");
    neo_printf("App name: %s\n", buf);
    void* p = neo_malloc(128);
    if (p) {
        neo_memset(p, 0, 128);
        neo_printf("malloc ok at %p\n", p);
    }
    int pid = neo_getpid();
    neo_printf("pid=%d\n", pid);
    neo_exit(0);
}
