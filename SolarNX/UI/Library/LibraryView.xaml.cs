using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using SolarNX.Library;
using Wpf.Ui.Controls;
// UseWindowsForms + UseWPF + WPF-UI all define these names - pin the WPF ones.
using Brushes = System.Windows.Media.Brushes;
using Color = System.Windows.Media.Color;
using MessageBoxButton = System.Windows.MessageBoxButton;
using MessageBoxImage = System.Windows.MessageBoxImage;
using MessageBoxResult = System.Windows.MessageBoxResult;
using TextBlock = System.Windows.Controls.TextBlock;

namespace SolarNX.UI.Library;

/// <summary>
/// The host-side game library list: one card per entry (cover thumbnail, name, how it starts) with
/// edit/delete, plus "add". Editing happens in GameEditWindow; every change is saved by GameLibrary
/// immediately (same no-Save-button pattern as SettingsView) and reaches the Switch on the next
/// LibraryPusher round while streaming is enabled.
/// </summary>
public partial class LibraryView : System.Windows.Controls.UserControl
{
    private readonly GameLibrary _library;

    public event Action? BackRequested;

    public LibraryView(GameLibrary library)
    {
        InitializeComponent();
        _library = library;

        BackButton.Click += (_, _) => BackRequested?.Invoke();
        AddButton.Click += (_, _) => Edit(null);
        // Changed can come from any thread (GameLibrary is thread-safe) - rebuild on the UI thread.
        _library.Changed += () => Dispatcher.BeginInvoke(Rebuild);
        Loc.Changed += Rebuild;
        Rebuild();
    }

    private void Edit(GameEntry? existing)
    {
        var dialog = new GameEditWindow(_library, existing) { Owner = Window.GetWindow(this) };
        dialog.ShowDialog();
    }

    private void Rebuild()
    {
        EntryList.Children.Clear();
        var games = _library.Snapshot();
        EmptyHint.Visibility = games.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        foreach (var game in games) EntryList.Children.Add(BuildCard(game));
    }

    private UIElement BuildCard(GameEntry game)
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var cover = new Border
        {
            Width = 60,
            Height = 90,
            CornerRadius = new CornerRadius(6),
            Background = new SolidColorBrush(Color.FromRgb(0x2A, 0x2A, 0x36)),
            Margin = new Thickness(0, 0, 14, 0),
            ClipToBounds = true,
        };
        var image = CoverLoader.LoadAsTile(GameLibrary.CoverPath(game), game.CoverCrop, decodeWidth: 240);
        if (image is not null) cover.Child = new System.Windows.Controls.Image { Source = image, Stretch = Stretch.UniformToFill };
        grid.Children.Add(cover);

        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock { Text = game.Name, Foreground = Brushes.White, FontSize = 15, FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis });
        text.Children.Add(new TextBlock { Text = Describe(game), Foreground = new SolidColorBrush(Color.FromRgb(0xAA, 0xAA, 0xAA)), FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 4, 0, 0) });
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);

        var buttons = new StackPanel { Orientation = System.Windows.Controls.Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        var edit = new Wpf.Ui.Controls.Button { Appearance = ControlAppearance.Secondary, ToolTip = Loc.T("Library.Edit"), Margin = new Thickness(0, 0, 8, 0), Icon = new SymbolIcon(SymbolRegular.Edit24) };
        edit.Click += (_, _) => Edit(game);
        var delete = new Wpf.Ui.Controls.Button { Appearance = ControlAppearance.Danger, ToolTip = Loc.T("Library.Delete"), Icon = new SymbolIcon(SymbolRegular.Delete24) };
        delete.Click += (_, _) =>
        {
            var answer = System.Windows.MessageBox.Show(Window.GetWindow(this), Loc.F("Library.DeleteConfirm", game.Name), "SolarNX",
                MessageBoxButton.YesNo, MessageBoxImage.Question);
            if (answer == MessageBoxResult.Yes) _library.Remove(game.Id);
        };
        buttons.Children.Add(edit);
        buttons.Children.Add(delete);
        Grid.SetColumn(buttons, 2);
        grid.Children.Add(buttons);

        return new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x1A, 0x1A, 0x24)),
            CornerRadius = new CornerRadius(12),
            Padding = new Thickness(14),
            Margin = new Thickness(0, 0, 0, 10),
            Child = grid,
        };
    }

    private static string Describe(GameEntry game) => game.Type switch
    {
        GameEntryType.Native => Loc.F("Library.DescGame", game.ExePath),
        GameEntryType.Steam => $"Steam · AppID {game.SteamAppId}",
        GameEntryType.Emulator => Loc.F("Library.DescEmulator", Path.GetFileName(game.EmulatorPath), Path.GetFileName(game.RomPath)),
        GameEntryType.Script => Loc.T("Library.DescScript"),
        _ => "",
    } + (game.Controller == Input.ControllerMode.DualShock4 ? " · DualShock 4" : " · Xbox 360");
}
