/* ios_host.m - UIKit / EAGL / AudioQueue host for the native RecompOne runtime.
 * Targets iOS 7-9.3 on armv7 (iPad mini 1). ARC is enabled for this file only. */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <GameController/GameController.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <mach/mach_time.h>
#include "psx.h"

/* ------------------------------------------------------------------ */
/* Paths / logging                                                      */
/* ------------------------------------------------------------------ */
static void *wd_thread(void *arg);
static char g_docs[512], g_cue[600], g_saves[600], g_logpath[600];
static FILE *g_logf;
static pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;

void plat_log(const char *fmt, ...)
{
    pthread_mutex_lock(&g_log_mx);
    if (!g_logf && g_logpath[0]) g_logf = fopen(g_logpath, "a");
    va_list a; va_start(a, fmt);
    if (g_logf) { vfprintf(g_logf, fmt, a); fputc('\n', g_logf); fflush(g_logf); }
    va_end(a);
    pthread_mutex_unlock(&g_log_mx);
}

const char *plat_disc_path(void) { return g_cue; }
const char *plat_save_dir(void) { return g_saves; }
void plat_rumble(uint8_t large, uint8_t small_) { (void)large; (void)small_; }

uint64_t plat_time_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    if (tb.numer == tb.denom) return t;   /* skip the 64-bit divide (slow on armv7) */
    return t * tb.numer / tb.denom;
}
void plat_sleep_us(unsigned us) { usleep(us); }

static BOOL find_cue(void)
{
    DIR *d = opendir(g_docs);
    if (!d) return NO;
    struct dirent *e;
    BOOL ok = NO;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".cue") == 0) {
            snprintf(g_cue, sizeof g_cue, "%s/%s", g_docs, e->d_name);
            ok = YES; break;
        }
    }
    closedir(d);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Pad state                                                            */
/* ------------------------------------------------------------------ */
static volatile uint32_t g_touch_mask, g_gc_mask;
static void pad_recompute(void) { g_pad_pressed = g_touch_mask | g_gc_mask; }
void plat_input_poll(void) {}

/* ------------------------------------------------------------------ */
/* GL presenter (runs entirely on the emulation thread)                 */
/* ------------------------------------------------------------------ */
@interface GLView : UIView
@end
@implementation GLView
+ (Class)layerClass { return [CAEAGLLayer class]; }
@end

static GLView *g_glview;
static EAGLContext *g_ctx;
static GLuint g_fbo, g_rbo;
static GLint g_view_w, g_view_h;

void gpu_present(int dx, int dy, int dw, int dh, bool rgb24, bool enabled);   /* gpu.c */

static void gl_setup(void)
{
    __block CAEAGLLayer *layer = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        layer = (CAEAGLLayer *)g_glview.layer;
        layer.opaque = YES;
        layer.drawableProperties = @{ kEAGLDrawablePropertyRetainedBacking: @NO,
                                      kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGB565 };
    });
    g_ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    [EAGLContext setCurrentContext:g_ctx];
    glGenFramebuffers(1, &g_fbo); glGenRenderbuffers(1, &g_rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g_rbo);
    [g_ctx renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_rbo);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &g_view_w);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &g_view_h);
    plat_log("[GL] screen %dx%d", g_view_w, g_view_h);
}

/* Called by gpu.c (emulation thread) before it touches GL. */
void plat_gl_make_current(void)
{
    if (!g_ctx) gl_setup();
    [EAGLContext setCurrentContext:g_ctx];
}

void plat_gl_screen(int *fbo, int *w, int *h)
{
    *fbo = (int)g_fbo; *w = (int)g_view_w; *h = (int)g_view_h;
}

/* vram pointer is unused: the renderer keeps VRAM on the GPU. */
void plat_present(const uint16_t *vram, int dx, int dy, int dw, int dh, bool rgb24, bool enabled)
{
    (void)vram;
    while (g_paused) usleep(10000);
    plat_gl_make_current();
    gpu_present(dx, dy, dw, dh, rgb24, enabled);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g_rbo);
    [g_ctx presentRenderbuffer:GL_RENDERBUFFER];
}

/* ------------------------------------------------------------------ */
/* Audio                                                                */
/* ------------------------------------------------------------------ */
#define AUDIO_FRAMES 1024
static AudioQueueRef g_aq;

static void aq_callback(void *ud, AudioQueueRef q, AudioQueueBufferRef buf)
{
    (void)ud;
    spu_mix((int16_t *)buf->mAudioData, AUDIO_FRAMES);
    buf->mAudioDataByteSize = AUDIO_FRAMES * 4;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

static void audio_start(void)
{
    [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:nil];
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = 44100; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 16; f.mChannelsPerFrame = 2; f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mBytesPerPacket = 4;
    if (AudioQueueNewOutput(&f, aq_callback, NULL, NULL, NULL, 0, &g_aq) != noErr) { plat_log("[AUDIO] AudioQueueNewOutput failed"); return; }
    for (int i = 0; i < 4; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(g_aq, AUDIO_FRAMES * 4, &b);
        memset(b->mAudioData, 0, AUDIO_FRAMES * 4);
        b->mAudioDataByteSize = AUDIO_FRAMES * 4;
        AudioQueueEnqueueBuffer(g_aq, b, 0, NULL);
    }
    AudioQueueStart(g_aq, NULL);
}

/* ------------------------------------------------------------------ */
/* Touch pad overlay                                                    */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t mask; CGPoint c; CGFloat r; const char *label; } PadBtn;

@interface PadView : UIView
@end
@implementation PadView {
    PadBtn _btn[10]; int _n;
    CGPoint _dpad; CGFloat _dpadR;
}
- (instancetype)initWithFrame:(CGRect)f
{
    if ((self = [super initWithFrame:f])) {
        self.multipleTouchEnabled = YES; self.backgroundColor = [UIColor clearColor];
        self.opaque = NO; self.userInteractionEnabled = YES;
    }
    return self;
}
- (void)layoutSubviews
{
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    CGFloat u = MIN(W, H) / 768.0;
    _dpad = CGPointMake(150 * u * 1.2, H - 170 * u * 1.2); _dpadR = 120 * u;
    CGFloat fx = W - 170 * u * 1.2, fy = H - 170 * u * 1.2, d = 80 * u;
    _n = 0;
    _btn[_n++] = (PadBtn){ PAD_TRIANGLE, CGPointMake(fx, fy - d), 42 * u, "T" };
    _btn[_n++] = (PadBtn){ PAD_CROSS, CGPointMake(fx, fy + d), 42 * u, "X" };
    _btn[_n++] = (PadBtn){ PAD_SQUARE, CGPointMake(fx - d, fy), 42 * u, "S" };
    _btn[_n++] = (PadBtn){ PAD_CIRCLE, CGPointMake(fx + d, fy), 42 * u, "O" };
    _btn[_n++] = (PadBtn){ PAD_L1, CGPointMake(90 * u, 70 * u), 50 * u, "L1" };
    _btn[_n++] = (PadBtn){ PAD_L2, CGPointMake(90 * u, 170 * u), 50 * u, "L2" };
    _btn[_n++] = (PadBtn){ PAD_R1, CGPointMake(W - 90 * u, 70 * u), 50 * u, "R1" };
    _btn[_n++] = (PadBtn){ PAD_R2, CGPointMake(W - 90 * u, 170 * u), 50 * u, "R2" };
    _btn[_n++] = (PadBtn){ PAD_SELECT, CGPointMake(W / 2 - 70 * u, H - 40 * u), 34 * u, "SEL" };
    _btn[_n++] = (PadBtn){ PAD_START, CGPointMake(W / 2 + 70 * u, H - 40 * u), 34 * u, "STA" };
    [self setNeedsDisplay];
}
- (void)drawRect:(CGRect)rect
{
    CGContextRef g = UIGraphicsGetCurrentContext();
    CGContextSetRGBStrokeColor(g, 1, 1, 1, 0.45);
    CGContextSetRGBFillColor(g, 1, 1, 1, 0.10);
    CGContextSetLineWidth(g, 2);
    CGContextAddEllipseInRect(g, CGRectMake(_dpad.x - _dpadR, _dpad.y - _dpadR, _dpadR * 2, _dpadR * 2));
    CGContextDrawPath(g, kCGPathFillStroke);
    for (int i = 0; i < 4; i++) {
        CGFloat a = i * M_PI_2, cx = _dpad.x + cos(a) * _dpadR * 0.62, cy = _dpad.y + sin(a) * _dpadR * 0.62;
        CGContextAddEllipseInRect(g, CGRectMake(cx - 6, cy - 6, 12, 12));
        CGContextStrokePath(g);
    }
    for (int i = 0; i < _n; i++) {
        PadBtn *b = &_btn[i];
        CGContextAddEllipseInRect(g, CGRectMake(b->c.x - b->r, b->c.y - b->r, b->r * 2, b->r * 2));
        CGContextDrawPath(g, kCGPathFillStroke);
        NSString *s = [NSString stringWithUTF8String:b->label];
        NSDictionary *at = @{ NSFontAttributeName: [UIFont boldSystemFontOfSize:MAX(12, b->r * 0.55)],
                              NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:0.6] };
        CGSize sz = [s sizeWithAttributes:at];
        [s drawAtPoint:CGPointMake(b->c.x - sz.width / 2, b->c.y - sz.height / 2) withAttributes:at];
    }
}
- (void)recompute:(NSSet *)all
{
    uint32_t m = 0;
    for (UITouch *t in all) {
        if (t.phase == UITouchPhaseEnded || t.phase == UITouchPhaseCancelled) continue;
        CGPoint p = [t locationInView:self];
        CGFloat dx = p.x - _dpad.x, dy = p.y - _dpad.y, dist = hypot(dx, dy);
        if (dist < _dpadR * 1.25 && dist > _dpadR * 0.18) {
            CGFloat ang = atan2(dy, dx);           /* 0 = right, +pi/2 = down */
            if (cos(ang) > 0.38) m |= PAD_RIGHT;
            if (cos(ang) < -0.38) m |= PAD_LEFT;
            if (sin(ang) > 0.38) m |= PAD_DOWN;
            if (sin(ang) < -0.38) m |= PAD_UP;
        }
        for (int i = 0; i < _n; i++)
            if (hypot(p.x - _btn[i].c.x, p.y - _btn[i].c.y) <= _btn[i].r * 1.25) m |= _btn[i].mask;
    }
    g_touch_mask = m; pad_recompute();
}
- (void)touchesBegan:(NSSet *)t withEvent:(UIEvent *)e { [self recompute:[e allTouches]]; }
- (void)touchesMoved:(NSSet *)t withEvent:(UIEvent *)e { [self recompute:[e allTouches]]; }
- (void)touchesEnded:(NSSet *)t withEvent:(UIEvent *)e { [self recompute:[e allTouches]]; }
- (void)touchesCancelled:(NSSet *)t withEvent:(UIEvent *)e { [self recompute:[e allTouches]]; }
@end

/* ------------------------------------------------------------------ */
/* MFi controller                                                       */
/* ------------------------------------------------------------------ */
static void gc_attach(GCController *c)
{
    GCExtendedGamepad *x = c.extendedGamepad;
    if (x) {
        x.valueChangedHandler = ^(GCExtendedGamepad *p, GCControllerElement *el) {
            uint32_t m = 0;
            if (p.dpad.up.pressed) m |= PAD_UP; if (p.dpad.down.pressed) m |= PAD_DOWN;
            if (p.dpad.left.pressed) m |= PAD_LEFT; if (p.dpad.right.pressed) m |= PAD_RIGHT;
            if (p.buttonA.pressed) m |= PAD_CROSS; if (p.buttonB.pressed) m |= PAD_CIRCLE;
            if (p.buttonX.pressed) m |= PAD_SQUARE; if (p.buttonY.pressed) m |= PAD_TRIANGLE;
            if (p.leftShoulder.pressed) m |= PAD_L1; if (p.rightShoulder.pressed) m |= PAD_R1;
            if (p.leftTrigger.pressed) m |= PAD_L2; if (p.rightTrigger.pressed) m |= PAD_R2;
            if (p.leftThumbstick.up.value > 0.5) m |= PAD_UP; if (p.leftThumbstick.down.value > 0.5) m |= PAD_DOWN;
            if (p.leftThumbstick.left.value > 0.5) m |= PAD_LEFT; if (p.leftThumbstick.right.value > 0.5) m |= PAD_RIGHT;
            g_gc_mask = m; pad_recompute();
        };
        return;
    }
    GCGamepad *g = c.gamepad;
    if (g) {
        g.valueChangedHandler = ^(GCGamepad *p, GCControllerElement *el) {
            uint32_t m = 0;
            if (p.dpad.up.pressed) m |= PAD_UP; if (p.dpad.down.pressed) m |= PAD_DOWN;
            if (p.dpad.left.pressed) m |= PAD_LEFT; if (p.dpad.right.pressed) m |= PAD_RIGHT;
            if (p.buttonA.pressed) m |= PAD_CROSS; if (p.buttonB.pressed) m |= PAD_CIRCLE;
            if (p.buttonX.pressed) m |= PAD_SQUARE; if (p.buttonY.pressed) m |= PAD_TRIANGLE;
            if (p.leftShoulder.pressed) m |= PAD_L1; if (p.rightShoulder.pressed) m |= PAD_R1;
            g_gc_mask = m; pad_recompute();
        };
    }
}

/* ------------------------------------------------------------------ */
/* Fatal errors                                                         */
/* ------------------------------------------------------------------ */
void plat_fatal(const char *msg)
{
    plat_log("[FATAL] %s", msg);
    NSString *s = [NSString stringWithUTF8String:msg];
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertView *a = [[UIAlertView alloc] initWithTitle:@"Crash Bandicoot" message:s delegate:nil cancelButtonTitle:@"OK" otherButtonTitles:nil];
        [a show];
    });
    g_paused = 1;
    if (g_aq) AudioQueuePause(g_aq);
    for (;;) sleep(3600);
}

/* ------------------------------------------------------------------ */
/* App                                                                  */
/* ------------------------------------------------------------------ */
@interface RootVC : UIViewController
@end
@implementation RootVC
- (BOOL)prefersStatusBarHidden { return YES; }
- (NSUInteger)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)shouldAutorotate { return YES; }
- (void)loadView
{
    CGRect b = [UIScreen mainScreen].bounds;
    if (b.size.width < b.size.height) b = CGRectMake(0, 0, b.size.height, b.size.width);
    UIView *root = [[UIView alloc] initWithFrame:b];
    root.backgroundColor = [UIColor blackColor];
    g_glview = [[GLView alloc] initWithFrame:b];
    g_glview.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [root addSubview:g_glview];
    PadView *pad = [[PadView alloc] initWithFrame:b];
    pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [root addSubview:pad];
    self.view = root;
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate
- (void)emuMain:(id)arg
{
    @autoreleasepool { audio_start(); }
    runtime_run_game();
    plat_fatal("game returned");
}
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    snprintf(g_docs, sizeof g_docs, "%s", docs.UTF8String);
    snprintf(g_saves, sizeof g_saves, "%s/Saves", g_docs);
    snprintf(g_logpath, sizeof g_logpath, "%s/crash.log", g_docs);
    mkdir(g_saves, 0755);

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [RootVC new];
    [self.window makeKeyAndVisible];
    app.idleTimerDisabled = YES;

    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
        usingBlock:^(NSNotification *n) { gc_attach(n.object); }];
    for (GCController *c in [GCController controllers]) gc_attach(c);

    if (!find_cue()) {
        UILabel *l = [[UILabel alloc] initWithFrame:self.window.bounds];
        l.numberOfLines = 0; l.textAlignment = NSTextAlignmentCenter; l.textColor = [UIColor whiteColor];
        l.text = @"Put your Crash Bandicoot disc image (.cue + .bin)\ninto this app's Documents folder\n(iTunes File Sharing / Filza) and relaunch.";
        [self.window.rootViewController.view addSubview:l];
        return YES;
    }
    plat_log("---- launch, cue=%s", g_cue);
    NSThread *t = [[NSThread alloc] initWithTarget:self selector:@selector(emuMain:) object:nil];
    t.stackSize = 16 * 1024 * 1024;
    t.name = @"emu";
    [t start];
    { pthread_t wt; pthread_create(&wt, NULL, wd_thread, NULL); pthread_detach(wt); }
    return YES;
}
- (void)applicationWillResignActive:(UIApplication *)a { g_paused = 1; if (g_aq) AudioQueuePause(g_aq); if (g_card_a) card_flush(g_card_a); if (g_card_b) card_flush(g_card_b); }
- (void)applicationDidBecomeActive:(UIApplication *)a { g_paused = 0; if (g_aq) AudioQueueStart(g_aq, NULL); }
@end


/* Freeze watchdog: if the emulation thread stops presenting frames for >3 s
 * (and the app is not paused) write where the guest CPU is stuck to crash.log. */
static void *wd_thread(void *arg)
{
    (void)arg;
    int64_t last = -1; int stalled = 0;
    for (;;) {
        sleep(1);
        if (g_paused) { stalled = 0; continue; }
        int64_t now = g_present_calls;
        if (now == last) {
            if (++stalled == 3 || (stalled > 3 && (stalled % 10) == 0)) {
                CpuContext *c = &g_cpu;
                plat_log("[WATCHDOG] no frame for %ds | present=%lld RA=%08X SP=%08X GP=%08X A0=%08X A1=%08X V0=%08X S0=%08X",
                         stalled, (long long)now, c->RA, c->SP, c->GP, c->A0, c->A1, c->V0, c->S0);
            }
        } else { stalled = 0; last = now; }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class])); }
}
