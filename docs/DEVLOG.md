# Development log

Goal: parity with UxPlay's features on the Xbox One (Dev Mode) as a UWP app: screen mirroring,
AirPlay speaker (audio-only) and AirPlay video casting. See README.md for usage.

## Status (2026-10-18, overnight)

| Feature | Status | How it was verified |
|---|---|---|
| Discovery (mDNS, built-in responder) | Works on Xbox | iPhone sees "Xbox One"; `fakesender --discover` from the PC |
| Screen mirroring, video | Works on Xbox | Real iPhone (smooth, low lag, follows rotation); fake sender: 3 back-to-back sessions with rotation, 0 dropped frames |
| Screen mirroring, audio (AAC-ELD) | Works on Xbox | Real iPhone; fake sender: 0 decode errors |
| Volume from the phone | Works | Real iPhone volume buttons |
| AirPlay speaker (ALAC) + now playing | Works on Xbox | Real iPhone (Music, with cover art); fake sender: 0 decode errors, damaged packets rejected; title/artist/album/cover art/progress shown |
| Screen mirroring of video content (web video, reels) | Works on Xbox | Real iPhone: in sync with the audio, no freezes, frames shown 0 ms late (after the SPS rewrite) |
| Video casting, direct URL (Safari style) | Works on Xbox | Fake sender: Apple HLS test stream, play/seek/pause/resume/stop |
| Video casting, YouTube app (HLS via FCUP) | Works on Xbox | Real iPhone; fake sender relaying Apple's test stream through FCUP, also with only its packed-audio track |
| YouTube ad → video | Slow (TODO) | Real iPhone: the video starts ~10 s after the ad ends; the ad's Skip button starts it at once |
| Mirroring → casting switch | Works on Xbox | Fake sender |
| Leaving the app and returning | Works | App suspends (stops advertising) and restarts its service when reopened |
| Long session | Works on Xbox | 5 min of 1080p mirroring + audio: 9,608/9,608 frames, 0 audio errors, player ~50-80 ms behind the newest frame without drift, memory 15.5 MB → 19.4 MB |

Not yet confirmed with a real iPhone (the fake sender covers the protocol, but iOS may differ):
a long speaker session after the decoder change (see the log below), and casting a web video
(Safari's AirPlay button).

## TODO
- **YouTube ad → video transition takes ~10 s.** The YouTube app plays an ad and the video as
  separate AirPlay items. What was tried, with real-iPhone results:
  - Ending the session when an item ends (UxPlay's behaviour: `/playback-info` duration -1, the
    library resets and disconnects): the phone reconnects, removes the ad, waits ~15 s, then sends
    `/stop` and a new `/play` for the video.
  - Reporting the item as paused at its end and keeping the session: the phone never moves on (it
    re-plays the ad at its end position, then gives up).
  - Current: paused at its end, plus a `POST /event` state "stopped" on the reverse connection
    (`raop_send_video_event`, as an Apple TV / Kodi send it). The phone reacts at once (closes its
    connections) but only sends the next `/play` ~7.5 s later; the video shows ~11 s after the ad.
  - The phone never sent `playlistInsert` (which UxPlay does not implement either: "FIXME").
  - Ideas: send "playing"/"paused"/"loading" events during playback too (an Apple TV reports every
    state change, and the phone may be waiting for "loading" or for the item's own end handling);
    compare with a packet capture of an Apple TV doing the same transition.
  - Workaround: the ad's Skip button starts the video at once.

## Environment facts
- Development PC: Windows 11, wired Ethernet, network profile Private, Windows Developer Mode on.
- Test console: Xbox One (first generation), Dev Mode, OS 26100.9445 (`xb_flt_2608ge`), on Wi-Fi.
  Device Portal with authentication disabled, driven by `tools/xbox.sh`.
  Its Wi-Fi adapter is named "Durango Virtual Network WiFi Adapter".
  Launching apps needs a signed-in Xbox profile (error 0x8004090A otherwise).
- Toolchain: VS 2022 Build Tools 17.14 (MSVC 14.44, UWP C++ tools), Windows SDK 10.0.26100.
- vcpkg lives in `tools/vcpkg`, packages in `tools/vcpkg_installed`; `tools/vcpkg-env.sh` keeps all
  vcpkg state inside this folder.
  - UWP (`x64-uwp-static-md`): openssl 3.6.5, libplist 2.8.0, ffmpeg 9.0.2 (avcodec).
  - Desktop test tools (`x64-windows-static-md`): openssl, libplist, openh264, ffmpeg 9.0.2
    (avcodec, avformat, swresample).
- Upstream UxPlay is cloned at `third_party/UxPlay` (commit 4e95a9e, 2026-10-16), unmodified.
  `src/airplay` is its `lib/` with patches: `docs/uxplay-lib-changes.patch`.

## Changes to the UxPlay library (src/airplay)
- MSVC/UWP portability: pthread/unistd shims (src/compat), `snprintf` macro only for old MSVC,
  `SOL_TCP` on Windows, keyfile I/O through memory BIOs (UWP OpenSSL has no stdio),
  `RAND_bytes` instead of CryptoAPI in srp.c, kernel receive timestamps disabled under UWP.
- mDNS: the adapter filter rejected every adapter named "...Virtual...", which on Xbox is the real
  Wi-Fi/Ethernet adapter ("Durango Virtual Network ... Adapter"); plus a routing-table fallback.
  The mDNS host name is "<hostname>-AirPlay.local" so it never collides with the console's own.
- Mirroring: the stream socket is shut down on stop (on Windows SO_RCVTIMEO with a timeval means
  "no timeout", so a blocked recv() could hang the stop); a closed header socket is now closed.
- FairPlay: reject a mode byte > 3 instead of reading past the reply table.
- httpd (upstream bug): on a reversed (PTTH) connection, new requests were never marked as
  reverse responses (`http_request_is_reverse` was called instead of `..._set_reverse`), so the
  client's "HTTP/1.1 200 OK" to the first FCUP request was parsed as a request, the connection was
  dropped, and YouTube casting stopped after the first playlist. Worth reporting upstream.
- New `raop_get_hls_playlist()` so the app can feed HLS playlists to the player from memory
  (UWP apps cannot use the loopback HTTP channel UxPlay normally uses for GStreamer).
- New `raop_send_video_event()`: a playback state event (`POST /event`, plist with category
  "video" and a state) to the client over the reverse connection, as an Apple TV sends; used for
  "stopped" when a cast item plays to its end.

## Log
- 2026-10-17 evening: toolchain installed, deps built, Device Portal reachable, porting started.
- First UWP build ran on the PC and on the Xbox; the user mirrored a real iPhone to both. Mirroring
  is smooth with low lag, rotation works, audio and volume work.
- Pointer-mode cursor hidden; debug-level logging only with `debug.txt`.
- Fake iPhone sender written (test/fakesender) and used to test everything on the Xbox overnight.
  (Its first version derived the wrong key: the FairPlay mode byte in the key message must be 0-3.)
- A frozen-picture report turned out to be the phone showing a static placeholder after a web
  video had been routed to the receiver as audio-only; a still screen and reused players were
  tested and refresh correctly. The player's clock vs. our timestamps is now logged every 3 s
  ("video diag") in case a real freeze ever happens.
- Added (UxPlay parity): AirPlay speaker mode (ALAC) with a now-playing screen; AirPlay video
  casting (direct URLs and YouTube-style HLS) via the Xbox media player; feature bits 0 and 4 on.
  - Apple's ALAC endian detection needs TARGET_OS_WIN32 under MSVC (later replaced, see below).
  - While casting, /playback-info polls count as the client heartbeat.
- The PC test copy now defaults to "<computer name> (PC)" so it never appears as a second "Xbox One".
- Store identity (vpro8725.ScreenMirrorServer, "Screen Mirror Server") in the manifest; the
  Phone identity element is required by the UWP packaging targets, so it stays with a new GUID.
- fdk-aac (not GPL-compatible) replaced by FFmpeg's native AAC decoder for AAC-ELD, as UxPlay
  does through GStreamer's avdec_aac. FFmpeg's build cannot run from a path with spaces
  (`tools/setup-deps.sh` builds through a temporary `subst` drive), and its static libavcodec
  needs mfuuid.lib and strmiids.lib (Media Foundation GUIDs) at link time.
- 2026-10-17: the user's iPhone was playing music in speaker mode when the app vanished. The log
  ended right after "now playing", with no suspend line, no unhandled-exception line and no
  report from the system: a native crash. Apple's reference ALAC decoder trusts the sample count
  of a "partial frame" header and writes that many samples into 352-sample buffers; one damaged
  or hostile packet crashes the process this way (reproduced on the PC and on the console with
  `fakesender --corrupt-every`, which makes the old build vanish with the same log ending).
  ALAC is now decoded by FFmpeg's decoder too (as UxPlay does with avdec_alac), which rejects
  such frames; Apple's codec is no longer used anywhere. Whether a damaged packet is what the
  phone's session hit is not proven, so crashes are now logged (below).
- Crash logging: an unhandled native exception, std::terminate, abort() or a CRT parameter error
  writes `CRASH:` lines (code, address, stack as `exe+0x<RVA>`) to airplay.log; the build writes a
  linker map and `tools/symbolize.sh` turns the addresses into function names. Tested on the
  console with a forced access violation and an exception escaping a thread. Fail-fast exits
  (heap corruption checks, /GS) still end the process silently.
- Test sender: no fdk-aac or ALAC library any more. It writes ALAC "escape" (uncompressed) frames
  itself, uses ALAC for mirroring audio too, reads audio files with FFmpeg, mirrors pictures
  (WIC), and has `--name`, `--start-at`, `--just-play` and `--corrupt-every`.
- Store screenshots: `tools/demo/` generates the media (a synthesized Canon in D with cover art,
  a made-up web page rendered at iPhone resolution with headless Edge) and
  `capture-screenshots.sh` records the four screens from the console; the video is NASA's
  public-domain "Earth Views from the ISS - No Music".
- Mirroring froze on busy content (a playing reel, an animated page) and web video audio ran ~1 s
  ahead. Frame counting (MediaStreamSource.SampleRendered) showed every frame reached the player
  but was shown 130-700 ms late, with 20-35 frames inside the decoder: the iPhone's SPS (High
  profile, POC type 0, 4 reference frames) has no VUI bitstream restriction, so the decoder must
  assume frame reordering and holds frames back. iOS never sends B slices, so the SPS is now
  rewritten in flight to declare no reordering (max_num_reorder_frames 0); frames are shown 0 ms
  late and the audio is in sync. A B slice would turn the rewrite off (logged). The rewrite is
  checked against FFmpeg's decoder by `build/tests/h264sps_test.exe`.
- YouTube casting buffered forever: its audio tracks are HLS "packed audio" (an ID3 timestamp tag,
  then raw ADTS AAC, named .ts), which the platform player rejects (0xC00D36CB, "0 bytes
  received"). Audio-rendition segments are now fetched by the app and rewrapped as MPEG-TS
  (`HlsAudio.cpp`, no re-encoding); `build/tests/hlsaudio_test.exe` checks the output with
  FFmpeg, and `fakesender --cast-hls ... --separate-audio` reproduces the case with Apple's
  stream. (The User-Agent was suspected first; a probe showed YouTube serves either way.)
- Casting: the heartbeat watchdog dropped a playing cast when the phone paused its
  `/playback-info` polls. A playing cast now never times out (as on an Apple TV, the phone may be
  locked); 2 minutes of silence after the video is over or failed to load still ends the session.
- A cast item that plays to its end is reported as paused at its end and announced with a
  "stopped" event instead of ending the session (see TODO for the ad → video transition).
