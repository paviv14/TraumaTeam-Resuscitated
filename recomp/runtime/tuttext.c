// Tutorial text for the controller in use: the tutorial messages (tutorial/<mode>/eng/NNN.bm2, MSG2) talk about
// the Wii Remote and Nunchuk ("Twist the Wii Remote to rotate."). When keyboard & mouse or a controller is in use,
// those lines are rewritten as the file is read (disc.c) to say what to do on the PC, from the player's bindings.
// The [N] button icons in the text stay: the game draws them from tutorial/button.tpx, whose live prompt slots
// show the bound buttons (texpack.c). disc.c makes room by telling the game these files are 512 bytes longer.
//
// MSG2: 0x0C file size; 0x24 table offset, 0x28 count; entries of 0x40 bytes: label (0x24), length, offset, ...;
// a message: 00000001 00000008, then its lines, each ending in a zero byte (the first is "*Title").
#include "cpu.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int g_prompt_set;
const char *bind_key_label(int b);   // platform.c
#define TUT_SLACK 512

static const char *pad_word(int p, int set) {   // a controller button's name, as printed on it
    static const char *X[] = { "A", "B", "X", "Y", "View", "Guide", "Menu", "LS", "RS", "LB", "RB", "Up", "Down", "Left", "Right" };
    static const char *P4[] = { "Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right" };
    static const char *P5[] = { "Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right" };
    if (p == PAD_LT) return set == 2 ? "LT" : "L2";
    if (p == PAD_RT) return set == 2 ? "RT" : "R2";
    if (p < 0 || p > 14) return "?";
    return (set == 2 ? X : set == 3 ? P4 : P5)[p];
}

// whole messages: the two Wii lines -> keyboard & mouse / controller ({GRAB} {SWING} {TWL} {TWR} {KUP}: bindings)
typedef struct { const char *a, *b, *ka, *kb, *pa, *pb; } Rw;
static const Rw RW[] = {
    { "Grab with [3] and [4], then twist", "the Wii Remote to rotate.", "Grab with [3] and [4], then use", "the mouse wheel to rotate.", "Grab with [3] and {GRAB}, then use", "{TWL} / {TWR} to rotate." },
    { "Auscultation results can be", "heard on the Wii Remote.", NULL, "heard on your speakers.", NULL, "heard on your speakers." },
    { "Twist the Wii Remote to match the", NULL, "Turn the mouse wheel to match the", NULL, "Use {TWL} / {TWR} to match the", NULL },
    { "Move the Wii Remote in time with", NULL, "Middle-click in time with", NULL, "Press {SWING} in time with", NULL },
    { "Point at the guide and rotate the", "Wii Remote while holding [3].", "Point at the guide and turn the", "mouse wheel while holding [3].", "Point at the guide and press", "{TWL} / {TWR} while holding [3]." },
    { "Hold [3] and swing the Wii", "Remote downward.", "Hold [3] and middle-click", "to strike.", "Hold [3] and press {SWING}", "to strike." },
    { "Hold [3] and [4] down and", "move the Wii Remote.", NULL, "move the mouse.", NULL, "move the right stick." },
    { "Push the Wii Remote and Nunchuk", "forward, pointing at the screen.", "Hold {KUP} and scroll the wheel up,", "pointing at the screen.", "Click in both sticks together,", "pointing at the screen." },
    { "Hold [3] and [4] and push the", "Wii Remote towards the screen.", "Hold [3] and [4] and scroll", "the mouse wheel up.", "Hold [3] and {GRAB} and push", "the right stick up." },
    { "Hold [3] and [4] and pull the", "Wii Remote towards you.", "Hold [3] and [4] and scroll", "the mouse wheel down.", "Hold [3] and {GRAB} and pull", "the right stick down." },
    { "Tilt the Nunchuk to adjust angle.", NULL, "Ctrl + mouse wheel turns it.", NULL, "Hold [8] + {TWL} / {TWR} to turn it.", NULL },
    { "Twist the Wii Remote to rotate.", NULL, "Turn the mouse wheel to rotate.", NULL, "Use {TWL} / {TWR} to rotate.", NULL },
    { "Strike down with the Wii Remote", NULL, "Middle-click to strike down", NULL, "Press {SWING} to strike down", NULL },
    { "Hold [3] and then twist the Wii", "Remote to follow the guideline.", "Hold [3] and turn the mouse", "wheel to follow the guideline.", "Hold [3] and use {TWL} / {TWR}", "to follow the guideline." },
    { "Twist the Nunchuk to align the", NULL, "Turn the mouse wheel to align the", NULL, "Use {TWL} / {TWR} to align the", NULL },
    { "strike down with the Wii Remote.", NULL, "middle-click to strike down.", NULL, "press {SWING} to strike down.", NULL },
    { "Then twist the Wii Remote.", NULL, "Then turn the mouse wheel.", NULL, "Then use {TWL} / {TWR}.", NULL },
    { "While grabbing a fragment, tilt", "[6] on the Nunchuk left or right.", "While grabbing a fragment, press", "[6] left or right.", NULL, "[6] left or right." },
    { "[6] on the Nunchuk left or right.", NULL, "[6] left or right.", NULL, "[6] left or right.", NULL },
};

static void put(char *o, size_t n, const char *s, int set) {   // expand the binding words
    size_t k = 0; o[0] = 0;
    while (*s && k + 1 < n) {
        const char *w = NULL; size_t skip = 0;
        if (*s == '{') {
            const char *e = strchr(s, '}'); char t[16] = ""; if (e && e - s < 15) { memcpy(t, s + 1, e - s - 1); t[e - s - 1] = 0; skip = e - s + 1; }
            if (!strcmp(t, "GRAB")) w = pad_word(g_bind[B_GRAB].pad, set);
            else if (!strcmp(t, "SWING")) w = pad_word(g_bind[B_SWING].pad, set);
            else if (!strcmp(t, "TWL")) w = pad_word(g_bind[B_TW_L].pad, set);
            else if (!strcmp(t, "TWR")) w = pad_word(g_bind[B_TW_R].pad, set);
            else if (!strcmp(t, "KUP")) w = bind_key_label(B_ST_UP);
        }
        if (w) { size_t l = strlen(w); if (k + l >= n) break; memcpy(o + k, w, l); k += l; s += skip; }
        else o[k++] = *s++;
    }
    o[k] = 0;
}
static void repl(char *s, size_t n, const char *from, const char *to) {   // every occurrence, in place
    char tmp[256]; char *p;
    while ((p = strstr(s, from))) {
        snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)(p - s), s, to, p + strlen(from));
        snprintf(s, n, "%s", tmp);
        if (strstr(to, from)) break;
    }
}

// one message (its lines) for set: 1 keyboard & mouse, 2-4 a controller style
static int rewrite_lines(char lines[][256], int nl, int set) {
    int kb = set == 1, ch = 0;
    for (int i = 1; i < nl; i++) for (size_t r = 0; r < sizeof RW / sizeof RW[0]; r++) {
        const Rw *w = &RW[r];
        if (strcmp(lines[i], w->a) || (w->b && (i + 1 >= nl || strcmp(lines[i + 1], w->b)))) continue;
        const char *na = kb ? w->ka : w->pa, *nb = kb ? w->kb : w->pb;
        if (na) put(lines[i], 256, na, set);
        if (w->b && nb) put(lines[i + 1], 256, nb, set);
        ch = 1; break;
    }
    for (int i = 1; i < nl; i++) {
        char before[256]; snprintf(before, sizeof before, "%s", lines[i]);
        repl(lines[i], 256, " on the Nunchuk", "");
        if (kb) { repl(lines[i], 256, "Tilt [6]", "Press [6]"); repl(lines[i], 256, "tilt [6]", "press [6]"); }
        else { char g[64]; snprintf(g, sizeof g, "[3] and %s", pad_word(g_bind[B_GRAB].pad, set)); repl(lines[i], 256, "[3] and [4]", g);
               snprintf(g, sizeof g, "[8] and %s", pad_word(g_bind[B_GRAB].pad, set)); repl(lines[i], 256, "[8] and [4]", g); }
        if (strcmp(before, lines[i])) ch = 1;
    }
    return ch;
}

static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// in: the file as on the disc. Returns the new length in out (cap bytes), or 0 to keep the file as it is.
u32 tut_rewrite(const u8 *in, u32 n, u8 *out, u32 cap) {
    int set = g_prompt_set;
    if (set < 1 || n < 0x30 || memcmp(in + 4, "MSG2", 4)) return 0;
    u32 tab = rd32(in + 0x24), cnt = rd32(in + 0x28);
    if (cnt > 64 || tab + cnt * 0x40 > n) return 0;
    u32 first = n;   // the messages follow the table
    for (u32 i = 0; i < cnt; i++) { u32 off = rd32(in + tab + i * 0x40 + 0x28); if (off < first) first = off; }
    if (first > n || first > cap) return 0;
    memcpy(out, in, first);
    u32 at = first; int changed = 0;
    for (u32 i = 0; i < cnt; i++) {
        u8 *e = out + tab + i * 0x40; u32 len = rd32(in + tab + i * 0x40 + 0x24), off = rd32(in + tab + i * 0x40 + 0x28);
        if (off + len > n || len < 8) return 0;
        const u8 *m = in + off; char lines[8][256]; int nl = 0; u32 p = 8;
        while (p < len && nl < 8) { u32 q = p; while (q < len && m[q]) q++; snprintf(lines[nl], 256, "%.*s", (int)(q - p), (const char *)m + p); nl++; p = q + 1; }
        u32 tail = p < len ? len - p : 0;   // anything after the lines, kept as it is
        if (rewrite_lines(lines, nl, set)) changed = 1;
        u32 start = at; if (at + 8 > cap) return 0;
        memcpy(out + at, m, 8); at += 8;
        for (int k = 0; k < nl; k++) { size_t l = strlen(lines[k]); if (at + l + 1 > cap) return 0; memcpy(out + at, lines[k], l + 1); at += (u32)l + 1; }
        if (at + tail > cap) return 0; memcpy(out + at, m + p, tail); at += tail;
        wr32(e + 0x24, at - start); wr32(e + 0x28, start);
    }
    if (!changed) return 0;
    wr32(out + 0x0C, at);
    return at;
}
int tut_is_text(const char *path) { const char *t = strstr(path, "tutorial/"); size_t l = strlen(path); return t && l > 4 && !strcmp(path + l - 4, ".bm2"); }
int tut_slack(void) { return TUT_SLACK; }
