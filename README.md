# Crash Bandicoot — iOS Host (iPhone 8 / iOS 14.7 / TrollStore)

Port of the statically recompiled Crash Bandicoot (PS1) to iOS.
Target device: iPhone 8, iOS 14.7, installed via TrollStore.

> No game data ships with this repo. You need your own disc dump
> (`.cue` + `.bin`).

---

## Quick start

1. Prepare your own disc dump (`game.cue` + `.bin`).
2. Generate C# from the disc (once, on any machine):
   ```bash
   ./scripts/prerecompile.sh /path/to/game.cue
   ```
3. Build the `.ipa`:
   - on a Mac: `dotnet build CrashBandicoot.IosHost -c Release -f net9.0-ios -r ios-arm64 -p:BuildIpa=true`
     (requires Xcode and `dotnet workload install ios`);
   - or via GitHub Actions: `.github/workflows/ios-build.yml`, the artifact is the `.ipa`.
4. Install the `.ipa` on the iPhone via TrollStore.
5. Using Files.app, copy `game.cue` and the `.bin` into the app's **Documents** folder.
6. Launch the game.

---

## Controls

An on-screen gamepad is drawn over the game (`TouchControllerView.cs`).

| Area | Buttons |
|------|---------|
| Top left | L2, L1 |
| Top right | R2, R1 |
| Bottom left | D-pad, 8 directions (slide your thumb across it) |
| Bottom right | △ ○ ✕ □ |
| Bottom centre | SELECT, START |

- Touching between two buttons presses both (e.g. jump + spin).
- A pressed button is highlighted and triggers a light haptic tap.
- Sizes and positions adapt to the screen and safe area.
- Holding **three fingers** for ~0.5 s triggers the `ThreeFingerHold` action.
- Input is written to `Controller.SetVirtualPadState(ushort)`, the same
  active-high bitmask as on Android. Nothing downstream was changed.

Not implemented: physical / MFi gamepad (`GameController.framework`).

---

## Performance (iPhone 8 / A11)

Defaults:

- **Logging is off**: no file writes, no `NSLog`, no string formatting,
  no debug overlay, no stall watchdog. `Console.Out/Error` are redirected to nowhere.
- **1x internal resolution** (the runtime default is 4x, which is the biggest source of lag on A11).
- **Texture filter, dedither, dejitter and widescreen are off.**
  All of them add per-pixel fragment shader work.
- `glGetError` is only called during the first 120 frames
  (on a tile-based GPU it forces a CPU/GPU sync).
- **Interpreter is off** (`UseInterpreter=false`): all code runs as native AOT.

Any of these can be overridden by setting the value explicitly in `settings.json`
(located in `Documents/runtime/`). Settings that are not present there get the defaults above.

### About JIT

A real JIT is not available here: the iOS device .NET runtime does not ship a JIT compiler.
TrollStore grants entitlements, but it does not add a compiler that is missing from the runtime.
AOT without the interpreter is the fastest option available.

### If the app crashes because of AOT

If `checkpoint.log` shows `attempting to JIT compile method ...`,
restore this in `CrashBandicoot.IosHost.csproj`:

```xml
<UseInterpreter>true</UseInterpreter>
```

To keep the hot assemblies native while doing so, also add:

```xml
<MtouchInterpreter>-CrashBandicoot.IosHost,-RecompOne.Runtime</MtouchInterpreter>
```

---

## Logs and debugging

Logging is off by default. To turn it on:

1. Put an **empty file named `enable_log.txt`** into the app's Documents folder (via Files.app).
2. Relaunch the game.

After that:
- a detailed `checkpoint.log` is written (in Documents);
- the debug overlay is visible;
- a `Present: XX.X FPS` line is logged roughly every 2 seconds.

Even with logging off, critical `[FATAL]` and `EXCEPTION` lines still go to `checkpoint.log`,
so crashes are never lost.

Attach `checkpoint.log` when reporting a bug.

---

## Known limitations

- **Audio is disabled**: `IosAudioOutput` is currently a no-op (stubs in the source).
- No launcher or menu: the game starts immediately if a `.cue` is found in Documents.
- No physical gamepad support.
- No dev menu or cheats.
- Mod hot-reload and on-device mod compilation do not work
  (they depend on Roslyn at runtime, which is impossible on iOS).

---

## Architecture (short)

- **Rendering:** native `EAGLContext` / `CAEAGLLayer` (OpenGL ES). Same `GlBackend.cs`
  as on Windows and Android. Not Metal, not ANGLE. GLES functions are resolved via `dlsym`
  from `OpenGLES.framework`. The framebuffer fetch extension (`Ext`) is used.
- **Ahead-of-time recompilation:** iOS forbids runtime code generation and loading assemblies
  on the fly, so ELF → C# is done ahead of time (`tools/CrashBandicoot.PreRecompiler`), and the
  generated `.cs` files live in `CrashBandicoot.IosHost/Recompiled/` and are compiled as ordinary code.
  `RunGame()` calls `Recompiled.Entry.Run(...)` directly.
- **Paths:** `PrepareRuntimePaths()` in `GameViewController.cs` sets `AppPaths.Root`
  (`Documents/runtime`) and loads the config **before** the game thread starts. This matters under AOT:
  the static constructor of `Runtime` (memory cards) reads `AppPaths` on method entry,
  and without this the game crashes with `UnauthorizedAccess` on the read-only `.app` bundle.

---

## Layout of `CrashBandicoot.IosHost/`

| File | Purpose |
|------|---------|
| `AppDelegate.cs` | Entry point, window, lifecycle |
| `GameViewController.cs` | Game startup, path and config preparation, overlays |
| `IosEglContext.cs` | EAGL context, layer, framebuffer |
| `IosPlatformHost.cs` | Present, FPS, glue to the runtime |
| `IosAudioOutput.cs` | Audio (currently no-op) |
| `TouchControllerView.cs` | On-screen gamepad |
| `DiskLog.cs` | Logging (off by default) |
| `Info.plist`, `Entitlements.plist` | App metadata and entitlements |
| `CrashBandicoot.IosHost.csproj` | Build settings (AOT, optimizations) |
