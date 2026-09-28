using CoreGraphics;
using Foundation;
using RecompOne.Runtime.Hardware;
using UIKit;

namespace CrashBandicoot.IosHost;

/// <summary>
/// On-screen PS1 pad for iOS. Draws its own buttons (translucent, with
/// pressed-state highlight) and feeds the same
/// Controller.SetVirtualPadState(ushort) active-high bitmask every other host
/// uses, so nothing downstream (InputManager, sdk/LibPad.cs) changes.
///
/// Layout (landscape):
///   top-left    : L2 / L1          top-right : R2 / R1
///   bottom-left : D-pad (8-way, slide your thumb across it)
///   bottom-right: △ ○ ✕ □ (touch between two buttons = both pressed,
///                          e.g. jump + spin)
///   bottom-mid  : SELECT / START
///
/// Only redraws when the pressed-button set changes, so it costs nothing
/// while the game is running.
/// </summary>
public sealed class TouchControllerView : UIView
{
    const int HoldMilliseconds = 500;

    // Idle / pressed look. Kept faint so the game stays visible.
    static readonly UIColor IdleFill = UIColor.FromWhiteAlpha(1f, 0.14f);
    static readonly UIColor PressedFill = UIColor.FromWhiteAlpha(1f, 0.42f);
    static readonly UIColor IdleStroke = UIColor.FromWhiteAlpha(1f, 0.40f);
    static readonly UIColor ArrowIdle = UIColor.FromWhiteAlpha(1f, 0.45f);
    static readonly UIColor ArrowPressed = UIColor.FromWhiteAlpha(1f, 0.95f);

    static readonly UIColor TriangleColor = UIColor.FromRGBA(0.35f, 0.90f, 0.72f, 1f);
    static readonly UIColor CircleColor = UIColor.FromRGBA(1.00f, 0.38f, 0.38f, 1f);
    static readonly UIColor CrossColor = UIColor.FromRGBA(0.42f, 0.62f, 1.00f, 1f);
    static readonly UIColor SquareColor = UIColor.FromRGBA(1.00f, 0.62f, 0.86f, 1f);

    // ---- geometry (points), rebuilt in LayoutZones ----
    CGPoint _dpadC;
    double _dpadR;
    double _armW;

    CGPoint _faceC;
    double _faceBtnR;
    double _faceOffset;

    (CGRect rect, ushort bit)[] _pills = Array.Empty<(CGRect, ushort)>();

    readonly UILabel _lblL1 = MakeLabel("L1", 14);
    readonly UILabel _lblL2 = MakeLabel("L2", 14);
    readonly UILabel _lblR1 = MakeLabel("R1", 14);
    readonly UILabel _lblR2 = MakeLabel("R2", 14);
    readonly UILabel _lblSelect = MakeLabel("SELECT", 10);
    readonly UILabel _lblStart = MakeLabel("START", 10);

    readonly Dictionary<nint, ushort> _activeTouchBits = new();
    ushort _currentState;

    readonly UIImpactFeedbackGenerator? _haptic;

    NSTimer? _holdTimer;
    public Action? ThreeFingerHold { get; set; }

    public TouchControllerView(CGRect frame) : base(frame)
    {
        BackgroundColor = UIColor.Clear;
        Opaque = false;
        MultipleTouchEnabled = true;
        UserInteractionEnabled = true;
        ContentMode = UIViewContentMode.Redraw;

        foreach (var l in new[] { _lblL1, _lblL2, _lblR1, _lblR2, _lblSelect, _lblStart })
            AddSubview(l);

        try
        {
            _haptic = new UIImpactFeedbackGenerator(UIImpactFeedbackStyle.Light);
            _haptic.Prepare();
        }
        catch { _haptic = null; }

        LayoutZones();
    }

    static UILabel MakeLabel(string text, float size) => new()
    {
        Text = text,
        TextColor = UIColor.FromWhiteAlpha(1f, 0.85f),
        TextAlignment = UITextAlignment.Center,
        Font = UIFont.BoldSystemFontOfSize(size),
        UserInteractionEnabled = false,
        BackgroundColor = UIColor.Clear,
    };

    public override void LayoutSubviews()
    {
        base.LayoutSubviews();
        LayoutZones();
        SetNeedsDisplay();
    }

    void LayoutZones()
    {
        var b = Bounds;
        double w = (double)b.Width;
        double h = (double)b.Height;
        var ins = SafeAreaInsets;

        double marginX = 22 + Math.Max((double)ins.Left, (double)ins.Right);
        double marginBottom = 18 + (double)ins.Bottom;
        double marginTop = 10 + (double)ins.Top;

        // Cluster size scales with the short side (375pt on iPhone 8 landscape -> ~157pt).
        double cluster = Math.Clamp(Math.Min(w, h) * 0.42, 120, 190);

        // ---- D-pad ----
        _dpadR = cluster / 2;
        _armW = _dpadR * 0.66;
        _dpadC = new CGPoint(marginX + _dpadR, h - marginBottom - _dpadR);

        // ---- Face buttons ----
        _faceBtnR = cluster * 0.19;
        _faceOffset = cluster * 0.32;
        _faceC = new CGPoint(w - marginX - cluster / 2, h - marginBottom - cluster / 2);

        // ---- Shoulders ----
        double sw = Math.Clamp(cluster * 0.50, 62, 90);
        double sh = Math.Clamp(cluster * 0.22, 30, 40);
        double gap = 6;
        double topL = marginX;
        double topR = w - marginX - sw;

        var l2 = new CGRect(topL, marginTop, sw, sh);
        var l1 = new CGRect(topL, marginTop + sh + gap, sw, sh);
        var r2 = new CGRect(topR, marginTop, sw, sh);
        var r1 = new CGRect(topR, marginTop + sh + gap, sw, sh);

        // ---- Select / Start (bottom centre, out of the thumbs' way) ----
        double pw = 64, ph = 26, pg = 16;
        double py = h - marginBottom - ph;
        var select = new CGRect(w / 2 - pw - pg / 2, py, pw, ph);
        var start = new CGRect(w / 2 + pg / 2, py, pw, ph);

        _pills = new (CGRect, ushort)[]
        {
            (l1, Controller.L1), (l2, Controller.L2),
            (r1, Controller.R1), (r2, Controller.R2),
            (select, Controller.Select), (start, Controller.Start),
        };

        _lblL1.Frame = l1; _lblL2.Frame = l2;
        _lblR1.Frame = r1; _lblR2.Frame = r2;
        _lblSelect.Frame = select; _lblStart.Frame = start;
    }

    // ------------------------------------------------------------------
    // Hit testing
    // ------------------------------------------------------------------

    ushort DpadBits(CGPoint p)
    {
        double dx = (double)(p.X - _dpadC.X);
        double dy = (double)(p.Y - _dpadC.Y);
        double dist = Math.Sqrt(dx * dx + dy * dy);
        // Small dead zone in the middle, generous outer edge (thumbs drift).
        if (dist < _dpadR * 0.16 || dist > _dpadR * 1.45) return 0;

        double nx = dx / dist;
        double ny = dy / dist;   // UIKit: +y is DOWN
        const double t = 0.38;   // sin(22.5deg) -> 8 equal 45deg sectors

        ushort bits = 0;
        if (nx > t) bits |= Controller.Right; else if (nx < -t) bits |= Controller.Left;
        if (ny > t) bits |= Controller.Down; else if (ny < -t) bits |= Controller.Up;
        return bits;
    }

    ushort FaceBits(CGPoint p)
    {
        double hit = _faceBtnR * 1.40;   // overlapping zones -> two buttons at once
        ushort bits = 0;
        if (Near(p, FaceCenter(Controller.Triangle), hit)) bits |= Controller.Triangle;
        if (Near(p, FaceCenter(Controller.Cross), hit)) bits |= Controller.Cross;
        if (Near(p, FaceCenter(Controller.Square), hit)) bits |= Controller.Square;
        if (Near(p, FaceCenter(Controller.Circle), hit)) bits |= Controller.Circle;
        return bits;
    }

    CGPoint FaceCenter(ushort bit)
    {
        double cx = (double)_faceC.X, cy = (double)_faceC.Y;
        return bit switch
        {
            Controller.Triangle => new CGPoint(cx, cy - _faceOffset),
            Controller.Cross => new CGPoint(cx, cy + _faceOffset),
            Controller.Square => new CGPoint(cx - _faceOffset, cy),
            _ => new CGPoint(cx + _faceOffset, cy), // Circle
        };
    }

    static bool Near(CGPoint a, CGPoint b, double r)
    {
        double dx = (double)(a.X - b.X), dy = (double)(a.Y - b.Y);
        return dx * dx + dy * dy <= r * r;
    }

    ushort HitTest(CGPoint p)
    {
        ushort bits = (ushort)(DpadBits(p) | FaceBits(p));
        foreach (var (rect, bit) in _pills)
            if (new CGRect(rect.X - 8, rect.Y - 8, rect.Width + 16, rect.Height + 16).Contains(p)) bits |= bit;
        return bits;
    }

    // ------------------------------------------------------------------
    // Touch handling
    // ------------------------------------------------------------------

    public override void TouchesBegan(NSSet touches, UIEvent? evt)
    {
        foreach (UITouch t in touches)
            _activeTouchBits[t.Handle.Handle] = HitTest(t.LocationInView(this));
        Recompute();
        CheckThreeFingerHold(evt);
    }

    public override void TouchesMoved(NSSet touches, UIEvent? evt)
    {
        foreach (UITouch t in touches)
            _activeTouchBits[t.Handle.Handle] = HitTest(t.LocationInView(this));
        Recompute();
    }

    public override void TouchesEnded(NSSet touches, UIEvent? evt)
    {
        foreach (UITouch t in touches)
            _activeTouchBits.Remove(t.Handle.Handle);
        Recompute();
        CancelHold();
    }

    public override void TouchesCancelled(NSSet touches, UIEvent? evt)
    {
        foreach (UITouch t in touches)
            _activeTouchBits.Remove(t.Handle.Handle);
        Recompute();
        CancelHold();
    }

    void Recompute()
    {
        ushort bits = 0;
        foreach (var b in _activeTouchBits.Values) bits |= b;
        if (bits == _currentState) return;

        bool newPress = (bits & ~_currentState) != 0;
        _currentState = bits;
        // SetVirtualPadState takes an ACTIVE-HIGH mask (same as Android).
        Controller.SetVirtualPadState(bits);

        if (newPress)
        {
            try { _haptic?.ImpactOccurred(); _haptic?.Prepare(); } catch { /* cosmetic */ }
        }
        SetNeedsDisplay();
    }

    void CheckThreeFingerHold(UIEvent? evt)
    {
        if (evt?.AllTouches?.Count >= 3)
        {
            CancelHold();
            _holdTimer = NSTimer.CreateScheduledTimer(HoldMilliseconds / 1000.0, false, _ =>
            {
                Controller.SetVirtualPadState(0);
                ThreeFingerHold?.Invoke();
            });
        }
    }

    void CancelHold()
    {
        _holdTimer?.Invalidate();
        _holdTimer = null;
    }

    // ------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------

    bool Down(ushort bit) => (_currentState & bit) != 0;

    public override void Draw(CGRect rect)
    {
        DrawDpad();
        DrawFaceButtons();
        DrawPills();
    }

    void DrawDpad()
    {
        double cx = (double)_dpadC.X, cy = (double)_dpadC.Y;
        double r = _dpadR, aw = _armW;

        // Base plate: one path for both arms so overlap doesn't double-blend.
        var plus = UIBezierPath.FromRoundedRect(new CGRect(cx - r, cy - aw / 2, r * 2, aw), aw * 0.30);
        plus.AppendPath(UIBezierPath.FromRoundedRect(new CGRect(cx - aw / 2, cy - r, aw, r * 2), aw * 0.30));
        IdleFill.SetFill();
        plus.Fill();

        // Pressed arm highlights.
        PressedFill.SetFill();
        if (Down(Controller.Up))
            UIBezierPath.FromRoundedRect(new CGRect(cx - aw / 2, cy - r, aw, r), aw * 0.30).Fill();
        if (Down(Controller.Down))
            UIBezierPath.FromRoundedRect(new CGRect(cx - aw / 2, cy, aw, r), aw * 0.30).Fill();
        if (Down(Controller.Left))
            UIBezierPath.FromRoundedRect(new CGRect(cx - r, cy - aw / 2, r, aw), aw * 0.30).Fill();
        if (Down(Controller.Right))
            UIBezierPath.FromRoundedRect(new CGRect(cx, cy - aw / 2, r, aw), aw * 0.30).Fill();

        // Arrows.
        double d = r * 0.68;   // distance from centre
        double s = r * 0.16;   // arrow half-size
        DrawArrow(cx, cy - d, 0, -1, s, Down(Controller.Up));
        DrawArrow(cx, cy + d, 0, 1, s, Down(Controller.Down));
        DrawArrow(cx - d, cy, -1, 0, s, Down(Controller.Left));
        DrawArrow(cx + d, cy, 1, 0, s, Down(Controller.Right));
    }

    static void DrawArrow(double x, double y, int dirX, int dirY, double s, bool pressed)
    {
        // Tip points along (dirX, dirY); base is perpendicular.
        var tip = new CGPoint(x + dirX * s, y + dirY * s);
        var b1 = new CGPoint(x - dirX * s + dirY * s, y - dirY * s + dirX * s);
        var b2 = new CGPoint(x - dirX * s - dirY * s, y - dirY * s - dirX * s);
        var path = new UIBezierPath();
        path.MoveTo(tip);
        path.AddLineTo(b1);
        path.AddLineTo(b2);
        path.ClosePath();
        (pressed ? ArrowPressed : ArrowIdle).SetFill();
        path.Fill();
    }

    void DrawFaceButtons()
    {
        DrawFaceButton(Controller.Triangle, TriangleColor);
        DrawFaceButton(Controller.Cross, CrossColor);
        DrawFaceButton(Controller.Square, SquareColor);
        DrawFaceButton(Controller.Circle, CircleColor);
    }

    void DrawFaceButton(ushort bit, UIColor color)
    {
        var c = FaceCenter(bit);
        double cx = (double)c.X, cy = (double)c.Y;
        double r = _faceBtnR;
        bool pressed = Down(bit);

        var disc = UIBezierPath.FromOval(new CGRect(cx - r, cy - r, r * 2, r * 2));
        (pressed ? PressedFill : IdleFill).SetFill();
        disc.Fill();
        IdleStroke.SetStroke();
        disc.LineWidth = 1.5f;
        disc.Stroke();

        // Symbol.
        color.ColorWithAlpha(pressed ? 1f : 0.85f).SetStroke();
        double k = r * 0.42;
        UIBezierPath sym;
        if (bit == Controller.Circle)
        {
            sym = UIBezierPath.FromOval(new CGRect(cx - k, cy - k, k * 2, k * 2));
        }
        else if (bit == Controller.Square)
        {
            sym = UIBezierPath.FromRect(new CGRect(cx - k, cy - k, k * 2, k * 2));
        }
        else if (bit == Controller.Cross)
        {
            sym = new UIBezierPath();
            sym.MoveTo(new CGPoint(cx - k, cy - k)); sym.AddLineTo(new CGPoint(cx + k, cy + k));
            sym.MoveTo(new CGPoint(cx + k, cy - k)); sym.AddLineTo(new CGPoint(cx - k, cy + k));
        }
        else // Triangle
        {
            sym = new UIBezierPath();
            sym.MoveTo(new CGPoint(cx, cy - k * 1.1));
            sym.AddLineTo(new CGPoint(cx + k * 1.05, cy + k * 0.8));
            sym.AddLineTo(new CGPoint(cx - k * 1.05, cy + k * 0.8));
            sym.ClosePath();
        }
        sym.LineWidth = 3f;
        sym.LineCapStyle = CGLineCap.Round;
        sym.LineJoinStyle = CGLineJoin.Round;
        sym.Stroke();
    }

    void DrawPills()
    {
        foreach (var (rect, bit) in _pills)
        {
            var pill = UIBezierPath.FromRoundedRect(rect, (double)rect.Height / 2.6);
            (Down(bit) ? PressedFill : IdleFill).SetFill();
            pill.Fill();
            IdleStroke.SetStroke();
            pill.LineWidth = 1.5f;
            pill.Stroke();
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) CancelHold();
        base.Dispose(disposing);
    }
}
