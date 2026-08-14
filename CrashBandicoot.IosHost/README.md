# CrashBandicoot.IosHost — iPhone 8 / iOS 14.7 / TrollStore port

## Status: skeleton, not yet run on real hardware

This is a first working structure, not a finished port. It compiles-worthy
in shape but has **not been built or tested on an actual device or Mac
yet** (no macOS/Xcode available in the environment that wrote this). Treat
everything below as "should work per the docs / per how Android does it",
to be validated by you on real hardware.

## Why this exists / what makes iOS different from the Android host

The Android host (`AndroidRuntimeHost/`) does two things at first launch
that iOS flatly cannot do, TrollStore or not:

1. **Generates C# from the PS1 disc via Roslyn, on-device**
   (`RecompRunner` → `.cs` files → `GameCompiler.CompileToDll`), then
2. **Loads that freshly-built .dll via `AssemblyLoadContext`**
   (`GameLoader.Run`).

iOS forbids both dynamic code generation (no JIT) and loading assemblies
that weren't known at app-build time — this is enforced by the OS/AMFI, not
just an App Store policy, and TrollStore's CoreTrust bypass grants
entitlements, it does not add a missing runtime capability.

**Fix**: split the pipeline in two.
- `tools/CrashBandicoot.PreRecompiler` + `scripts/prerecompile.sh` run the
  *first* step (ELF → C# source) once, ahead of time, on any machine with
  your disc image (CI or local).
- The generated `.cs` files land in `CrashBandicoot.IosHost/Recompiled/`
  and get compiled as ordinary source, AOT, together with the rest of the
  app — like any other file in the project. No Roslyn, no
  `AssemblyLoadContext`, anywhere on the device.
- `GameViewController.RunGame()` calls `Recompiled.Entry.Run(...)` as a
  plain static method call instead of via reflection.

Consequence: **mod hot-reloading / on-device mod compilation (`ModCompiler.cs`,
`AssetHotReload.cs`) will not work the same way it does on Android** — those
also rely on Roslyn-at-runtime. Out of scope for this first pass; flag if
you want that ported too (it would need the same "compile ahead of time"
treatment, per mod, or an interpreter-based mod format instead of C#).

## Rendering: native EAGLContext/CAEAGLLayer, not ANGLE, not a rewrite

`RecompOne.Runtime/Gpu/Hle/Gl/GlBackend.cs` is 947 lines of PS1 VRAM/
blending/texture-window emulation shared with Windows and Android. Rather
than hand-porting that to native Metal (high risk of subtle rendering bugs
vs. the reference implementation) or pulling in a third-party translation
layer like ANGLE (extra binary to source/pin/link correctly - its own
source of build failures), `IosEglContext.cs` uses Apple's own
`EAGLContext`/`CAEAGLLayer` GLES2 implementation directly. It's been
deprecated since iOS 12 in favor of Metal, but remains present and
functional through iOS 14.7 - and since this ships via TrollStore, not
the App Store, the deprecation carries no practical consequence.
`GlBackend.cs` itself is untouched - same file, same GL calls, as on
Android - `IosEglContext` just resolves GLES2 symbols via `dlsym` against
the already-loaded `OpenGLES.framework` instead of via `eglGetProcAddress`.

## Audio: AVAudioEngine, same push-mixer thread model as Android

`IosAudioOutput.cs` keeps the identical background-thread `spu.Mix(...)`
call pattern as `AndroidAudioOutput.cs`, bridged into CoreAudio's pull-based
`AVAudioSourceNode` render callback via a small ring buffer. See the doc
comment at the top of that file for the reasoning.

## Input: UIKit touches → the same `Controller.SetVirtualPadState` bitmask

`TouchControllerView.cs` is a first-pass on-screen d-pad + face buttons +
shoulder buttons layout (not final art/positioning — adjust freely) that
writes into `RecompOne.Runtime.Hardware.Controller` exactly like
`AndroidGamepad.cs`/`TouchControllerView.cs` do on Android. No changes
needed anywhere downstream (`InputManager.cs`, `sdk/LibPad.cs`).

## What's NOT ported yet

- The launcher/menu UI (`CrashBandicoot.Launcher/Ui/`) — this skeleton
  boots straight into the game if it finds a `.cue` in the app's
  Documents folder (visible over Files.app / iTunes file sharing), full
  stop. No disc picker, no mod manager, no settings screen.
- Physical/MFi gamepad support (`AndroidGamepad.cs` equivalent via
  `GameController.framework`).
- Save states / memory card UI.
- Dev menu / cheats dialog (`DevMenuOverlay`, `CheatDialog` on Android).
- On-device mod compilation (see above).

## Build

1. Get your own legally-dumped `.cue`/`.bin` disc image (not provided or
   fetched by this repo — copyrighted game data).
2. `./scripts/prerecompile.sh /path/to/game.cue`
3. `dotnet build CrashBandicoot.IosHost -c Release -f net8.0-ios -r ios-arm64 -p:BuildIpa=true`
   on a Mac with Xcode + the `dotnet workload install ios` workload, OR
   trigger `.github/workflows/ios-build.yml` (macOS GitHub Actions runner)
   and download the resulting `.ipa` artifact.
4. AirDrop/transfer the `.ipa` to the iPhone 8 and install via TrollStore.
5. Use Files.app to copy your `.cue`/`.bin` into the app's Documents
   folder before first launch.

## Known open risks (please report back what you hit)

- Whether `net8.0-ios`'s Mono interpreter actually handles everything
  `RecompOne.Runtime` throws at it (generic virtual dispatch through
  `IGpuBackend`, `IRuntimePlatformHost`, etc.) — Full AOT alone is known
  to reject some of these patterns; `UseInterpreter=true` is the mitigation
  but hasn't been validated against this specific codebase.
- Whether Silk.NET's `GL.GetApi(INativeContext)` resolves every GLES2
  symbol `GlBackend.cs` needs purely via `dlsym` against
  `OpenGLES.framework` (no `eglGetProcAddress`-style indirection needed
  for Apple's own implementation, unlike ANGLE) — should work per how
  `dlopen`/`dlsym` resolve exported C symbols generally, unverified here.
- A11/iOS 14.7 CPU headroom for the Mono interpreter path specifically
  (interpreted code is meaningfully slower than JIT/AOT) — the recompiled
  game code itself is AOT'd (point of this whole design), so only
  `RecompOne.Runtime`'s own hot paths that fall through to the interpreter
  matter here; likely fine but worth profiling early.
