namespace SolarNX.Input;

/// <summary>
/// Turns Minus+Plus pressed together on the Switch into the virtual pad's Guide button (Xbox360 -
/// opens Steam's menu in Big Picture) / PS button (DualShock4). The Switch's own Home button never
/// reaches a homebrew app - the system takes it to the Switch home menu - and neither does Capture,
/// so a chord is the only way to produce the Steam button. Minus+Plus mirrors the familiar
/// Select+Start convention (Moonlight, RetroArch).
///
/// The two buttons are never pressed in exactly the same instant, so a naive mapping would let
/// Big Picture see a short Back or Start first (opening the wrong menu) before Guide. Instead a
/// lone Minus or Plus is withheld for up to <see cref="ChordWindow"/> waiting for its partner:
/// - partner arrives in time: Guide;
/// - held alone past the window: reported as itself from then on;
/// - tapped and released inside the window: still delivered, as a one-packet tap on release - so
///   nothing is lost, just up to ChordWindow late, which is fine for menu/pause buttons.
/// Once the chord has fired, Back/Start stay suppressed until both buttons are released, so
/// letting go of one first can't leak a stray press either.
///
/// One instance per virtual controller slot; Update is called once per incoming HID packet.
/// </summary>
internal sealed class GuideButtonChord
{
    private static readonly long ChordWindowMs = 100;

    private enum State { Idle, Pending, LonePress, Chord }

    private State _state = State.Idle;
    private bool _pendingIsMinus;
    private long _pendingSinceMs;

    public readonly record struct Output(bool Minus, bool Plus, bool Guide);

    public Output Update(bool minus, bool plus)
    {
        if (minus && plus)
        {
            _state = State.Chord;
            return new Output(false, false, true);
        }

        if (!minus && !plus)
        {
            var wasPending = _state == State.Pending;
            _state = State.Idle;
            // Released before the window ran out: the press was never reported - deliver it now as
            // a tap (pressed this packet, released on the next one, which arrives back in Idle).
            return wasPending ? new Output(_pendingIsMinus, !_pendingIsMinus, false) : new Output(false, false, false);
        }

        // Exactly one of the two is down.
        switch (_state)
        {
            case State.Chord:
                return new Output(false, false, false); // letting go of the chord one finger at a time

            case State.Idle:
                _state = State.Pending;
                _pendingIsMinus = minus;
                _pendingSinceMs = Environment.TickCount64;
                return new Output(false, false, false);

            case State.Pending:
                if (Environment.TickCount64 - _pendingSinceMs < ChordWindowMs) return new Output(false, false, false);
                _state = State.LonePress;
                return new Output(minus, plus, false);

            default: // LonePress
                return new Output(minus, plus, false);
        }
    }

    public void Reset() => _state = State.Idle;
}
