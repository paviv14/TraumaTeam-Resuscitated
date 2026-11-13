// A real Wii Remote over Bluetooth HID (wiimote_hid.c).
#pragma once
typedef struct {
    unsigned short buttons;   // KPAD button bits (Nunchuk C / Z included)
    float acc[3];             // KPAD axes, in g
    int ndots; int dot[4][2]; // IR camera dots (1024 x 768)
    int nunchuk; float stick[2], nacc[3];
} WiiState;
int whid_poll(WiiState *out);            // 1 while a Remote is connected (looks for one every few seconds)
void whid_rumble(int on);
int whid_pointer(const WiiState *s, float *px, float *py, float *hx, float *hy, float *dist);   // dots seen (0: none)
