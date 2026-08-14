using CrashBandicoot.Launcher.Recomp;

// Usage:
//   dotnet run --project tools/CrashBandicoot.PreRecompiler -- \
//       <path-to-CrashBandicoot.json> <path-to-game.cue> <output-dir-for-cs>
//
// This is the ONE thing that has to happen outside of Xcode/on a machine
// that owns a legally-dumped copy of the disc: turn the PS1 executable into
// C# source files. Run it once (locally, or as a GitHub Actions step) and
// commit/upload the resulting .cs files - they are just source code, no
// different in kind from any other file in the repo, and from this point on
// the iOS build is a completely ordinary AOT `dotnet build`.
//
// The .cue/.bin themselves are NOT committed to the repo (copyright) - this
// tool is meant to be pointed at a disc image you provide out-of-band, e.g.
// via a GitHub Actions "workflow_dispatch" secret path or a self-hosted
// runner that already has it on disk.

if (args.Length < 3)
{
    Console.Error.WriteLine("Usage: PreRecompiler <config.json> <game.cue> <outDir>");
    return 1;
}

var configPath = args[0];
var cuePath = args[1];
var outDir = args[2];

if (!File.Exists(configPath))
{
    Console.Error.WriteLine($"Config not found: {configPath}");
    return 1;
}
if (!File.Exists(cuePath))
{
    Console.Error.WriteLine($"Disc .cue not found: {cuePath}");
    return 1;
}

Console.WriteLine($"[PreRecompiler] config = {configPath}");
Console.WriteLine($"[PreRecompiler] cue    = {cuePath}");
Console.WriteLine($"[PreRecompiler] outDir = {outDir}");

try
{
    RecompRunner.Run(
        configTemplatePath: configPath,
        cuePath: cuePath,
        outDir: outDir,
        progress: new Progress<string>(msg => Console.WriteLine($"[PreRecompiler] {msg}")));
}
catch (Exception ex)
{
    Console.Error.WriteLine($"[PreRecompiler] FAILED: {ex}");
    return 1;
}

var csFiles = Directory.GetFiles(outDir, "*.cs");
Console.WriteLine($"[PreRecompiler] Done. {csFiles.Length} .cs files written to {outDir}");
return 0;
