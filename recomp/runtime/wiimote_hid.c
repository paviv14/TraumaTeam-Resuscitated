// A real Wii Remote (+ Nunchuk) on the PC, over Bluetooth HID (paired in Windows, or through a Mayflash
// DolphinBar in mode 4). Its buttons, accelerometers, IR camera (pointer), Nunchuk stick and accelerometers
// go to the game as they are - the original controls, with real motion. Rumble is driven from here too.
//
// Protocol (wiibrew.org/wiki/Wiimote): output reports 0x11 LEDs, 0x12 report mode, 0x13/0x1A IR camera,
// 0x15 status, 0x16 write memory, 0x17 read memory; bit 0 of every output report's first byte is the rumble.
// Input report 0x37: buttons, accelerometer, 10 bytes of IR (basic mode, 2 x 2 dots), 6 bytes of extension.
// Windows only (hid.dll + setupapi); elsewhere the stubs at the bottom report no Remote.
//   TTPC_WIIMOTE=0     don't look for a Remote
//   TTPC_WII_GAIN      pointer sensitivity (default 1.4: how far the IR dots move for the pointer to reach the edge)
//   TTPC_WII_FLIPY=1   if the pointer moves up when you point down
#include "cpu.h"
#include "wiimote_hid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <process.h>

static CRITICAL_SECTION g_cs; static int g_cs_init;
static HANDLE g_dev = INVALID_HANDLE_VALUE; static int g_outlen = 22;
static volatile int g_connected, g_stop;
static WiiState g_st;                   // last parsed state (under g_cs)
static volatile int g_rumble;
static u64 g_seq;
// calibration
static int g_c0[3] = { 512, 512, 512 }, g_c1[3] = { 616, 616, 616 };
static int g_nc0[3] = { 512, 512, 512 }, g_nc1[3] = { 716, 716, 716 }, g_nsx[3] = { 230, 26, 128 }, g_nsy[3] = { 230, 26, 128 };
static int g_ext_ready;

static int send(const u8 *rep, int n) {
    u8 buf[64] = { 0 }; memcpy(buf, rep, n);
    buf[1] |= g_rumble ? 1 : 0;
    DWORD w = 0; OVERLAPPED ov = { 0 }; ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    int len = g_outlen > n ? g_outlen : n;
    BOOL ok = WriteFile(g_dev, buf, len, &w, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0 && GetOverlappedResult(g_dev, &ov, &w, FALSE);
    CloseHandle(ov.hEvent);
    if (!ok) ok = HidD_SetOutputReport(g_dev, buf, len);   // the Microsoft Bluetooth stack sometimes wants this
    return ok;
}
static void write_reg(u32 addr, const u8 *data, int n) {
    u8 r[22] = { 0x16, 0x04, (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, (u8)n };
    memcpy(r + 6, data, n); send(r, 22); Sleep(30);
}
static void read_mem(int reg, u32 addr, int n) {
    u8 r[7] = { 0x17, (u8)(reg ? 0x04 : 0x00), (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, (u8)(n >> 8), (u8)n };
    send(r, 7);
}
static void set_mode(void) { u8 r[3] = { 0x12, 0x04, 0x37 }; send(r, 3); }
static void setup(void) {
    u8 led[2] = { 0x11, 0x10 }; send(led, 2);
    u8 a[2] = { 0x13, 0x04 }, b[2] = { 0x1A, 0x04 }; send(a, 2); Sleep(30); send(b, 2); Sleep(30);
    u8 on = 0x08; write_reg(0xB00030, &on, 1);
    static const u8 s1[9] = { 0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64 }, s2[2] = { 0x63, 0x03 };   // sensitivity: Wii level 3
    write_reg(0xB00000, s1, 9); write_reg(0xB0001A, s2, 2);
    u8 mode = 0x01; write_reg(0xB00033, &mode, 1);   // basic: 10 bytes, 2 x 2 dots
    write_reg(0xB00030, &on, 1);
    read_mem(0, 0x0016, 10);                          // accelerometer calibration
    u8 st[2] = { 0x15, 0x00 }; send(st, 2);           // status: is an extension plugged in?
    set_mode();
}
static void init_ext(void) {
    u8 v = 0x55; write_reg(0xA400F0, &v, 1); v = 0x00; write_reg(0xA400FB, &v, 1);   // unencrypted
    read_mem(1, 0xA40020, 16);                                                      // Nunchuk calibration
    set_mode(); g_ext_ready = 1;
}

static float acc_g(int raw, int i, const int *c0, const int *c1) { int d = c1[i] - c0[i]; return d ? (float)(raw - c0[i]) / d : 0.f; }
static float stick(int v, const int *c) {   // c: max, min, centre
    float r = v >= c[2] ? (float)(v - c[2]) / (c[0] - c[2] ? c[0] - c[2] : 1) : (float)(v - c[2]) / (c[2] - c[1] ? c[2] - c[1] : 1);
    if (r > 1) r = 1; if (r < -1) r = -1; if (fabsf(r) < 0.08f) r = 0; return r;
}

static void parse(const u8 *d, int n) {
    u8 id = d[0];
    if (id == 0x20 && n >= 7) {   // status: extension plugged in or out; the report mode must be set again after it
        int ext = (d[3] & 0x02) != 0;
        EnterCriticalSection(&g_cs); g_st.nunchuk = ext; LeaveCriticalSection(&g_cs);
        if (ext) init_ext(); else { g_ext_ready = 0; set_mode(); }
        fprintf(stderr, "[wiimote] battery %d%%, Nunchuk %s\n", d[6] * 100 / 200, ext ? "in" : "out");
        return;
    }
    if (id == 0x21 && n >= 22) {  // memory read reply
        int size = (d[3] >> 4) + 1; u16 addr = (u16)(d[4] << 8 | d[5]); const u8 *p = d + 6;
        if (addr == 0x0016 && size >= 8) {
            g_c0[0] = p[0] << 2 | (p[3] >> 4 & 3); g_c0[1] = p[1] << 2 | (p[3] >> 2 & 3); g_c0[2] = p[2] << 2 | (p[3] & 3);
            g_c1[0] = p[4] << 2 | (p[7] >> 4 & 3); g_c1[1] = p[5] << 2 | (p[7] >> 2 & 3); g_c1[2] = p[6] << 2 | (p[7] & 3);
        } else if (addr == 0x0020 && size >= 14) {
            for (int i = 0; i < 3; i++) { g_nc0[i] = p[i] << 2; g_nc1[i] = p[4 + i] << 2; }
            g_nsx[0] = p[8]; g_nsx[1] = p[9]; g_nsx[2] = p[10]; g_nsy[0] = p[11]; g_nsy[1] = p[12]; g_nsy[2] = p[13];
            if (g_nc1[1] <= g_nc0[1] || g_nsx[0] <= g_nsx[1]) {   // blank calibration: defaults
                for (int i = 0; i < 3; i++) { g_nc0[i] = 512; g_nc1[i] = 716; }
                g_nsx[0] = g_nsy[0] = 230; g_nsx[1] = g_nsy[1] = 26; g_nsx[2] = g_nsy[2] = 128;
            }
        }
        return;
    }
    if (id != 0x37 || n < 22) return;
    WiiState s; EnterCriticalSection(&g_cs); s = g_st; LeaveCriticalSection(&g_cs);
    s.buttons = (u16)((d[1] & 0x1F) | (d[2] & 0x9F) << 8);
    int ax = d[3] << 2 | (d[1] >> 5 & 3), ay = d[4] << 2 | (d[2] >> 4 & 2), az = d[5] << 2 | (d[2] >> 5 & 2);
    float rx = acc_g(ax, 0, g_c0, g_c1), ry = acc_g(ay, 1, g_c0, g_c1), rz = acc_g(az, 2, g_c0, g_c1);
    s.acc[0] = -rx; s.acc[1] = -rz; s.acc[2] = ry;                // KPAD axes: pointing at the screen, level = (0,-1,0)
    s.ndots = 0;
    for (int g = 0; g < 2; g++) {
        const u8 *p = d + 6 + g * 5;
        int x1 = p[0] | (p[2] >> 4 & 3) << 8, y1 = p[1] | (p[2] >> 6 & 3) << 8;
        int x2 = p[3] | (p[2] & 3) << 8, y2 = p[4] | (p[2] >> 2 & 3) << 8;
        if (x1 != 0x3FF && y1 != 0x3FF && s.ndots < 4) { s.dot[s.ndots][0] = x1; s.dot[s.ndots][1] = y1; s.ndots++; }
        if (x2 != 0x3FF && y2 != 0x3FF && s.ndots < 4) { s.dot[s.ndots][0] = x2; s.dot[s.ndots][1] = y2; s.ndots++; }
    }
    if (s.nunchuk && g_ext_ready) {
        const u8 *e = d + 16;
        if (!(e[0] == 0xFF && e[1] == 0xFF)) {
            s.stick[0] = stick(e[0], g_nsx); s.stick[1] = stick(e[1], g_nsy);
            int nx = e[2] << 2 | (e[5] >> 2 & 3), ny = e[3] << 2 | (e[5] >> 4 & 3), nz = e[4] << 2 | (e[5] >> 6 & 3);
            float qx = acc_g(nx, 0, g_nc0, g_nc1), qy = acc_g(ny, 1, g_nc0, g_nc1), qz = acc_g(nz, 2, g_nc0, g_nc1);
            s.nacc[0] = -qx; s.nacc[1] = -qz; s.nacc[2] = qy;
            if (!(e[5] & 1)) s.buttons |= 0x2000;   // Z
            if (!(e[5] & 2)) s.buttons |= 0x4000;   // C
        }
    }
    EnterCriticalSection(&g_cs); g_st = s; g_seq++; LeaveCriticalSection(&g_cs);
}

static unsigned __stdcall reader(void *arg) {
    (void)arg;
    u8 buf[64]; OVERLAPPED ov = { 0 }; ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    while (!g_stop) {
        DWORD r = 0; ResetEvent(ov.hEvent);
        BOOL ok = ReadFile(g_dev, buf, sizeof buf, &r, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 1000);
            if (w == WAIT_TIMEOUT) { CancelIo(g_dev); continue; }
            ok = GetOverlappedResult(g_dev, &ov, &r, FALSE);
        }
        if (!ok) { fprintf(stderr, "[wiimote] disconnected\n"); break; }
        parse(buf, (int)r);
    }
    CloseHandle(ov.hEvent);
    CloseHandle(g_dev); g_dev = INVALID_HANDLE_VALUE; g_connected = 0;
    return 0;
}

static int try_open(void) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd }; int found = 0;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        DWORD need = 0; SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = malloc(need); det->cbSize = sizeof *det;
        if (SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, NULL, NULL)) {
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES at = { sizeof at };
                if (HidD_GetAttributes(h, &at) && at.VendorID == 0x057E && (at.ProductID == 0x0306 || at.ProductID == 0x0330)) {
                    PHIDP_PREPARSED_DATA pp; HIDP_CAPS caps;
                    if (HidD_GetPreparsedData(h, &pp)) { if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength) g_outlen = caps.OutputReportByteLength; HidD_FreePreparsedData(pp); }
                    g_dev = h; found = 1;
                    fprintf(stderr, "[wiimote] found a Wii Remote%s (%04X:%04X)\n", at.ProductID == 0x0330 ? " Plus" : "", at.VendorID, at.ProductID);
                } else CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

int whid_poll(WiiState *out) {
    static int enabled = -1; static DWORD last_try;
    if (enabled < 0) { const char *e = getenv("TTPC_WIIMOTE"); enabled = !(e && *e == '0'); }
    if (!enabled) return 0;
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = 1; }
    if (!g_connected) {
        DWORD now = GetTickCount();
        if (now - last_try < 3000 && last_try) return 0;   // look again every 3 s (pair it any time)
        last_try = now;
        if (!try_open()) return 0;
        memset(&g_st, 0, sizeof g_st); g_ext_ready = 0;
        g_connected = 1; g_stop = 0;
        setup();
        _beginthreadex(NULL, 0, reader, NULL, 0, NULL);
    }
    static u64 seen; static int fresh;
    EnterCriticalSection(&g_cs); *out = g_st; fresh = g_seq != seen; seen = g_seq; LeaveCriticalSection(&g_cs);
    (void)fresh;
    return g_connected;
}
void whid_rumble(int on) {
    on = on != 0;
    if (on == g_rumble || !g_connected) { g_rumble = on; return; }
    g_rumble = on;
    u8 r[2] = { 0x10, 0x00 }; send(r, 2);   // report 0x10 carries only the rumble bit
}
#else
int whid_poll(WiiState *out) { (void)out; return 0; }
void whid_rumble(int on) { (void)on; }
#endif

// ---------------------------------------------------------------- IR dots -> the KPAD pointer
// The camera sees the sensor bar's two dots (1024 x 768, mirrored). The pointer is their midpoint turned back
// by the Remote's roll (the angle between the dots), scaled so the dots moving ~1/gain of the picture reaches
// the screen's edge. With one dot in view the other is assumed where it was last seen.
int whid_pointer(const WiiState *s, float *px, float *py, float *hx, float *hy, float *dist) {
    static float last_dx = 200, last_dy = 0; static int have_pair;
    static float gain = 0; static int flip = -1;
    if (!gain) { const char *e = getenv("TTPC_WII_GAIN"); gain = e ? (float)atof(e) : 1.4f; if (gain < 0.3f || gain > 5) gain = 1.4f; }
    if (flip < 0) { const char *e = getenv("TTPC_WII_FLIPY"); flip = e && *e == '1'; }
    float ax, ay, bx, by;
    if (s->ndots >= 2) {
        ax = s->dot[0][0]; ay = s->dot[0][1]; bx = s->dot[1][0]; by = s->dot[1][1];
        if (ax > bx) { float t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
        last_dx = bx - ax; last_dy = by - ay; have_pair = 1;
    } else if (s->ndots == 1 && have_pair) {
        // which of the pair is it? the one nearer where that dot was would do; assume the left one if it's left of centre
        ax = s->dot[0][0]; ay = s->dot[0][1];
        if (ax > 512) { bx = ax; by = ay; ax = bx - last_dx; ay = by - last_dy; } else { bx = ax + last_dx; by = ay + last_dy; }
    } else return 0;
    float ang = atan2f(by - ay, bx - ax);           // the Remote's roll as the camera sees it
    float mx = (ax + bx) * 0.5f - 512.f, my = (ay + by) * 0.5f - 384.f;
    float c = cosf(-ang), sn = sinf(-ang), rx = mx * c - my * sn, ry = mx * sn + my * c;
    *px = -rx / 512.f * gain; *py = (flip ? -ry : ry) / 384.f * gain * (4.f / 3.f) * 0.75f;
    *hx = cosf(ang); *hy = sinf(ang);
    float sep = sqrtf(last_dx * last_dx + last_dy * last_dy);
    *dist = sep > 1 ? 0.2f * 1330.f / sep : 2.f;     // sensor bar dots ~20 cm apart; the camera's focal length ~1330 px
    return s->ndots >= 2 ? 2 : 1;
}
