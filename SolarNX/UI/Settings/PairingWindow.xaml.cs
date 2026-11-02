using System.Windows;
using System.Windows.Threading;
using SolarNX.Session;

namespace SolarNX.UI.Settings;

/// <summary>
/// Opens pairing on the running Internet session for two minutes and walks the player through it:
/// waiting for the Switch, then the 6-digit code to compare with the Switch's screen, then the
/// result. Closing the window closes pairing.
/// </summary>
public partial class PairingWindow : Wpf.Ui.Controls.FluentWindow
{
    private static readonly TimeSpan PairingTime = TimeSpan.FromMinutes(2);

    private readonly InternetSession _session;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly DateTime _closesAt = DateTime.Now + PairingTime;
    private bool _done;

    public PairingWindow(InternetSession session)
    {
        InitializeComponent();
        _session = session;
        _session.PairingRequested += OnPairingRequested;
        _session.OpenPairing(PairingTime);

        AcceptButton.Click += (_, _) => Decide(true);
        RejectButton.Click += (_, _) => Decide(false);
        CloseButton.Click += (_, _) => Close();
        _timer.Tick += (_, _) =>
        {
            if (_done || CodeText.Visibility == Visibility.Visible) return;
            var left = _closesAt - DateTime.Now;
            if (left <= TimeSpan.Zero)
            {
                _done = true;
                StatusText.Text = Loc.T("Pair.TimedOut");
                HintText.Text = Loc.T("Pair.TryAgain");
                _timer.Stop();
                return;
            }
            HintText.Text = Loc.F("Pair.HintLeft", left.ToString(@"m\:ss"));
        };
        _timer.Start();
        Closed += (_, _) =>
        {
            _timer.Stop();
            _session.PairingRequested -= OnPairingRequested;
            _session.ClosePairing();
        };
    }

    // Network thread.
    private void OnPairingRequested(string name, int code) => Dispatcher.BeginInvoke(() =>
    {
        if (_done) return;
        StatusText.Text = Loc.F("Pair.Request", name);
        CodeText.Text = $"{code / 1000:000} {code % 1000:000}";
        CodeText.Visibility = Visibility.Visible;
        HintText.Text = Loc.T("Pair.Warning");
        AcceptButton.Visibility = RejectButton.Visibility = Visibility.Visible;
    });

    private void Decide(bool accept)
    {
        _done = true;
        _session.CompletePairing(accept);
        AcceptButton.Visibility = RejectButton.Visibility = Visibility.Collapsed;
        CodeText.Visibility = Visibility.Collapsed;
        StatusText.Text = Loc.T(accept ? "Pair.Done" : "Pair.Rejected");
        HintText.Text = accept ? Loc.T("Pair.DoneHint") : "";
    }
}
