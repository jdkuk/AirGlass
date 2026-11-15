# Third-party notices

AirGlass includes the following components.

## playfair (FairPlay SAP handshake), via UxPlay / RPiPlay

* Source: https://github.com/FDH2/UxPlay (`lib/playfair`), originally from RPiPlay
* License: GNU General Public License v3.0 (`third_party/playfair/LICENSE.md`, also `LICENSE`)
* AirPlay protocol details were also learned from UxPlay and RPiPlay. Because of this, AirGlass as
  a whole is distributed under GPL-3.0.
* PIN pairing ("AirPlay Code", `src/crypto/srp.*` and the pair-pin-start / pair-setup-pin handlers) follows
  UxPlay's `-pin` support (`lib/srp.c`, `lib/pairing.c`, `lib/raop_handlers.h`, GPL-3.0; its SRP code is
  Tom Cocagne's csrp, MIT, modified by fduncanh for Apple's variant). AirGlass's SRP-6a, big-number arithmetic and
  AES-GCM are its own code, written from RFC 5054 and NIST SP 800-38D; no UxPlay or csrp code is copied.

## ed25519 (Orson Peters)

* Source: https://github.com/orlp/ed25519
* License: zlib (`third_party/ed25519/LICENSE.txt`)

## FFmpeg (libavcodec, libavutil, libswresample)

* Version: n8.1, Windows LGPL shared build from https://github.com/BtbN/FFmpeg-Builds
* License: GNU Lesser General Public License (see `FFmpeg-LICENSE.txt` next to the DLLs)
* Source: https://github.com/FFmpeg/FFmpeg/tree/n8.1 and the build scripts at
  https://github.com/BtbN/FFmpeg-Builds
* AirGlass links to FFmpeg dynamically. You may replace the DLLs with your own compatible build.

AirPlay, iPhone, iPad and Mac are trademarks of Apple Inc. AirGlass is not affiliated with or
endorsed by Apple.
