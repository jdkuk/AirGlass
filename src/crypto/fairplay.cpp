#include "crypto/fairplay.h"

#include <cstring>

#include "crypto/fairplay_tables.h"

extern "C" {
#include "playfair.h"
}

bool FairPlay::Setup(const uint8_t* req, size_t len, uint8_t reply[142]) {
    if (len != 16 || req[4] != 0x03) return false;
    int mode = req[14];
    if (mode < 0 || mode > 3) return false;
    std::memcpy(reply, kReplyMessage[mode], 142);
    haveKeyMsg_ = false;
    return true;
}

bool FairPlay::Handshake(const uint8_t* req, size_t len, uint8_t reply[32]) {
    // Byte 12 is the key-message mode (0..3); playfair indexes tables with it.
    if (len != 164 || req[4] != 0x03 || req[12] > 3) return false;
    std::memcpy(keyMsg_, req, 164);
    haveKeyMsg_ = true;
    std::memcpy(reply, kFpHeader, 12);
    std::memcpy(reply + 12, req + 144, 20);
    return true;
}

bool FairPlay::DecryptKey(const uint8_t* ekey, size_t len, uint8_t aesKey[16]) {
    if (!haveKeyMsg_ || len < 72) return false;
    uint8_t msg[164];
    uint8_t cipher[72];
    std::memcpy(msg, keyMsg_, sizeof(msg));
    std::memcpy(cipher, ekey, sizeof(cipher));
    playfair_decrypt(msg, cipher, aesKey);
    return true;
}

void FairPlay::SetKeyMessage(const uint8_t msg[164]) {
    std::memcpy(keyMsg_, msg, 164);
    haveKeyMsg_ = true;
}
