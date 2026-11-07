// Native stand-in for IOS, the Wii's I/O co-processor. Replaces __ios_Ipc2, through which every
// IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv request passes.
//
// Request block (guest memory, built by the SDK):
//   +0x00 cmd (1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv)   +0x04 result   +0x08 fd
//   +0x0C args[5]: open: path, mode | read/write: buf, len | seek: offset, whence
//                  ioctl: cmd, in, in_len, out, out_len | ioctlv: cmd, n_in, n_io, vec -> {ptr, len}[]
//   +0x20 callback   +0x24 callback arg   (+0x2C thread queue for synchronous callers)
// Pointers inside requests are physical addresses; R32/W32 mask addresses so they work as-is.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define IOS_ENOENT (-106)
#define IOS_EEXIST (-105)
#define IOS_EINVAL (-4)
#define IOS_PENDING 0x7FFFFFFF  // request stays outstanding (e.g. STM event hook)

typedef struct { char path[64]; FILE *fp; int is_file; } Fd;
static Fd g_fd[32];
static int g_log = 1;
const char *g_nand_root = "nand";  // host folder standing in for the Wii's internal storage

static void rd_str(u32 a, char *out, int max) {
    int k = 0; for (; k < max - 1; k++) { out[k] = (char)R8(a + k); if (!out[k]) break; } out[k] = 0;
}
static void wr_buf(u32 a, const void *src, u32 n) { const u8 *s = src; for (u32 k = 0; k < n; k++) W8(a + k, s[k]); }
static void zero(u32 a, u32 n) { for (u32 k = 0; k < n; k++) W8(a + k, 0); }

// ---------------------------------------------------------------- NAND files (/dev/fs paths)
static void nand_host_path(const char *p, char *out, int n) { snprintf(out, n, "%s%s", g_nand_root, p); }
static void mkdirs(const char *path) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
}

static int open_fd(const char *path, int mode) {
    int fd;
    for (fd = 0; fd < 32 && g_fd[fd].path[0]; fd++) ;
    if (fd == 32) return -22;
    if (path[0] == '/' && strncmp(path, "/dev/", 5) != 0) {  // a NAND file
        char hp[512]; nand_host_path(path, hp, sizeof hp);
        FILE *f = fopen(hp, mode & 2 ? "r+b" : "rb");
        if (!f) return IOS_ENOENT;
        g_fd[fd].fp = f; g_fd[fd].is_file = 1;
    }
    snprintf(g_fd[fd].path, sizeof g_fd[fd].path, "%s", path);
    return fd;
}

// ---------------------------------------------------------------- devices
// /dev/di (disc drive). The driver reports 1 = success, 2 = drive error.
static s32 di_ioctl(u32 cmd, u32 in, u32 inl, u32 out, u32 outl) {
    if (outl) zero(out, outl);
    switch (cmd) {
    case 0x12: /* Inquiry */ if (outl >= 8) { W32(out, 0x00000002); W32(out + 4, 0x20060526); } return 1;
    case 0x88: /* GetCoverStatus */ if (outl >= 4) W32(out, 0x2); return 1;   // disc inserted, cover closed
    case 0x7A: /* GetCoverRegister */ if (outl >= 4) W32(out, 0); return 1;
    case 0xE0: /* RequestError */ if (outl >= 4) W32(out, 0); return 1;
    case 0x71: case 0x8D: case 0x70:
        fprintf(stderr, "[di] read cmd 0x%X offset %08X len %08X (not implemented yet)\n", cmd, R32(in + 8), R32(in + 4));
        return 2;
    }
    return 1;
}

static s32 dev_ioctl(Fd *d, u32 cmd, u32 in, u32 inl, u32 out, u32 outl) {
    if (!strcmp(d->path, "/dev/di")) return di_ioctl(cmd, in, inl, out, outl);
    if (!strcmp(d->path, "/dev/stm/eventhook")) return IOS_PENDING;  // fires only on reset/power button
    if (!strcmp(d->path, "/dev/fs")) {
        char p[128], hp[512];
        switch (cmd) {
        case 0x03: case 0x04: /* CreateDir / SetAttr */
            if (cmd == 0x03) { rd_str(in + 6, p, sizeof p); nand_host_path(p, hp, sizeof hp); mkdirs(hp); mkdir(hp, 0755); }
            return 0;
        case 0x06: /* GetAttr: out = owner u32, group u16, path[64], perms... */ {
            rd_str(in, p, sizeof p); nand_host_path(p, hp, sizeof hp);
            struct stat st; if (stat(hp, &st)) return IOS_ENOENT;
            zero(out, outl); return 0;
        }
        case 0x07: /* Delete */ rd_str(in, p, sizeof p); nand_host_path(p, hp, sizeof hp); remove(hp); return 0;
        case 0x08: /* Rename */ return 0;
        case 0x09: /* CreateFile: owner u32, group u16, path[64] at +6, perms... */ {
            rd_str(in + 6, p, sizeof p); nand_host_path(p, hp, sizeof hp);
            struct stat st; if (!stat(hp, &st)) return IOS_EEXIST;
            mkdirs(hp); FILE *f = fopen(hp, "wb"); if (f) fclose(f); return 0;
        }
        case 0x0B: /* GetUsage */ zero(out, outl); return 0;
        case 0x0C: /* Shutdown */ return 0;
        }
    }
    if (outl) zero(out, outl);
    return 0;
}

static s32 dev_ioctlv(Fd *d, u32 cmd, u32 nin, u32 nio, u32 vec) {
    u32 v[16][2];
    for (u32 k = 0; k < nin + nio && k < 16; k++) { v[k][0] = R32(vec + 8 * k); v[k][1] = R32(vec + 8 * k + 4); }
    if (!strcmp(d->path, "/dev/es")) {
        switch (cmd) {
        case 0x20: /* GetTitleID */ W32(v[0][0], 0x00010000); W32(v[0][0] + 4, 0x53554F45); return 0;  // SUOE
        case 0x1D: /* GetDataDir */ { const char *s = "/title/00010000/53554f45/data"; wr_buf(v[1][0], s, strlen(s) + 1); return 0; }
        }
    }
    if (!strcmp(d->path, "/dev/di")) { fprintf(stderr, "[di] ioctlv 0x%X\n", cmd); return 1; }
    if (!strcmp(d->path, "/dev/fs") && cmd == 0x0C /* GetUsage */) { W32(v[1][0], 0); W32(v[2][0], 0); return 0; }
    for (u32 k = nin; k < nin + nio && k < 16; k++) if (v[k][1] < 0x10000) zero(v[k][0], v[k][1]);
    return 0;
}

static s32 handle(u32 req) {
    u32 cmd = R32(req), fd = R32(req + 8);
    u32 a0 = R32(req + 0x0C), a1 = R32(req + 0x10), a2 = R32(req + 0x14), a3 = R32(req + 0x18), a4 = R32(req + 0x1C);
    Fd *d = fd < 32 ? &g_fd[fd] : NULL;
    s32 r = IOS_EINVAL;
    char path[128];
    switch (cmd) {
    case 1: rd_str(a0, path, sizeof path); r = open_fd(path, a1);
        if (g_log) fprintf(stderr, "[ios] open %s mode %u -> %d\n", path, a1, r); return r;
    case 2: if (d) { if (d->fp) fclose(d->fp); memset(d, 0, sizeof *d); } r = 0; break;
    case 3: if (d && d->fp) { u8 *tmp = malloc(a1); r = (s32)fread(tmp, 1, a1, d->fp); wr_buf(a0, tmp, r); free(tmp); } else r = 0; break;
    case 4: if (d && d->fp) { u8 *tmp = malloc(a1); for (u32 k = 0; k < a1; k++) tmp[k] = R8(a0 + k);
                              r = (s32)fwrite(tmp, 1, a1, d->fp); free(tmp); } else r = (s32)a1; break;
    case 5: if (d && d->fp) { fseek(d->fp, (s32)a0, a1 == 0 ? SEEK_SET : a1 == 1 ? SEEK_CUR : SEEK_END); r = (s32)ftell(d->fp); } else r = 0; break;
    case 6: r = d ? dev_ioctl(d, a0, a1, a2, a3, a4) : IOS_EINVAL; break;
    case 7: r = d ? dev_ioctlv(d, a0, a1, a2, a3) : IOS_EINVAL; break;
    }
    if (g_log && cmd >= 6)
        fprintf(stderr, "[ios] %s %s cmd 0x%X -> %d\n", cmd == 6 ? "ioctl" : "ioctlv", d ? d->path : "?", a0, r);
    return r;
}

// Asynchronous replies are queued and delivered later from rt_poll, like the real IPC interrupt:
// callers often arm a timeout alarm *after* issuing an async request, so replying instantly breaks them.
typedef struct { u32 req, cb; s32 result; } Pending;
static Pending g_pend[64]; static int g_npend;
#define SCRATCH_CTX 0x933F0000u   // free guest memory (the real IOS heap area) used as interrupt context

void ios_service(CPU *c) {
    while (g_npend) {
        Pending p = g_pend[0];
        memmove(g_pend, g_pend + 1, --g_npend * sizeof *g_pend);
        CPU save = *c;
        u32 cur = R32(0x800000D4);
        c->r[3] = SCRATCH_CTX; rt_call(c, 0x8021B7A0u);   // OSClearContext(scratch)
        c->r[3] = SCRATCH_CTX; rt_call(c, 0x8021B560u);   // OSSetCurrentContext(scratch)
        c->r[3] = (u32)p.result; c->r[4] = R32(p.req + 0x24);
        rt_call(c, p.cb);
        c->r[3] = R32(c->r[13] - 0x557C); c->r[4] = p.req; rt_call(c, 0x8024E7D0u);  // iosFree
        c->r[3] = SCRATCH_CTX; rt_call(c, 0x8021B7A0u);
        c->r[3] = cur; rt_call(c, 0x8021B560u);           // back to the interrupted context
        *c = save;
    }
}

// __ios_Ipc2(request, callback): callback == 0 means synchronous (return the result).
void f_8024D110(CPU *c) {
    u32 req = c->r[3], cb = c->r[4];
    if (!req) { c->r[3] = (u32)-4; return; }
    s32 r = handle(req);
    if (r == IOS_PENDING) { c->r[3] = 0; return; }   // left outstanding: the callback never fires
    W32(req + 4, (u32)r);
    if (!cb) { c->r[3] = (u32)r; return; }
    if (g_npend == 64) { fprintf(stderr, "[ios] too many pending replies\n"); return; }
    g_pend[g_npend++] = (Pending){ req, cb, r };
    g_irq_pending = 1;
    c->r[3] = 0;
}
