using System.Buffers.Binary;
using Nefarius.ViGEm.Client;
using Nefarius.ViGEm.Client.Targets;
using Nefarius.ViGEm.Client.Targets.Xbox360;

namespace SolarNX.Input;

/// <summary>Which virtual controller type new connections use - see VirtualGamepadHub.Mode.</summary>
public enum ControllerMode
{
    Xbox360,
    DualShock4,
}

/// <summary>
/// Owns up to 4 virtual controllers via ViGEmBus (Nefarius.ViGEm.Client) - an independent, actively
/// maintained driver (also used by Sunshine, DS4Windows). Mirrors main.js's handleControllerInput
/// mapping exactly (button swap, boolean-threshold triggers, dpad combination logic). Deliberately
/// does not touch the real mouse/keyboard - all input is gamepad-only.
///
/// Two controller types are supported (see <see cref="Mode"/>): Xbox360 (default, used for Steam/
/// Big Picture - the client only ever sees "a standard Xbox 360 controller" through XInput) and
/// DualShock4 (used for shadPS4). DualShock4 is the only path that can carry real gyro/accelerometer
/// data - XInput/Xbox360 has no motion-sensor concept at all, and ViGEm's convenience DS4 API
/// (SetAxisValue/SetButtonState/...) has no gyro/accel setters either, so the DS4 path builds and
/// submits the full 63-byte raw HID report by hand (SubmitRawReport) instead. That layout was
/// cross-verified against two independent sources (ViGEmClient's own DS4_REPORT_EX struct and the
/// Linux kernel's hid-playstation.c driver, which agree): bThumbLX/LY/RX/RY (bytes 0-3), wButtons
/// (bytes 4-5: bits 0-3 dpad hat, 4=Square 5=Cross 6=Circle 7=Triangle, 8=L1 9=R1 10=L2 11=R2
/// 12=Share 13=Options 14=L3 15=R3), bSpecial (byte 6: PS/touchpad, unused here), bTriggerL/R
/// (bytes 7-8), wTimestamp (bytes 9-10), bBatteryLvl (byte 11), wGyroX/Y/Z (bytes 12-17, signed),
/// wAccelX/Y/Z (bytes 18-23, signed), remaining bytes (touchpad/reserved) zeroed.
///
/// The exact raw-unit calibration (how many gyro/accel units per real deg/s or g) could not be
/// verified against real hardware - see GyroRawUnitsPerDegPerSecond/AccelRawUnitsPerG below; these
/// are reasoned defaults, tune them if motion controls feel too weak/strong/inverted once tested
/// live in an actual game.
/// </summary>
public sealed class VirtualGamepadHub : IDisposable
{
    /// <summary>True (default) swaps A/B and X/Y (or the DualShock4 equivalent positions) to match
    /// the client's physical Nintendo Switch controller layout - the Switch's own button positions
    /// are diagonally mirrored relative to an Xbox/DS4 controller's, so without this, prompts/muscle
    /// memory from an Xbox-style game would point at the physically wrong button. Settable at
    /// runtime from the main window's checkbox (see MainForm) - takes effect on the very next
    /// packet, no reconnect needed.</summary>
    public bool SwapAbxy { get; set; } = true;

    /// <summary>Which virtual controller type new connections use. Switching disconnects any
    /// currently-connected controllers of the OTHER type first - a game/Steam Input expects one
    /// consistent controller identity per session, not both types plugged in at once. Set by
    /// AppOrchestrator based on the active AppTarget (ShadPs4 -> DualShock4, BigPicture ->
    /// Xbox360).</summary>
    public ControllerMode Mode
    {
        get => _mode;
        set
        {
            if (_mode == value) return;
            _mode = value;
            DisconnectAll();
        }
    }
    private ControllerMode _mode = ControllerMode.Xbox360;

    private readonly ViGEmClient _client = new();
    private readonly IXbox360Controller?[] _xbox360Controllers = new IXbox360Controller?[5]; // 1-based, 0 unused
    private readonly IDualShock4Controller?[] _ds4Controllers = new IDualShock4Controller?[5];

    // Minus+Plus -> Guide/PS, per controller slot (1-based like the arrays above) - see GuideButtonChord.
    private readonly GuideButtonChord[] _guideChords = Enumerable.Range(0, 5).Select(_ => new GuideButtonChord()).ToArray();

    public VirtualGamepadHub()
    {
        GameBarSuppressor.Disable();
    }

    public void Apply(HidPacket packet)
    {
        var count = Math.Clamp((int)packet.ControllerCount, 0, 4);

        for (var i = 0; i < 4; i++)
        {
            var id = i + 1;
            if (i < count)
            {
                var state = packet.Controllers[i];
                var chord = _guideChords[id].Update(state.Minus, state.Plus);
                if (_mode == ControllerMode.Xbox360)
                {
                    if (_xbox360Controllers[id] is null)
                    {
                        _xbox360Controllers[id] = PlugInXbox360(id);
                    }
                    ApplyXbox360State(_xbox360Controllers[id]!, state, chord, SwapAbxy);
                }
                else
                {
                    if (_ds4Controllers[id] is null)
                    {
                        _ds4Controllers[id] = PlugInDs4(id);
                    }
                    ApplyDs4State(_ds4Controllers[id]!, state, chord, SwapAbxy);
                }
            }
            else
            {
                if (_xbox360Controllers[id] is not null) UnplugXbox360At(id);
                if (_ds4Controllers[id] is not null) UnplugDs4At(id);
                _guideChords[id].Reset();
            }
        }
    }

    private IXbox360Controller PlugInXbox360(int id)
    {
        var pad = _client.CreateXbox360Controller();
        pad.Connect();
        Logger.Log("VirtualGamepad", $"controller {id} connected via ViGEmBus (Xbox360)");
        return pad;
    }

    private readonly CancellationTokenSource?[] _ds4DrainCts = new CancellationTokenSource?[5];

    private IDualShock4Controller PlugInDs4(int id)
    {
        var pad = _client.CreateDualShock4Controller();
        pad.Connect();
        Logger.Log("VirtualGamepad", $"controller {id} connected via ViGEmBus (DualShock4)");

        // Rumble/lightbar-color output reports have nowhere to go (no haptics on the Switch side to
        // forward them to) - but they still need to be actively consumed. Suspected root cause of a
        // confirmed live regression (video stutter/artifacts tied to WSAENOBUFS - UDP send buffer
        // exhaustion - specifically in DualShock4 mode): a game continuously sending output reports
        // (e.g. per-frame lightbar color) with nothing on this end ever reading them could back up
        // the underlying report queue and stall the writer (shadPS4) waiting for buffer space.
        // AwaitRawOutputReport (not the FeedbackReceived event, which ViGEm.Client itself marks
        // obsolete/unreliable) is the documented way to drain this. Xbox360 has no equivalent stall
        // surface (no output-report handling exists in ApplyXbox360State either, and that mode has
        // never shown this symptom).
        // A dedicated Thread, not Task.Run: this loop spends virtually all its time blocked inside
        // AwaitRawOutputReport for as long as the controller stays connected (which can be the
        // entire session), and Task.Run schedules onto the shared .NET thread pool - a pool sized
        // and tuned for short-lived work. Parking a thread-pool worker in a long-running blocking
        // wait like this starves the pool for everything else that needs it (async socket I/O
        // completions for the video/audio/HID sockets included), and .NET's thread-pool growth is
        // deliberately slow (roughly one new thread per ~1s under the hill-climbing algorithm) -
        // confirmed live as the cause of a real regression: video lag and audio glitching that
        // persisted for the rest of the process's life even after switching back to Xbox360/Steam
        // in the same run, well past when this specific DS4 session had already ended.
        var cts = new CancellationTokenSource();
        _ds4DrainCts[id] = cts;
        var thread = new Thread(() => DrainDs4OutputReports(pad, id, cts.Token))
        {
            IsBackground = true,
            Name = $"Ds4OutputDrain-{id}",
        };
        thread.Start();

        return pad;
    }

    private static void DrainDs4OutputReports(IDualShock4Controller pad, int id, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                // Bounded per-call wait (not the no-arg overload, which blocks indefinitely) so this
                // loop notices cancellation within ~500ms of Disconnect() instead of potentially
                // blocking forever on a device that's already gone.
                foreach (var _ in pad.AwaitRawOutputReport(500, out _))
                {
                    if (ct.IsCancellationRequested) break;
                    // discarded - draining is the point, see PlugInDs4's comment
                }
            }
            catch (Exception ex)
            {
                if (ct.IsCancellationRequested) break;
                Logger.Log("VirtualGamepad", $"controller {id} output-report drain loop ended: {ex.Message}");
                break;
            }
        }
    }

    private void UnplugXbox360At(int id)
    {
        try { _xbox360Controllers[id]?.Disconnect(); }
        catch (Exception ex) { Logger.Log("VirtualGamepad", $"controller {id} disconnect failed (already gone?): {ex.Message}"); }
        _xbox360Controllers[id] = null;
        _guideChords[id].Reset();
        Logger.Log("VirtualGamepad", $"controller {id} disconnected (Xbox360)");
    }

    private void UnplugDs4At(int id)
    {
        _ds4DrainCts[id]?.Cancel();
        _ds4DrainCts[id] = null;
        try { _ds4Controllers[id]?.Disconnect(); }
        catch (Exception ex) { Logger.Log("VirtualGamepad", $"controller {id} disconnect failed (already gone?): {ex.Message}"); }
        _ds4Controllers[id] = null;
        _guideChords[id].Reset();
        Logger.Log("VirtualGamepad", $"controller {id} disconnected (DualShock4)");
    }

    private static void ApplyXbox360State(IXbox360Controller pad, ControllerState s, GuideButtonChord.Output chord, bool swapAbxy)
    {
        pad.SetAxisValue(Xbox360Axis.LeftThumbX, ToShortAxis(ConvertAnalog(s.LJoyX)));
        pad.SetAxisValue(Xbox360Axis.LeftThumbY, ToShortAxis(ConvertAnalog(s.LJoyY)));
        pad.SetAxisValue(Xbox360Axis.RightThumbX, ToShortAxis(ConvertAnalog(s.RJoyX)));
        pad.SetAxisValue(Xbox360Axis.RightThumbY, ToShortAxis(ConvertAnalog(s.RJoyY)));

        // Confirmed live: the two branches below were swapped relative to reality - with
        // swapAbxy=true (the "Раскладка Switch" toggle checked), the controller actually behaved
        // as plain Xbox passthrough, and vice versa. Branch bodies fixed in place; the swapAbxy
        // condition itself is unchanged.
        if (swapAbxy)
        {
            pad.SetButtonState(Xbox360Button.B, s.B);
            pad.SetButtonState(Xbox360Button.A, s.A);
            pad.SetButtonState(Xbox360Button.X, s.X);
            pad.SetButtonState(Xbox360Button.Y, s.Y);
        }
        else
        {
            pad.SetButtonState(Xbox360Button.B, s.A);
            pad.SetButtonState(Xbox360Button.A, s.B);
            pad.SetButtonState(Xbox360Button.X, s.Y);
            pad.SetButtonState(Xbox360Button.Y, s.X);
        }

        // Minus/Plus go through the chord filter (Minus+Plus = Guide, i.e. Steam's menu button).
        pad.SetButtonState(Xbox360Button.Back, chord.Minus);
        pad.SetButtonState(Xbox360Button.Start, chord.Plus);
        pad.SetButtonState(Xbox360Button.Guide, chord.Guide);
        pad.SetButtonState(Xbox360Button.LeftShoulder, s.L);
        pad.SetButtonState(Xbox360Button.RightShoulder, s.R);
        pad.SetButtonState(Xbox360Button.LeftThumb, s.LS);
        pad.SetButtonState(Xbox360Button.RightThumb, s.RS);
        pad.SetButtonState(Xbox360Button.Up, s.Up);
        pad.SetButtonState(Xbox360Button.Down, s.Down);
        pad.SetButtonState(Xbox360Button.Left, s.Left);
        pad.SetButtonState(Xbox360Button.Right, s.Right);

        // ZL/ZR are boolean in the wire format, so triggers are all-or-nothing (matches main.js).
        pad.SetSliderValue(Xbox360Slider.LeftTrigger, (byte)(s.ZL ? 255 : 0));
        pad.SetSliderValue(Xbox360Slider.RightTrigger, (byte)(s.ZR ? 255 : 0));
    }

    // --- DualShock4 raw-report path (see class doc comment for the verified byte layout) ---

    private const int Ds4ReportSize = 63; // matches ViGEm's DS4_REPORT_EX.ReportBuffer size exactly

    // Unverifiable against real hardware without a physical DS4 to compare against - the consuming
    // game/its own DS4 calibration ultimately decides what these raw units mean. Reasoned defaults
    // based on commonly-cited DS4 sensor resolutions; tune if motion feels too weak/strong.
    private const float GyroRawUnitsPerDegPerSecond = 16.0f;
    private const float AccelRawUnitsPerG = 8192.0f;

    private ushort _ds4Timestamp;

    private void ApplyDs4State(IDualShock4Controller pad, ControllerState s, GuideButtonChord.Output chord, bool swapAbxy)
    {
        Span<byte> report = stackalloc byte[Ds4ReportSize];

        report[0] = ToDs4AxisByte(ConvertAnalog(s.LJoyX), invert: false);
        report[1] = ToDs4AxisByte(ConvertAnalog(s.LJoyY), invert: true);
        report[2] = ToDs4AxisByte(ConvertAnalog(s.RJoyX), invert: false);
        report[3] = ToDs4AxisByte(ConvertAnalog(s.RJoyY), invert: true);

        // See ApplyXbox360State's comment - branches fixed the same way here.
        bool circle, cross, square, triangle;
        if (swapAbxy) { cross = s.A; circle = s.B; square = s.X; triangle = s.Y; }
        else { circle = s.A; cross = s.B; square = s.Y; triangle = s.X; }

        ushort buttons = (ushort)ToDs4DPadHat(s.Up, s.Down, s.Left, s.Right);
        if (square) buttons |= 1 << 4;
        if (cross) buttons |= 1 << 5;
        if (circle) buttons |= 1 << 6;
        if (triangle) buttons |= 1 << 7;
        if (s.L) buttons |= 1 << 8;
        if (s.R) buttons |= 1 << 9;
        if (s.ZL) buttons |= 1 << 10;
        if (s.ZR) buttons |= 1 << 11;
        if (chord.Minus) buttons |= 1 << 12; // Share - via the chord filter, see GuideButtonChord
        if (chord.Plus) buttons |= 1 << 13; // Options
        if (s.LS) buttons |= 1 << 14;
        if (s.RS) buttons |= 1 << 15;
        BinaryPrimitives.WriteUInt16LittleEndian(report[4..6], buttons);

        report[6] = (byte)(chord.Guide ? 0x01 : 0x00); // bSpecial: bit 0 = PS (Minus+Plus chord), bit 1 = touchpad click (unused)
        report[7] = (byte)(s.ZL ? 255 : 0); // bTriggerL - boolean in the wire format, matches Xbox360 path
        report[8] = (byte)(s.ZR ? 255 : 0); // bTriggerR

        // Real hardware increments this at a fixed internal clock rate the game uses for its own
        // gyro integration - the exact tick unit couldn't be confirmed (see class doc comment), so
        // this just increments steadily call-to-call, which is enough for a monotonic, non-repeating
        // value; a fixed step is a reasonable stand-in given calls arrive at a roughly steady rate.
        _ds4Timestamp += 188;
        BinaryPrimitives.WriteUInt16LittleEndian(report[9..11], _ds4Timestamp);
        report[11] = 0xFF; // bBatteryLvl - cosmetic only, "full" placeholder

        // Every controller now carries its own motion reading (see ControllerState's doc comment) -
        // no more "primary player only" special-casing here.
        WriteS16(report[12..14], s.GyroX * (180f / MathF.PI) * GyroRawUnitsPerDegPerSecond);
        WriteS16(report[14..16], s.GyroY * (180f / MathF.PI) * GyroRawUnitsPerDegPerSecond);
        WriteS16(report[16..18], s.GyroZ * (180f / MathF.PI) * GyroRawUnitsPerDegPerSecond);
        WriteS16(report[18..20], s.AccelX * AccelRawUnitsPerG);
        WriteS16(report[20..22], s.AccelY * AccelRawUnitsPerG);
        WriteS16(report[22..24], s.AccelZ * AccelRawUnitsPerG);
        // Bytes 24-62 (unknown/reserved, secondary battery, touchpad packets) intentionally left
        // zeroed - no Switch-side equivalent data exists for the touchpad, and zero is what an
        // idle/untouched touchpad reports on real hardware.

        pad.SubmitRawReport(report.ToArray());
    }

    private static void WriteS16(Span<byte> dest, float value)
    {
        var clamped = (short)Math.Clamp(value, short.MinValue, short.MaxValue);
        BinaryPrimitives.WriteInt16LittleEndian(dest, clamped);
    }

    // DS4's Y axes report 0=up/255=down on real hardware (inverted relative to the signed
    // convention ConvertAnalog/the Xbox360 path already use) - X axes don't need this.
    private static byte ToDs4AxisByte(double normalized, bool invert)
    {
        var n = invert ? -normalized : normalized;
        return (byte)Math.Clamp((n + 1) / 2 * 255, 0, 255);
    }

    // Standard HID hat-switch encoding: 0=N,1=NE,2=E,3=SE,4=S,5=SW,6=W,7=NW,8=neutral - matches
    // ViGEm's own DualShock4DPadDirection values, replicated here since the raw-report path builds
    // wButtons by hand rather than going through that convenience enum.
    private static int ToDs4DPadHat(bool up, bool down, bool left, bool right)
    {
        if (up && right) return 1;
        if (down && right) return 3;
        if (down && left) return 5;
        if (up && left) return 7;
        if (up) return 0;
        if (right) return 2;
        if (down) return 4;
        if (left) return 6;
        return 8;
    }

    // Mirrors main.js's convertAnalog: normalizes the raw wire int32 into [-1, 1].
    private static double ConvertAnalog(int axis)
    {
        var na = axis / 32767.5;
        if (na > 1)
        {
            na = 2 - na;
            na = -na;
        }
        return na;
    }

    private static short ToShortAxis(double normalized) =>
        (short)Math.Clamp(normalized * 32766, short.MinValue, short.MaxValue);

    public void DisconnectAll()
    {
        for (var id = 1; id <= 4; id++)
        {
            if (_xbox360Controllers[id] is not null) UnplugXbox360At(id);
            if (_ds4Controllers[id] is not null) UnplugDs4At(id);
        }
    }

    public void Dispose()
    {
        DisconnectAll();
        _client.Dispose();
    }
}
