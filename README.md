# Crash Bandicoot — native iOS port (armv7, 32-bit)

Static recompilation of Crash Bandicoot (PS1) for **32-bit iOS devices**.
The game code is recompiled ahead of time into plain C and built as a regular
native app: no JIT, no interpreter, no emulator core at runtime.

This is the main README of the repository. The old iPhone 8 / iOS 14.7 /
TrollStore description has moved to [Other targets](#other-targets-in-this-repo).

> **No game data is included.** You need your own legally dumped disc image
> (`.cue` + `.bin`). This project is unofficial and is not affiliated with
> or endorsed by the rights holders.

---

## Requirements

| | |
|---|---|
| **iOS** | 7.0 or newer (built against the iPhoneOS 9.3 SDK, Mach-O min version 7.0) |
| **CPU** | **Apple A5 or newer recommended** (armv7 + OpenGL ES 2.0) |
| **Landscape** | The app runs in landscape only |
| **Disc image** | Your own `.cue` + `.bin` |

Typical A5-class devices: iPhone 4S, iPad 2, iPad mini (1st gen), iPod touch 5.
A6 and newer 32-bit-capable devices (iPhone 5/5c, iPad 4, …) should run it faster.

Notes:

- Developed and tuned against an **iPad mini 1 (A5, iOS 9.3.5)**. Other devices
  are expected to work but have not all been tested.
- 64-bit devices can run this armv7 build only while their iOS version still
  supports 32-bit apps (up to iOS 10). iOS 11+ dropped 32-bit apps entirely.
  For 64-bit iOS there is a separate, experimental .NET AOT target (TrollStore).

---

## Install

You get an `.ipa` and a `.deb` from the build (see **Building**).

**Jailbroken device (simplest)**
1. Copy the `.deb` to the device and install it (Filza / `dpkg -i`).
2. Respring if the icon doesn't appear.

**Not jailbroken**
1. Sign and install the `.ipa` with your own Apple ID using a sideloading tool
   that works with your iOS version.
2. Free Apple ID signatures expire after 7 days and must be renewed.

---

## Adding the game

1. Connect the device to a computer (iTunes / Finder file sharing), or use
   Filza on a jailbroken device.
2. Copy your **`.cue` and `.bin`** into the app's **Documents** folder.
   - Make sure the `FILE "..."` line in the `.cue` matches the real `.bin` name.
3. Launch the app. It boots the first `.cue` it finds in Documents.

If no `.cue` is found, the app shows a message telling you to add one.

---

## Controls

An on-screen PlayStation pad is drawn over the game:

| Area | Buttons |
|------|---------|
| Top left | L1, L2 |
| Top right | R1, R2 |
| Bottom left | D-pad (slide your thumb across it) |
| Bottom right | Triangle, Circle, Cross, Square |
| Bottom centre | SELECT, START |

Multitouch is supported (jump + spin at the same time).

**MFi / Bluetooth gamepads** are supported through `GameController.framework`
on iOS versions that include it (Extended and basic gamepad profiles).

---

## Saves

- Memory cards are stored in `Documents/Saves/` as `card1.mcr` and `card2.mcr`.
- Saves are written atomically (temp file, then rename), so a kill mid-write
  won't corrupt them.
- Memory cards are flushed when the app goes to the background.
- The game pauses (video and audio) when the app loses focus.

To back up your progress, copy the `Saves` folder out of Documents.

---

## Troubleshooting

The app writes `Documents/crash.log`. It records startup info, fatal errors
and a freeze watchdog (if no frame is presented for 3+ seconds it logs where
the game code is stuck). Attach this file when reporting a bug.

| Problem | What to check |
|---------|---------------|
| "Put your disc image…" message | `.cue` is not directly inside **Documents** |
| Black screen / instant exit | `.bin` name doesn't match the `.cue`, or the dump is bad |
| Slow / choppy | Expected on the oldest A5 devices; close other apps, reboot the device |
| App won't install on iOS 7/8 | Your signing tool may not support that iOS version; try a different one or jailbreak install |

---

## Building

The disc is only used at build time to recompile the game executable. The
resulting app contains **no game data**, which is why you still add the disc
to Documents on the device.

### Option A — GitHub Actions (no Mac needed)

1. Open **Actions → "Build Crash Bandicoot (pick target)" → Run workflow**.
2. Choose target **`ios-armv7-native`**.
3. Provide your disc via the `cue_file_id` / `bin_file_id` fields (Google Drive
   file IDs, sharing set to "Anyone with the link"), or save them once as repo
   secrets `DRIVE_CUE_ID` / `DRIVE_BIN_ID`.
4. Download the **`CrashBandicoot-armv7-iOS9`** artifact (`.ipa` + `.deb`).

### Option B — locally (Linux or macOS)

Requirements: .NET SDK (10), Python 3, [Theos](https://theos.dev) with an iOS
toolchain and the **iPhoneOS 9.3 SDK**.

```bash
# 1. Disc -> C (ELF -> C# -> C), run once
./scripts/prerecompile_native.sh /path/to/game.cue

# 2. Build
cd CrashBandicoot.Native
make package FINALPACKAGE=1
```

The `.deb` ends up in `CrashBandicoot.Native/packages/`. To make an `.ipa`,
zip the built `CrashBandicootARMv7.app` inside a `Payload/` folder.

Generated sources (`CrashBandicoot.Native/gen/`) are derived from your disc and
are **never committed**.

---

## Project layout

| Path | What it is |
|------|------------|
| `CrashBandicoot.Native/src/` | Runtime in C: CPU dispatch, GTE, GPU (OpenGL ES 2), SPU, MDEC, CD-ROM, memory cards |
| `CrashBandicoot.Native/host/ios_host.m` | UIKit / EAGL / AudioQueue host, touch pad, gamepads, watchdog |
| `CrashBandicoot.Native/tools/cs2c.py` | Converts recompiler C# output into C |
| `CrashBandicoot.Native/Makefile` | Theos build (armv7, iOS 7.0 minimum) |
| `tools/CrashBandicoot.PreRecompiler/` | Disc ELF → C# recompiler front-end |
| `RecompOne.*` | Recompiler and reference runtime shared with Windows / Android |

Rendering uses OpenGL ES 2.0 only (no software rasterizer on iOS). Audio goes
through AudioQueue (44.1 kHz stereo).

---

## Known limitations

- No launcher, settings screen or disc picker: the game starts immediately.
- No mods, cheats or dev menu in this build.
- No controller rumble.
- Performance on A5 is the limiting factor; expect it to be the minimum spec,
  not a comfortable one.

---

## Other targets in this repo

The same recompiler also feeds other hosts. Pick the target in the
**"Build Crash Bandicoot (pick target)"** workflow.

| Target | Platform | Notes |
|--------|----------|-------|
| `ios-armv7-native` | **32-bit iOS 7.0 – 9.3.5** | **Main target of this README.** C + Theos, `.ipa` / `.deb` |
| `ios-arm64-trollstore` | 64-bit iOS (iPhone 8 / iOS 14.7 tested target) | Experimental. .NET AOT, installed via TrollStore. See `CrashBandicoot.IosHost/README.md` |
| `android-apk` | Android | `AndroidRuntimeHost`, no disc needed at build time |
| `smoke-tests-only` | x86_64 | Runtime tests only |

Pick **armv7** for iPhone 4S / 5 / 5c, iPad 2 / 3 / 4, iPad mini 1, iPod touch 5.
Pick **arm64 TrollStore** only if you are on a 64-bit device with iOS 14+ and
TrollStore.

---

## License

MIT. See [LICENSE](LICENSE). Game assets and code generated from your disc
remain the property of their respective owners and are not distributed here.
