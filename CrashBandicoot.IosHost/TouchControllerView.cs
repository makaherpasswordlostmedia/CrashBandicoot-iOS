using CoreGraphics;
using Foundation;
using RecompOne.Runtime.Hardware;
using UIKit;

namespace CrashBandicoot.IosHost;

/// <summary>
/// iOS port of AndroidRuntimeHost/TouchControllerView.cs +
/// GameTouchRoot.cs combined into one UIView, since UIKit's touch
/// delivery (UIResponder.TouchesBegan/Moved/Ended/Cancelled with a
/// NSSet&lt;UITouch&gt;) already gives us all active touches per callback the
/// way Android's MotionEvent.PointerCount does - no need for two classes.
///
/// Feeds the exact same Controller.SetVirtualPadState(ushort) bitmask
/// used by every other host, so RecompOne.Runtime.Hardware.Controller and
/// everything downstream of it (InputManager, sdk/LibPad.cs) needs zero
/// changes.
/// </summary>
public sealed class TouchControllerView : UIView
{
    const int HoldMilliseconds = 500;

    // Layout: left-bottom = d-pad, right-bottom = face buttons, top corners
    // = L1/R1/Select/Start. Tune freely; this is a first pass, not final art.
    CGRect _dpadRect, _faceRect, _l1Rect, _r1Rect, _selectRect, _startRect;
    (CGRect rect, ushort bit)[] _dpadZones = Array.Empty<(CGRect, ushort)>();
    (CGRect rect, ushort bit)[] _faceZones = Array.Empty<(CGRect, ushort)>();

    readonly Dictionary<nint, ushort> _activeTouchBits = new();
    ushort _currentState;

    NSTimer? _holdTimer;
    public Action? ThreeFingerHold { get; set; }

    public TouchControllerView(CGRect frame) : base(frame)
    {
        BackgroundColor = UIColor.Clear;
        MultipleTouchEnabled = true;
        UserInteractionEnabled = true;
        LayoutZones();
    }

    public override void LayoutSubviews()
    {
        base.LayoutSubviews();
        LayoutZones();
    }

    void LayoutZones()
    {
        var b = Bounds;
        nfloat pad = 24;
        nfloat dpadSize = 150;
        nfloat faceSize = 150;
        nfloat smallBtn = 56;

        _dpadRect = new CGRect(pad, b.Height - dpadSize - pad, dpadSize, dpadSize);
        _faceRect = new CGRect(b.Width - faceSize - pad, b.Height - faceSize - pad, faceSize, faceSize);
        _l1Rect = new CGRect(pad, pad, smallBtn * 1.6, smallBtn);
        _r1Rect = new CGRect(b.Width - smallBtn * 1.6 - pad, pad, smallBtn * 1.6, smallBtn);
        _selectRect = new CGRect(b.Width / 2 - smallBtn * 1.3, pad, smallBtn * 1.1, smallBtn * 0.7);
        _startRect = new CGRect(b.Width / 2 + smallBtn * 0.2, pad, smallBtn * 1.1, smallBtn * 0.7);

        var dw = _dpadRect.Width / 3;
        var dh = _dpadRect.Height / 3;
        var dx = _dpadRect.X;
        var dy = _dpadRect.Y;
        _dpadZones = new (CGRect, ushort)[]
        {
            (new CGRect(dx + dw, dy, dw, dh), Controller.Up),
            (new CGRect(dx + dw, dy + dh * 2, dw, dh), Controller.Down),
            (new CGRect(dx, dy + dh, dw, dh), Controller.Left),
            (new CGRect(dx + dw * 2, dy + dh, dw, dh), Controller.Right),
        };

        var fw = _faceRect.Width / 3;
        var fh = _faceRect.Height / 3;
        var fx = _faceRect.X;
        var fy = _faceRect.Y;
        _faceZones = new (CGRect, ushort)[]
        {
            (new CGRect(fx + fw, fy, fw, fh), Controller.Triangle),
            (new CGRect(fx + fw, fy + fh * 2, fw, fh), Controller.Cross),
            (new CGRect(fx, fy + fh, fw, fh), Controller.Square),
            (new CGRect(fx + fw * 2, fy + fh, fw, fh), Controller.Circle),
        };
    }

    ushort HitTest(CGPoint p)
    {
        ushort bits = 0;
        foreach (var (rect, bit) in _dpadZones)
            if (rect.Contains(p)) bits |= bit;
        foreach (var (rect, bit) in _faceZones)
            if (rect.Contains(p)) bits |= bit;
        if (_l1Rect.Contains(p)) bits |= Controller.L1;
        if (_r1Rect.Contains(p)) bits |= Controller.R1;
        if (_selectRect.Contains(p)) bits |= Controller.Select;
        if (_startRect.Contains(p)) bits |= Controller.Start;
        return bits;
    }

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
        // Controller.State is active-low (0xFFFF = nothing pressed), but
        // SetVirtualPadState takes an active-high mask per its own doc
        // comment - matches Android's usage in AndroidGamepad.cs exactly.
        if (bits != _currentState)
        {
            _currentState = bits;
            Controller.SetVirtualPadState(bits);
        }
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
}
