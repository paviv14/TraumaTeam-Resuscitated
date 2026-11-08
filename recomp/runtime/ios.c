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
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#define IOS_ENOENT (-106)
#define IOS_EEXIST (-105)
#define IOS_EINVAL (-4)
#define IOS_PENDING 0x7FFFFFFF  // request stays outstanding (e.g. STM event hook)

typedef struct { char path[64]; FILE *fp; int is_file; } Fd;
static Fd g_fd[32];
static int g_log = 1;
static u32 g_cur_req, g_cur_cb;
typedef struct { u32 req, cb; s32 result; u64 due; } Pending;
// IOS answers take time on hardware (the request goes to the ARM coprocessor). Code relies on that: it
// stores the async call's return value *after* issuing the request, so a reply delivered before the call
// returns gets overwritten (seen as the save-data check failing). Replies wait at least this long.
#define IOS_LATENCY_NS 200000ull
static u64 now_ns(void) { return host_now_ns(); }
static Pending g_pend[64]; static int g_npend;   // request being handled (for requests completed later)
static void complete_later(u32 req, u32 cb, s32 result);
const char *g_nand_root = "nand";  // host folder standing in for the Wii's internal storage

// The system menu creates a title's data directory before the game first runs; games assume it exists.
void ios_init(void) {
    char p[512];
    snprintf(p, sizeof p, "%s/title/00010000/53554f45/data/x", g_nand_root);
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; host_mkdir(p); *q = '/'; }
}

static void rd_str(u32 a, char *out, int max) {
    int k = 0; for (; k < max - 1; k++) { out[k] = (char)R8(a + k); if (!out[k]) break; } out[k] = 0;
}
static void wr_buf(u32 a, const void *src, u32 n) { const u8 *s = src; for (u32 k = 0; k < n; k++) W8(a + k, s[k]); }
static void zero(u32 a, u32 n) { for (u32 k = 0; k < n; k++) W8(a + k, 0); }

// ---------------------------------------------------------------- NAND files (/dev/fs paths)
static void nand_host_path(const char *p, char *out, int n) { snprintf(out, n, "%s%s", g_nand_root, p); }
static void mkdirs(const char *path) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++) if (*q == '/') { *q = 0; host_mkdir(tmp); *q = '/'; }
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
int disc_read(u64 off, u32 len, u32 dst);
// /dev/di (disc drive). The driver reports 1 = success, 2 = drive error.
static s32 di_ioctl(u32 cmd, u32 in, u32 inl, u32 out, u32 outl) {
    if (outl) zero(out, outl);
    switch (cmd) {
    case 0x12: /* Inquiry */ if (outl >= 8) { W32(out, 0x00000002); W32(out + 4, 0x20060526); } return 1;
    case 0x88: /* GetCoverStatus */ if (outl >= 4) W32(out, 0x2); return 1;   // disc inserted, cover closed
    case 0x7A: /* GetCoverRegister */ if (outl >= 4) W32(out, 0); return 1;
    case 0xE0: /* RequestError */ if (outl >= 4) W32(out, 0); return 1;
    case 0x71: case 0x8D:   /* Read / UnencryptedRead: in+4 length, in+8 offset in 4-byte units */
        return disc_read((u64)R32(in + 8) << 2, R32(in + 4), out);
    case 0x70:              /* ReadDiskID: first 0x20 bytes of the disc header */
        return disc_read(0, outl < 0x20 ? outl : 0x20, out);
    }
    return 1;
}

static s32 dev_ioctl(Fd *d, u32 cmd, u32 in, u32 inl, u32 out, u32 outl) {
    if (!strcmp(d->path, "/dev/di")) return di_ioctl(cmd, in, inl, out, outl);
    if (!strcmp(d->path, "/dev/stm/eventhook")) return IOS_PENDING;  // fires only on reset/power button
    if (!strcmp(d->path, "/dev/fs")) {
        // ISFS ioctls: 1 Format, 2 GetStats, 3 CreateDir, 5 SetAttr, 6 GetAttr, 7 Delete, 8 Rename,
        // 9 CreateFile, 13 Shutdown (4 ReadDir and 12 GetUsage are ioctlv). Create* input: owner u32,
        // group u16, path[64] at +6, then owner/group/other permissions and attributes.
        char p[128], p2[128], hp[512], hp2[512];
        struct stat st;
        switch (cmd) {
        case 0x02:   /* GetStats: block size, free/used clusters, bad, reserved, free/used inodes */
            if (outl >= 0x1C) { W32(out, 0x4000); W32(out + 4, 0x4000); W32(out + 8, 0x100); W32(out + 12, 0);
                                W32(out + 16, 0); W32(out + 20, 0x4000); W32(out + 24, 0x100); }
            return 0;
        case 0x03:   /* CreateDir */
            rd_str(in + 6, p, sizeof p); nand_host_path(p, hp, sizeof hp);
            if (!stat(hp, &st)) return IOS_EEXIST;
            mkdirs(hp); host_mkdir(hp); return 0;
        case 0x05: return 0;   /* SetAttr */
        case 0x06:   /* GetAttr -> owner u32, group u16, path[64], owner/group/other perms, attributes */
            rd_str(in, p, sizeof p); nand_host_path(p, hp, sizeof hp);
            if (stat(hp, &st)) return IOS_ENOENT;
            zero(out, outl);
            if (outl >= 0x4A) { W32(out, 0x1000); W16(out + 4, 0x0001); wr_buf(out + 6, p, strlen(p));
                                W8(out + 0x46, 3); W8(out + 0x47, 3); W8(out + 0x48, 1); }
            return 0;
        case 0x07:   /* Delete */
            rd_str(in, p, sizeof p); nand_host_path(p, hp, sizeof hp);
            if (stat(hp, &st)) return IOS_ENOENT;
            remove(hp); return 0;
        case 0x08:   /* Rename: old path[64], new path[64] */
            rd_str(in, p, sizeof p); rd_str(in + 64, p2, sizeof p2);
            nand_host_path(p, hp, sizeof hp); nand_host_path(p2, hp2, sizeof hp2);
            if (stat(hp, &st)) return IOS_ENOENT;
            mkdirs(hp2); rename(hp, hp2); return 0;
        case 0x09:   /* CreateFile */
            rd_str(in + 6, p, sizeof p); nand_host_path(p, hp, sizeof hp);
            if (!stat(hp, &st)) return IOS_EEXIST;
            mkdirs(hp); { FILE *f = fopen(hp, "wb"); if (f) fclose(f); } return 0;
        case 0x0D: return 0;   /* Shutdown */
        }
        if (g_log) fprintf(stderr, "[ios] /dev/fs ioctl 0x%X not handled\n", cmd);
    }
    // ioctl on an open NAND file: 11 = GetFileStats -> size u32, position u32
    if (d->is_file && cmd == 0x0B) {
        long pos = ftell(d->fp); fseek(d->fp, 0, SEEK_END); long size = ftell(d->fp); fseek(d->fp, pos, SEEK_SET);
        if (outl >= 8) { W32(out, (u32)size); W32(out + 4, (u32)pos); }
        return 0;
    }
    if (outl) zero(out, outl);
    return 0;
}

// ---------------------------------------------------------------- Bluetooth module (idle HCI controller)
// The SDK's Bluetooth stack talks HCI over USB: control messages carry commands, interrupt reads
// (endpoint 0x81) receive events, bulk transfers carry ACL data. We answer every command like an idle
// controller with no paired devices, so WPAD initialises and reports no Wii Remotes. Input for the
// PC port is injected at the WPAD level instead.
typedef struct { u32 req, cb, buf, len; } BtRead;
static BtRead g_btr[8]; static int g_nbtr;
static u8 g_evq[32][64]; static int g_evlen[32], g_nev;
static void bt_event(const u8 *e, int n) { if (g_nev < 32) { memcpy(g_evq[g_nev], e, n); g_evlen[g_nev++] = n; } }
static void bt_pump(void) {
    while (g_nev && g_nbtr) {
        BtRead r = g_btr[0]; memmove(g_btr, g_btr + 1, --g_nbtr * sizeof *g_btr);
        int n = g_evlen[0] < (int)r.len ? g_evlen[0] : (int)r.len;
        wr_buf(r.buf, g_evq[0], n);
        memmove(g_evq, g_evq + 1, (g_nev - 1) * sizeof g_evq[0]); memmove(g_evlen, g_evlen + 1, (g_nev - 1) * sizeof(int)); g_nev--;
        W32(r.req + 4, (u32)n);
        complete_later(r.req, r.cb, n);
    }
}
static void hci_command(u16 op, u32 data, u32 len) {
    u8 e[320] = { 0x0E, 0, 1, (u8)op, (u8)(op >> 8), 0 };   // Command Complete, 1 packet, opcode, status 0
    int n = 6;
    switch (op) {
    case 0x1009: memcpy(e + n, "\x11\x22\x33\x44\x55\x66", 6); n += 6; break;          // Read_BD_ADDR
    case 0x1001: { u8 r[] = { 3, 0x40, 0xA7, 3, 0x0F, 0, 0xC3, 0x40 }; memcpy(e + n, r, 8); n += 8; break; } // local version
    case 0x1003: { u8 r[] = { 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0, 0x80 }; memcpy(e + n, r, 8); n += 8; break; } // features
    case 0x1005: { u8 r[] = { 0x53, 0x01, 0x40, 0x0A, 0x00, 0x00, 0x00 }; memcpy(e + n, r, 7); n += 7; break; }   // buffer size
    case 0x0C0D: { u8 r[] = { 0xFF, 0x00, 0x00, 0x00 }; memcpy(e + n, r, 4); n += 4; break; }  // stored link keys: none
    case 0x0C14: memset(e + n, 0, 248); n += 248 > 58 ? 58 : 248; break;                        // local name
    case 0x0401: case 0x0405: case 0x0406: case 0x0419: case 0x041B: case 0x080D:            // async: Command Status
        e[0] = 0x0F; e[2] = 0; e[3] = 1; e[4] = (u8)op; e[5] = (u8)(op >> 8); n = 6;
        if (op == 0x0401) { bt_event(e, n); u8 done[] = { 0x01, 1, 0 }; memcpy(e, done, 3); n = 3; }  // inquiry: none found
        break;
    }
    e[1] = (u8)(n - 2);
    bt_event(e, n);
    if (g_log) fprintf(stderr, "[bt] HCI %04X -> answered\n", op);
}
static s32 bt_ioctlv(u32 cmd, u32 nin, u32 nio, u32 v[16][2]) {
    if (cmd == 2 || cmd == 1) {                          // interrupt/bulk read
        u32 ep = R8(v[0][0]);
        if (cmd == 2 && ep == 0x81 && g_nbtr < 8) { g_btr[g_nbtr++] = (BtRead){ g_cur_req, g_cur_cb, v[nin][0], v[nin][1] }; bt_pump(); }
        return IOS_PENDING;                              // completed by bt_pump when an event exists
    }
    if (cmd == 0) {                                      // control message = HCI command
        u32 data = v[6][0], len = v[6][1];
        if (len >= 3) hci_command((u16)(R8(data) | R8(data + 1) << 8), data + 3, len - 3);
        bt_pump();
        return (s32)len;
    }
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
    if (!strncmp(d->path, "/dev/usb/oh1/57e/305", 20)) return bt_ioctlv(cmd, nin, nio, v);
    if (!strcmp(d->path, "/dev/fs") && (cmd == 0x04 || cmd == 0x0C)) {
        char p[128], hp[512]; rd_str(v[0][0], p, sizeof p); nand_host_path(p, hp, sizeof hp);
        DIR *dir = opendir(hp);
        if (!dir) return IOS_ENOENT;
        u32 count = 0, files = 0; char names[4096]; u32 nl = 0;
        for (struct dirent *e; (e = readdir(dir)); ) {
            if (e->d_name[0] == '.') continue;
            count++; files++;
            size_t L = strlen(e->d_name) + 1;
            if (nl + L < sizeof names) { memcpy(names + nl, e->d_name, L); nl += L; }
        }
        closedir(dir);
        if (cmd == 0x0C) { W32(v[nin][0], files * 2); W32(v[nin + 1][0], count + 1); return 0; }   // GetUsage
        if (nin == 1) { W32(v[nin][0], count); return 0; }                                          // ReadDir: count
        u32 maxn = R32(v[1][0]); if (count > maxn) count = maxn;                                    // ReadDir: names
        wr_buf(v[nin][0], names, nl < v[nin][1] ? nl : v[nin][1]); W32(v[nin + 1][0], count);
        return 0;
    }
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

#define SCRATCH_CTX 0x933F0000u   // free guest memory (the real IOS heap area) used as interrupt context

void ios_service(CPU *c) {
    while (g_npend) {
        if (now_ns() < g_pend[0].due) { g_irq_pending = 1; return; }   // not answered yet: poll again
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

static void complete_later(u32 req, u32 cb, s32 result) {
    if (!cb) return;   // a synchronous caller would be asleep: not used for pending requests here
    if (g_npend < 64) { g_pend[g_npend++] = (Pending){ req, cb, result, now_ns() + IOS_LATENCY_NS }; g_irq_pending = 1; }
}

// __ios_Ipc2(request, callback): callback == 0 means synchronous (return the result).
void f_8024D110(CPU *c) {
    u32 req = c->r[3], cb = c->r[4];
    if (!req) { c->r[3] = (u32)-4; return; }
    g_cur_req = req; g_cur_cb = cb;
    s32 r = handle(req);
    if (r == IOS_PENDING) { c->r[3] = 0; return; }   // left outstanding: the callback never fires
    W32(req + 4, (u32)r);
    if (!cb) { c->r[3] = (u32)r; return; }
    if (g_npend == 64) { fprintf(stderr, "[ios] too many pending replies\n"); return; }
    g_pend[g_npend++] = (Pending){ req, cb, r, now_ns() + IOS_LATENCY_NS };
    g_irq_pending = 1;
    c->r[3] = 0;
}
