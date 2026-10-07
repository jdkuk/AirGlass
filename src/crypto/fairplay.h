// FairPlay SAP v2.5 handshake used by AirPlay for the session key exchange.
#pragma once

#include <cstddef>
#include <cstdint>

class FairPlay {
public:
    // Step 1: 16-byte request -> 142-byte reply. Returns false on unsupported request.
    bool Setup(const uint8_t* req, size_t len, uint8_t reply[142]);
    // Step 2: 164-byte request -> 32-byte reply; stores the key message.
    bool Handshake(const uint8_t* req, size_t len, uint8_t reply[32]);
    // Decrypt the 72-byte "ekey" from SETUP into the 16-byte AES session key.
    bool DecryptKey(const uint8_t* ekey, size_t len, uint8_t aesKey[16]);
    bool Ready() const { return haveKeyMsg_; }
    // Test helper: install a key message directly.
    void SetKeyMessage(const uint8_t msg[164]);

private:
    uint8_t keyMsg_[164]{};
    bool haveKeyMsg_ = false;
};
