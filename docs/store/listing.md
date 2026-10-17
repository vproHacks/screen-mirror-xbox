# Microsoft Store listing: Screen Mirror Server

Text for Partner Center > Screen Mirror Server > Store listings > English (United States), in the
order of the form. Images are listed at the end.

## Product name

Screen Mirror Server

## Description

Screen Mirror Server turns your Xbox into a receiver for your iPhone or iPad. There is nothing to install on the phone and no account to create: open the app on the Xbox and it appears on your phone.

• Mirror your screen: open Control Center on your iPhone or iPad, tap Screen Mirroring and choose your Xbox. Your screen and its sound appear on the TV, in portrait or landscape, with low lag.
• Play music: choose your Xbox with the AirPlay button in Music, podcasts or other audio apps. The TV shows the title, artist, album, cover art and progress, and the sound plays through your TV or sound system in full quality.
• Cast videos: send YouTube and web videos to the TV in full resolution, and play, pause and seek from your phone.
• Use your phone's volume buttons to set the volume.

Your phone and your Xbox must be on the same network, and the app must be open on the Xbox while you use it. Everything stays on your local network: the app has no accounts, ads or tracking.

Not supported: apps that protect their content (such as Apple TV+, Netflix and Disney+) do not allow mirroring or casting to receivers like this one, and videos in formats the Xbox cannot decode (HEVC, VP9, AV1) do not play.

Screen Mirror Server is free and open source (GNU GPL v3), built on the UxPlay project. Source code: https://github.com/vproHacks/screen-mirror-xbox

AirPlay, iPhone and iPad are trademarks of Apple Inc. This app is not affiliated with or endorsed by Apple.

## What's new in this version

(leave blank: first submission)

## Product features

1. Mirror your iPhone or iPad screen to the TV
2. Portrait and landscape, following your phone as you rotate it
3. Low-lag video, decoded in hardware, up to 1080p
4. Your phone's sound plays in sync with the picture
5. Play music from your phone, with cover art, title and progress on the TV
6. Lossless audio for music
7. Cast YouTube and web videos in full resolution
8. Play, pause and seek from your phone
9. Volume follows your phone's volume buttons
10. Shows up on your phone automatically: no pairing or codes
11. No account, ads or tracking
12. Free and open source

## Screenshots (Xbox, 1920 x 1080 PNG)

From `build\screenshots` in the project folder, with captions:

| File | Caption |
|---|---|
| `1-ready.png` (already uploaded) | Open the app, then choose your Xbox in Screen Mirroring on your phone |
| `2-music.png` | Music from your phone, with cover art and progress |
| `3-website.png` | Your phone's screen on the TV, in portrait or landscape |
| `4-video.png` | Cast videos to the TV in full resolution |

## Store logos

From `docs\store\art`:

| Slot | File |
|---|---|
| 9:16 Poster art (1440 x 2160) | `poster-1440x2160.png` |
| 1:1 Box art (2160 x 2160) | `box-2160x2160.png` |
| Store display images, 1:1 300 x 300 / 150 x 150 / 71 x 71 (optional) | `tile-300.png`, `tile-150.png`, `tile-71.png` |

## Trailers and additional assets

No trailer. Images (optional, recommended for Xbox), from `docs\store\art`:

| Slot | File |
|---|---|
| 16:9 Super hero art (3840 x 2160, no title) | `super-hero-3840x2160.png` |
| Xbox Branded key art (584 x 800) | `branded-key-art-584x800.png` |
| Xbox Titled hero art (1920 x 1080) | `titled-hero-1920x1080.png` |
| Xbox Featured promotional square art (1080 x 1080, no title) | `featured-square-1080x1080.png` |

## Supplemental fields

**Short title:** Screen Mirror

**Voice title:** Screen Mirror Server

**Short description:**

Mirror your iPhone or iPad to the TV, play music from your phone with cover art, and cast YouTube and web videos to your Xbox. Free and open source, with no accounts, ads or tracking.

## Additional information

**Keywords** (7):

- screen mirroring
- mirror phone to TV
- cast to TV
- screen sharing
- wireless display
- music streaming
- video casting

**Copyright and trademark info:**

© 2026 vpro8725. Free software under the GNU GPL v3, built on UxPlay. AirPlay, iPhone and iPad are trademarks of Apple Inc.; this app is not affiliated with Apple.

**Additional license terms:**

Screen Mirror Server is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License, version 3 or later. Its complete source code, the license text and the notices for the components it includes (UxPlay, FFmpeg, OpenSSL, libplist, llhttp) are available at https://github.com/vproHacks/screen-mirror-xbox.

**Developed by:** vpro8725

## Other pages of the submission

- **Properties:** category Utilities & tools; privacy policy URL
  https://github.com/vproHacks/screen-mirror-xbox/blob/main/PRIVACY.md
- **Packages:** `build\AppPackages\ScreenMirrorServer_<version>_x64.msixupload`
- **Pricing and availability:** Free; visibility Private audience (or Public, when ready)
- **Additional testing information** (notes for the certification testers):

  This is a receiver for screen mirroring from Apple devices (the AirPlay protocol). To test it, open the app on an Xbox, then on an iPhone or iPad on the same network: open Control Center, tap Screen Mirroring and choose "Xbox One". The phone's screen and sound appear on the TV. Music: in the Music app, tap the AirPlay button and choose "Xbox One". Video: in the YouTube app, tap the AirPlay/cast button and choose "Xbox One". The app needs no account or sign-in. It listens on the local network only (TCP port 7000 and mDNS), so the phone and the Xbox must be on the same network.
