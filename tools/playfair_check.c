// Determinism check for playfair_decrypt: same inputs must give the same key, every call.
#include <stdio.h>
#include <string.h>
#include "playfair.h"

int main(void) {
    unsigned char msg[164], ekey[72], out1[16], out2[16], out3[16];
    int bad = 0;
    for (int mode = 0; mode < 4; mode++) {
        for (int i = 0; i < 164; i++) msg[i] = (unsigned char)(i * 37 + 11);
        for (int i = 0; i < 72; i++) ekey[i] = (unsigned char)(i * 13 + 5);
        msg[4] = 3;
        msg[12] = (unsigned char)mode;  // FairPlay mode lives in byte 12 of the key message
        unsigned char m[164], e[72];
        memcpy(m, msg, 164); memcpy(e, ekey, 72); playfair_decrypt(m, e, out1);
        memcpy(m, msg, 164); memcpy(e, ekey, 72); playfair_decrypt(m, e, out2);
        memcpy(m, msg, 164); memcpy(e, ekey, 72); playfair_decrypt(m, e, out3);
        printf("mode %d: ", mode);
        for (int i = 0; i < 16; i++) printf("%02x", out1[i]);
        int same = !memcmp(out1, out2, 16) && !memcmp(out2, out3, 16);
        printf(" %s\n", same ? "deterministic" : "NON-DETERMINISTIC");
        if (!same) bad = 1;
    }
    return bad;
}
