#include "sys/utsname.h"
#include "syscall.h"
#include "string.h"

int uname(struct utsname *buf) {
    if (!buf) return -1;
    struct uname_buf k;
    if (sys_uname(&k) < 0) return -1;
    strncpy(buf->sysname, k.sysname, 64);  buf->sysname[64]  = 0;
    strncpy(buf->nodename, "helium", 64);  buf->nodename[64] = 0;
    strncpy(buf->release, k.release, 64);  buf->release[64]  = 0;
    strncpy(buf->version, "0.1", 64);      buf->version[64]  = 0;
    strncpy(buf->machine, k.machine, 64);  buf->machine[64]  = 0;
    return 0;
}