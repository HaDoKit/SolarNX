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
                StatusText.Text = "Время вышло — привязка закрыта.";
                HintText.Text = "Откройте окно ещё раз, чтобы попробовать снова.";
                _timer.Stop();
                return;
            }
            HintText.Text = $"На Switch: главное меню → X → Pair with this PC. Осталось {left:m\\:ss}.";
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
        StatusText.Text = $"«{name}» просит привязку. Сравните код с экраном Switch:";
        CodeText.Text = $"{code / 1000:000} {code % 1000:000}";
        CodeText.Visibility = Visibility.Visible;
        HintText.Text = "Привязывайте, только если коды совпадают. Если нет — кто-то пытается подключиться вместо вашего Switch.";
        AcceptButton.Visibility = RejectButton.Visibility = Visibility.Visible;
    });

    private void Decide(bool accept)
    {
        _done = true;
        _session.CompletePairing(accept);
        AcceptButton.Visibility = RejectButton.Visibility = Visibility.Collapsed;
        CodeText.Visibility = Visibility.Collapsed;
        StatusText.Text = accept ? "Готово: Switch привязан." : "Привязка отклонена.";
        HintText.Text = accept ? "Теперь Switch подключается к этому ПК через интернет без кода." : "";
    }
}
