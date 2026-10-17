# Third-party components

Screen Mirror Server as a whole is distributed under the GNU General Public License v3 (see
LICENSE), because it is built on UxPlay. It contains or links the following components.

| Component | Used for | License | Where |
|---|---|---|---|
| [UxPlay](https://github.com/FDH2/UxPlay) protocol library (from RPiPlay, shairplay, dsafa22's work) | AirPlay protocol: RTSP/HTTP, pairing, FairPlay handshake, mirroring, audio RTP, HLS, mDNS | GPL-3.0 (parts LGPL-2.1) | `src/airplay` (modified, see `docs/uxplay-lib-changes.patch`); upstream in `third_party/UxPlay` |
| playfair | FairPlay key exchange | GPL-3.0 | `src/airplay/playfair` |
| [llhttp](https://github.com/nodejs/llhttp) | HTTP/RTSP parser | MIT | `src/airplay/llhttp` |
| [FFmpeg](https://ffmpeg.org) libavcodec/libavutil, built without GPL or non-free parts | AAC-ELD (mirroring) and ALAC (speaker) audio decoding; in the test sender, reading audio files | LGPL-2.1+ | fetched and built by vcpkg |
| [OpenSSL](https://www.openssl.org) | cryptography | Apache-2.0 | fetched and built by vcpkg |
| [libplist](https://github.com/libimobiledevice/libplist) | Apple property lists | LGPL-2.1+ | fetched and built by vcpkg |
| [OpenH264](https://github.com/cisco/openh264) | test sender only: H.264 test video | BSD-2-Clause | fetched and built by vcpkg |

The screenshots in `docs/screenshots` use only material made for this project (the web page in
`tools/demo/demo-page.html`, the cover art and the synthesized recording of Pachelbel's Canon in D
from `tools/demo/make-demo-media.ps1`) and NASA's public-domain video "Earth Views from the ISS -
No Music" (images-assets.nasa.gov), which is streamed from NASA and not included here.

"AirPlay", "iPhone" and "iPad" are trademarks of Apple Inc. This project is not affiliated with or
endorsed by Apple, Microsoft or NASA.
