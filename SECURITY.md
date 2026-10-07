# Security policy

## Supported versions

Only the latest release receives security fixes.

## Reporting a vulnerability

**Please don't open a public issue for security problems.**

Report them privately through GitHub:
[**Report a vulnerability**](https://github.com/jdkuk/AirGlass/security/advisories/new). Include:

* what an attacker can do (for example code execution from a malicious device on the LAN, or a crash)
* steps or a proof-of-concept to reproduce it
* the AirGlass version and Windows version

You'll get a reply within a few days. A fix will be released as soon as possible, and you'll be credited in the release
notes unless you'd rather not be.

## Threat model in short

* AirGlass accepts connections from **any device on the local network** (like an Apple TV set to "Everyone on the same
  network"). Everything parsed from the network is untrusted: RTSP, binary plists, RTP and mirroring packets, mDNS.
* The control pipe only accepts the **current Windows user** on the local machine.
* Internet access is limited to the Lemon Squeezy license API, over HTTPS.
