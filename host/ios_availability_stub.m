/*
 * ___isPlatformVersionAtLeast is a compiler-rt builtin clang emits for
 * @available(...)/API_AVAILABLE checks. This toolchain's prebuilt
 * libSDL2.a was compiled against a clang that assumes it's always
 * present at link time (normally supplied by Apple's compiler-rt on a
 * real Xcode toolchain), but this bare Linux->iOS cross-toolchain
 * doesn't ship a compiler-rt archive at all - so the symbol is simply
 * never linked in.
 *
 * Both call sites that need it (SDL_HideHomeIndicatorHintChanged and
 * UIKit_ShowMessageBoxAlertController, both in SDL2's UIKit backend)
 * only use it to guard iOS 11+/13+ API calls. Since this whole build
 * targets iPhoneOS 9.3, any real OS this binary ever runs on that is
 * newer than 9.3 should still report "yes, at least" for those
 * checks correctly - so a full platform/version parser isn't needed,
 * just something that answers consistently with the real runtime
 * version already available via -[UIDevice systemVersion].
 *
 * Clang's ABI for this builtin (see clang/lib/CodeGen/CGObjC.cpp,
 * EmitAvailabilityCheck): int32_t platform, then three components
 * (major, minor, subminor) of the version being checked against.
 * Returning 1 when the running OS's version >= the requested version
 * matches real semantics; the SDL call sites already gate their own
 * logic on this, so this stub just needs to answer correctly rather
 * than unconditionally return true.
 */
#import <UIKit/UIKit.h>

int32_t __isPlatformVersionAtLeast(int32_t platform, int32_t major, int32_t minor, int32_t subminor) {
    (void)platform; /* single-platform (iOS) build; ignore platform id */

    NSString *sysVersion = [[UIDevice currentDevice] systemVersion];
    NSArray<NSString *> *parts = [sysVersion componentsSeparatedByString:@"."];

    int32_t runningMajor = parts.count > 0 ? [parts[0] intValue] : 0;
    int32_t runningMinor = parts.count > 1 ? [parts[1] intValue] : 0;
    int32_t runningSubminor = parts.count > 2 ? [parts[2] intValue] : 0;

    if (runningMajor != major) return runningMajor > major;
    if (runningMinor != minor) return runningMinor > minor;
    return runningSubminor >= subminor;
}
