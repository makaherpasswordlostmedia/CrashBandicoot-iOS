using System.Runtime.InteropServices;
using CoreAnimation;
using Foundation;
using ObjCRuntime;
using OpenGLES;
using Silk.NET.Core.Contexts;
using UIKit;

namespace CrashBandicoot.IosHost;

/// <summary>
/// iOS equivalent of AndroidEglContext, using Apple's own EAGLContext /
/// CAEAGLLayer instead of ANGLE.
///
/// Why EAGL and not ANGLE: OpenGLES was deprecated in iOS 12 in favor of
/// Metal, but the EAGL/CAEAGLLayer classes themselves remain present and
/// functional through iOS 14.7 (deprecated APIs on Apple platforms
/// routinely stay working for years - see README for citations). Since
/// this is a TrollStore-sideloaded app, not an App Store submission, the
/// deprecation warning has zero practical consequence: nobody rejects the
/// build for using it. Using EAGL directly means zero third-party
/// dependencies (no ANGLE.xcframework to source, version-pin, and link) -
/// strictly fewer moving parts than the ANGLE-based version this file
/// replaced, which is the single biggest lever for avoiding a repeat of
/// the ~70-iteration debugging slog from the previous iOS port.
///
/// Implements Silk.NET's INativeContext (same contract AndroidEglContext
/// satisfies) so it can be handed straight to
/// Silk.NET.OpenGL.GL.GetApi(this), exactly like MainActivity.cs does on
/// Android - GlBackend.cs itself needs zero changes either way.
/// </summary>
sealed class IosEglContext : INativeContext, IDisposable
{
    EAGLContext? _context;
    CAEAGLLayer? _layer;
    uint _framebuffer;
    uint _colorRenderbuffer;

    public int SurfaceWidth { get; private set; }
    public int SurfaceHeight { get; private set; }

    // --- Raw GLES2 calls needed only for framebuffer/renderbuffer setup;
    // GlBackend.cs itself talks to GL exclusively through Silk.NET's GL
    // object (resolved via GetProcAddress below), never through these. ---
    const string GlesLib = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES";
    [DllImport(GlesLib)] static extern void glGenFramebuffers(int n, out uint framebuffers);
    [DllImport(GlesLib)] static extern void glGenRenderbuffers(int n, out uint renderbuffers);
    [DllImport(GlesLib)] static extern void glBindFramebuffer(uint target, uint framebuffer);
    [DllImport(GlesLib)] static extern void glBindRenderbuffer(uint target, uint renderbuffer);
    [DllImport(GlesLib)] static extern void glFramebufferRenderbuffer(uint target, uint attachment, uint renderbuffertarget, uint renderbuffer);
    [DllImport(GlesLib)] static extern void glGetRenderbufferParameteriv(uint target, uint pname, out int param);
    [DllImport(GlesLib)] static extern uint glCheckFramebufferStatus(uint target);
    [DllImport(GlesLib)] static extern void glViewport(int x, int y, int width, int height);

    const uint GL_FRAMEBUFFER = 0x8D40;
    const uint GL_RENDERBUFFER = 0x8D41;
    const uint GL_COLOR_ATTACHMENT0 = 0x8CE0;
    const uint GL_RENDERBUFFER_WIDTH = 0x8D42;
    const uint GL_RENDERBUFFER_HEIGHT = 0x8D43;
    const uint GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

    /// <summary>
    /// Creates our own CAEAGLLayer and installs it as a sublayer of the
    /// given host view, sized to (width, height) in physical pixels.
    /// GameViewController owns the UIView and just hands it to us.
    /// </summary>
    public void Initialize(UIView hostView, int width, int height)
    {
        _context = new EAGLContext(EAGLRenderingAPI.OpenGLES2)
            ?? throw new InvalidOperationException("EAGLContext creation failed (OpenGLES2 unavailable).");
        if (!EAGLContext.SetCurrentContext(_context))
            throw new InvalidOperationException("EAGLContext.SetCurrentContext failed.");

        _layer = new CAEAGLLayer
        {
            Frame = hostView.Bounds,
            Opaque = true,
            DrawableProperties = NSDictionary.FromObjectsAndKeys(
                new NSObject[] { NSNumber.FromBoolean(false), EAGLColorFormat.RGBA8 },
                new NSObject[] { EAGLDrawableProperty.RetainedBacking, EAGLDrawableProperty.ColorFormat }),
        };
        hostView.Layer.AddSublayer(_layer);

        CreateFramebuffer(width, height);

        // IMPORTANT: EAGLContext is thread-affine (like EGL/GLX/WGL contexts
        // generally). Initialize() itself runs on the main thread (see
        // GameViewController.RunGame's InvokeOnMainThread call), but the
        // actual render loop - IosPlatformHost.Present -> egl.SwapBuffers()
        // and every Silk.NET GL call in between - runs on the background
        // "crash-game-main" thread. A context that is only ever made current
        // on the main thread is NOT current on that render thread, so any
        // GLES entry point called from there hits Apple's internal
        // "no current context" assertion and the process aborts - this is
        // exactly the SIGABRT seen on Thread 0 in the field crash report,
        // not on the render thread, because the *first* unguarded GL call
        // from the render thread trips the assertion before anything
        // renders. Clearing the main thread's current context here and
        // re-asserting it explicitly on the render thread (see
        // MakeCurrentOnCallingThread, called from GameViewController.RunGame
        // right before the render loop starts) fixes this.
        EAGLContext.SetCurrentContext(null);
    }

    void CreateFramebuffer(int width, int height)
    {
        glGenFramebuffers(1, out _framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);

        glGenRenderbuffers(1, out _colorRenderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderbuffer);

        // RenderbufferStorage takes ownership of sizing from the CAEAGLLayer's
        // drawableProperties/bounds - this is the standard EAGL dance
        // (see Apple's now-retired "Configuring OpenGL ES Contexts" guide).
        _context!.RenderBufferStorage((uint)GL_RENDERBUFFER, _layer!);

        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, out var w);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, out var h);
        SurfaceWidth = w > 0 ? w : width;
        SurfaceHeight = h > 0 ? h : height;

        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _colorRenderbuffer);

        var status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            throw new InvalidOperationException($"EAGL framebuffer incomplete: 0x{status:X}");

        glViewport(0, 0, SurfaceWidth, SurfaceHeight);
    }

    /// <summary>
    /// Makes this context current on whatever thread calls this method.
    /// EAGLContext is thread-affine: being current on one thread does not
    /// make it current on another. Must be called once at the start of any
    /// thread that is going to issue GLES calls against this context
    /// (render thread included), and again after any Dispose/re-create.
    /// Throws instead of silently continuing, since a GL call issued with
    /// no current context on this thread is what causes the native abort()
    /// this fixes - better to fail loud and early with a catchable
    /// managed exception than crash the whole process.
    /// </summary>
    public void MakeCurrentOnCallingThread()
    {
        if (_context == null)
            throw new InvalidOperationException("MakeCurrentOnCallingThread called before Initialize().");
        if (!EAGLContext.SetCurrentContext(_context))
            throw new InvalidOperationException("EAGLContext.SetCurrentContext failed on render thread.");
    }

    public void SetExpectedSize(int width, int height)
    {
        if (_layer == null || _context == null) return;
        if (width <= 0 || height <= 0) return;
        if (width == SurfaceWidth && height == SurfaceHeight) return;

        MakeCurrentOnCallingThread();
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        CreateFramebuffer(width, height);
    }

    public void SwapBuffers()
    {
        if (_context == null || _colorRenderbuffer == 0) return;
        // Cheap idempotent re-assert: SetCurrentContext is a no-op fast
        // path when this context is already current on this thread, and
        // guards against any future caller that forgets the explicit
        // MakeCurrentOnCallingThread() at render-thread start (e.g. a
        // resize event handled on yet another thread down the line).
        if (!EAGLContext.SetCurrentContext(_context))
            throw new InvalidOperationException("EAGLContext.SetCurrentContext failed before SwapBuffers.");
        glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderbuffer);
        _context.PresentRenderBuffer((uint)GL_RENDERBUFFER);
    }

    // --- INativeContext (Silk.NET.OpenGL.GL.GetApi(this) needs this) ---
    // OpenGLES.framework's GLES2 entry points are resolved by ordinary
    // dynamic symbol lookup against the already-loaded framework image -
    // no eglGetProcAddress equivalent needed for the built-in Apple GLES
    // implementation (unlike ANGLE, which requires it).
    public nint GetProcAddress(string proc, int? slot = null) =>
        Dlfcn.dlsym(Libraries.OpenGLESHandle, proc);

    public bool TryGetProcAddress(string proc, out nint addr, int? slot = null)
    {
        addr = GetProcAddress(proc, slot);
        return addr != 0;
    }

    public void Dispose()
    {
        _layer?.RemoveFromSuperLayer();
        _layer = null;
        if (_context != null)
        {
            EAGLContext.SetCurrentContext(null);
            _context.Dispose();
            _context = null;
        }
    }

    static class Libraries
    {
        public static readonly IntPtr OpenGLESHandle =
            Dlfcn.dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", 0);
    }
}
