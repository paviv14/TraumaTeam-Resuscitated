// Text and voice language.
//
// The US disc already holds the game's Japanese text: every text file exists in an eng and a jpn folder,
// and the program picks the folder from the Wii's system language (SYSCONF item IPL.LNG: 0 Japanese,
// 1 English). Only the voices differ: the US disc has sound/eng, the Japanese disc (HOSPITAL. 6-nin no
// Ishi) has sound/jpn, with the same file names. So:
//   text  - the language written into the SYSCONF the game reads at boot (nand/shared2/sys/SYSCONF);
//   voice - the disc's sound/<text language> folder is served from either disc: its file-table entries
//           are pointed at the Japanese disc's files when Japanese voices are chosen, and the two
//           language folders swap names when the text is Japanese (the US disc's jpn folder is empty).
// The Japanese disc is looked for next to the US one: <disc>_jp (e.g. disc and disc_jp), TTPC_DISC_JP,
// or bin/disc_jp.txt. Without it Japanese voices fall back to English.
#include "cpu.h"
#include "platform.h"
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern const char *g_nand_root;

// ---------------------------------------------------------------- SYSCONF: IPL.LNG
// Format: "SCv0", u16 count, u16 offset[count + 1] (the last is where the items end), items, zeros,
// "SCed" in the last 4 bytes of 0x4000. Item: u8 (type << 5 | name length - 1), name, data.
static int item_len(const u8 *d, int o) {
    int t = d[o] >> 5, nl = (d[o] & 31) + 1, p = o + 1 + nl;
    switch (t) {
    case 1: return 1 + nl + 2 + ((d[p] << 8 | d[p + 1]) + 1);   // big array
    case 2: return 1 + nl + 1 + (d[p] + 1);                     // small array
    case 3: case 7: return 1 + nl + 1;                          // byte, bool
    case 4: return 1 + nl + 2; case 5: return 1 + nl + 4; case 6: return 1 + nl + 8;
    }
    return -1;
}
void lang_sysconf(int lang) {   // lang: 0 Japanese, 1 English
    char path[600]; snprintf(path, sizeof path, "%s/shared2/sys/SYSCONF", g_nand_root);
    u8 in[0x4000] = { 0 }, out[0x4000] = { 0 };
    FILE *f = fopen(path, "rb"); int have = f && fread(in, 1, sizeof in, f) == sizeof in && !memcmp(in, "SCv0", 4);
    if (f) fclose(f);
    int n = have ? (in[4] << 8 | in[5]) : 0, cur = -1;
    // copy every item except IPL.LNG, then add it
    int on = 0, offs[256], lens[256]; const u8 *items[256];
    for (int k = 0; k < n && k < 250; k++) {
        int o = in[6 + 2 * k] << 8 | in[7 + 2 * k], l = item_len(in, o);
        if (l <= 0 || o + l > 0x3FFC) { have = 0; break; }
        if ((in[o] & 31) + 1 == 7 && !memcmp(in + o + 1, "IPL.LNG", 7)) { cur = in[o + 8]; continue; }
        items[on] = in + o; lens[on] = l; on++;
    }
    if (cur == lang) return;   // already right
    static const u8 lng[] = { 3 << 5 | 6, 'I', 'P', 'L', '.', 'L', 'N', 'G', 0 };
    u8 mine[sizeof lng]; memcpy(mine, lng, sizeof lng); mine[8] = (u8)lang;
    items[on] = mine; lens[on] = sizeof mine; on++;
    memcpy(out, "SCv0", 4); out[4] = on >> 8; out[5] = (u8)on;
    int o = 6 + 2 * (on + 1);
    for (int k = 0; k < on; k++) { offs[k] = o; memcpy(out + o, items[k], lens[k]); o += lens[k]; }
    for (int k = 0; k < on; k++) { out[6 + 2 * k] = offs[k] >> 8; out[7 + 2 * k] = (u8)offs[k]; }
    out[6 + 2 * on] = o >> 8; out[7 + 2 * on] = (u8)o;
    memcpy(out + 0x3FFC, "SCed", 4);
    char dir[600]; snprintf(dir, sizeof dir, "%s/shared2/sys/x", g_nand_root);
    for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; host_mkdir(dir); *q = '/'; }
    f = fopen(path, "wb");
    if (f) { fwrite(out, 1, sizeof out, f); fclose(f); }
    fprintf(stderr, "[lang] system language set to %s%s\n", lang ? "English" : "Japanese", f ? "" : " (could not write SYSCONF)");
}

// ---------------------------------------------------------------- the Japanese disc
static int is_file(const char *p, long long *size) {
    struct stat st; if (stat(p, &st) || !(st.st_mode & S_IFREG)) return 0;
    if (size) *size = (long long)st.st_size; return 1;
}
static int jp_ok(const char *root) {
    char p[800]; snprintf(p, sizeof p, "%s/files/sound/jpn/sys/00000000/static.brsar", root);
    return is_file(p, NULL);
}
// Finds the Japanese disc's root (the folder with sys and files); "" if there is none.
const char *lang_jp_root(const char *us_root) {
    static char root[600]; static int done; if (done) return root; done = 1; root[0] = 0;
    char c[600];
    const char *e = getenv("TTPC_DISC_JP");
    if (e && *e) { snprintf(c, sizeof c, "%s", e); if (jp_ok(c)) { snprintf(root, sizeof root, "%s", c); return root; }
                   snprintf(c, sizeof c, "%s/DATA", e); if (jp_ok(c)) { snprintf(root, sizeof root, "%s", c); return root; } }
    FILE *f = fopen("disc_jp.txt", "r");
    if (f) {
        if (fgets(c, sizeof c, f)) {
            c[strcspn(c, "\r\n")] = 0; char d[620];
            if (jp_ok(c)) snprintf(root, sizeof root, "%s", c);
            else { snprintf(d, sizeof d, "%s/DATA", c); if (jp_ok(d)) snprintf(root, sizeof root, "%s", d); }
        }
        fclose(f); if (root[0]) return root;
    }
    // <disc>_jp next to the US disc folder (disc/DATA -> disc_jp/DATA)
    snprintf(c, sizeof c, "%s", us_root);
    size_t l = strlen(c); while (l && (c[l - 1] == '/' || c[l - 1] == '\\')) c[--l] = 0;
    char *tail = NULL; if (l >= 5 && (!strcmp(c + l - 5, "/DATA") || !strcmp(c + l - 5, "\\DATA"))) { tail = c + l - 5; *tail = 0; }
    char d[700];
    snprintf(d, sizeof d, "%s_jp/DATA", c); if (jp_ok(d)) { snprintf(root, sizeof root, "%s", d); return root; }
    snprintf(d, sizeof d, "%s_jp", c); if (jp_ok(d)) { snprintf(root, sizeof root, "%s", d); return root; }
    return root;
}
int lang_jp_size(const char *jp_root, const char *rest, long long *size) {   // rest: path below sound/<lang>/
    char p[900]; snprintf(p, sizeof p, "%s/files/sound/jpn/%s", jp_root, rest);
    return is_file(p, size);
}
