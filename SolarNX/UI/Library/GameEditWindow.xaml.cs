using System.Windows;
using SolarNX.Input;
using SolarNX.Library;

namespace SolarNX.UI.Library;

/// <summary>
/// Add/edit one library entry. Works on a copy and writes it back through GameLibrary only on
/// "Сохранить", so cancelling leaves the library (and the Switch's copy) untouched. A newly chosen
/// cover is only copied into the library's covers folder on save.
/// </summary>
public partial class GameEditWindow : Wpf.Ui.Controls.FluentWindow
{
    private static readonly (GameEntryType Type, string Label)[] Types =
    {
        (GameEntryType.Native, "Обычная игра (exe)"),
        (GameEntryType.Steam, "Игра из Steam"),
        (GameEntryType.Emulator, "Эмулятор"),
        (GameEntryType.Script, "Свой скрипт (cmd)"),
    };

    private static readonly (ControllerMode Mode, string Label)[] Controllers =
    {
        (ControllerMode.Xbox360, "Xbox 360"),
        (ControllerMode.DualShock4, "DualShock 4 (с гироскопом)"),
    };

    private readonly GameLibrary _library;
    private readonly GameEntry _entry;

    // Cover state: the file currently shown, and whether it's a new pick that still has to be
    // imported (vs. the entry's existing CoverFile) or an explicit removal.
    private string? _coverSourcePath;
    private bool _coverChanged;
    private CoverCrop? _coverCrop; // null = centre crop; reset whenever the image changes

    public GameEditWindow(GameLibrary library, GameEntry? existing)
    {
        InitializeComponent();
        _library = library;
        _entry = existing?.Clone() ?? new GameEntry();
        Title = TitleBar.Title = existing is null ? "Новая игра" : "Изменить игру";

        foreach (var t in Types) TypeCombo.Items.Add(t.Label);
        foreach (var c in Controllers) ControllerCombo.Items.Add(c.Label);

        NameBox.Text = _entry.Name;
        TypeCombo.SelectedIndex = Array.FindIndex(Types, t => t.Type == _entry.Type);
        ExePathBox.Text = _entry.ExePath;
        ArgumentsBox.Text = _entry.Arguments;
        WorkingDirBox.Text = _entry.WorkingDirectory;
        SteamAppIdBox.Text = _entry.SteamAppId;
        EmulatorPathBox.Text = _entry.EmulatorPath;
        RomPathBox.Text = _entry.RomPath;
        ArgumentTemplateBox.Text = _entry.ArgumentTemplate;
        ScriptBox.Text = _entry.Script;
        ControllerCombo.SelectedIndex = Math.Max(0, Array.FindIndex(Controllers, c => c.Mode == _entry.Controller));
        CaptureWindowCheck.IsChecked = _entry.CaptureGameWindow;
        RunAsAdminCheck.IsChecked = _entry.RunAsAdmin;
        _coverSourcePath = GameLibrary.CoverPath(_entry);
        _coverCrop = _entry.CoverCrop?.Clone();
        ShowCover();

        TypeCombo.SelectionChanged += (_, _) => UpdatePanels();
        UpdatePanels();

        BrowseExeButton.Click += (_, _) => BrowseFile(ExePathBox, "Программы (*.exe)|*.exe|Все файлы (*.*)|*.*", fillName: true);
        BrowseEmulatorButton.Click += (_, _) => BrowseFile(EmulatorPathBox, "Программы (*.exe)|*.exe|Все файлы (*.*)|*.*", fillName: false);
        BrowseRomButton.Click += (_, _) => BrowseFile(RomPathBox, "Все файлы (*.*)|*.*", fillName: true);
        BrowseWorkingDirButton.Click += (_, _) => BrowseFolder(WorkingDirBox);
        ChooseCoverButton.Click += (_, _) => ChooseCover();
        CropCoverButton.Click += (_, _) => CropCover();
        ClearCoverButton.Click += (_, _) => { _coverSourcePath = null; _coverCrop = null; _coverChanged = true; ShowCover(); };
        SteamGameCombo.SelectionChanged += (_, _) => OnSteamGamePicked();
        CancelButton.Click += (_, _) => Close();
        SaveButton.Click += (_, _) => Save();

        LoadSteamGames();
    }

    private GameEntryType SelectedType => Types[Math.Max(0, TypeCombo.SelectedIndex)].Type;

    private void UpdatePanels()
    {
        var type = SelectedType;
        NativePanel.Visibility = type == GameEntryType.Native ? Visibility.Visible : Visibility.Collapsed;
        SteamPanel.Visibility = type == GameEntryType.Steam ? Visibility.Visible : Visibility.Collapsed;
        EmulatorPanel.Visibility = type == GameEntryType.Emulator ? Visibility.Visible : Visibility.Collapsed;
        ScriptPanel.Visibility = type == GameEntryType.Script ? Visibility.Visible : Visibility.Collapsed;
        WorkingDirRow.Visibility = type is GameEntryType.Native or GameEntryType.Script ? Visibility.Visible : Visibility.Collapsed;
        RunAsAdminRow.Visibility = type == GameEntryType.Steam ? Visibility.Collapsed : Visibility.Visible; // Steam starts its games itself
    }

    private void LoadSteamGames()
    {
        // Scans a few folders and parses small text files - fast, but kept off the UI thread anyway.
        _ = Task.Run(() =>
        {
            var games = SteamLibraryScanner.ListInstalled();
            Dispatcher.BeginInvoke(() =>
            {
                foreach (var g in games) SteamGameCombo.Items.Add(g);
                var current = games.FirstOrDefault(g => g.AppId == _entry.SteamAppId);
                if (current is not null) SteamGameCombo.SelectedItem = current;
            });
        });
    }

    private void OnSteamGamePicked()
    {
        if (SteamGameCombo.SelectedItem is not SteamGame game) return;
        SteamAppIdBox.Text = game.AppId;
        if (string.IsNullOrWhiteSpace(NameBox.Text)) NameBox.Text = game.Name;
        // Steam's own library capsule as the default cover, unless one was already chosen.
        if (_coverSourcePath is null && game.CoverPath is not null)
        {
            _coverSourcePath = game.CoverPath;
            _coverCrop = null;
            _coverChanged = true;
            ShowCover();
        }
    }

    private void BrowseFile(System.Windows.Controls.TextBox target, string filter, bool fillName)
    {
        var dialog = new Microsoft.Win32.OpenFileDialog { Filter = filter };
        if (!string.IsNullOrWhiteSpace(target.Text) && File.Exists(target.Text))
            dialog.InitialDirectory = Path.GetDirectoryName(target.Text);
        if (dialog.ShowDialog(this) != true) return;
        target.Text = dialog.FileName;
        if (fillName && string.IsNullOrWhiteSpace(NameBox.Text))
            NameBox.Text = Path.GetFileNameWithoutExtension(dialog.FileName);
    }

    private void BrowseFolder(System.Windows.Controls.TextBox target)
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog();
        if (Directory.Exists(target.Text)) dialog.InitialDirectory = target.Text;
        if (dialog.ShowDialog(this) == true) target.Text = dialog.FolderName;
    }

    private void ChooseCover()
    {
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Filter = "Изображения (*.png;*.jpg;*.jpeg;*.bmp;*.gif)|*.png;*.jpg;*.jpeg;*.bmp;*.gif",
            Title = "Обложка игры (лучше вертикальная, 2:3)",
        };
        if (dialog.ShowDialog(this) != true) return;
        _coverSourcePath = dialog.FileName;
        _coverCrop = null; // a new image starts from the centre crop
        _coverChanged = true;
        ShowCover();
        // A picture that isn't already 2:3 almost always needs the frame placed - offer it right away.
        var probe = CoverLoader.Load(_coverSourcePath, decodeWidth: 0);
        if (probe is not null && Math.Abs((double)probe.PixelWidth / probe.PixelHeight - 2.0 / 3.0) > 0.02) CropCover();
    }

    private void CropCover()
    {
        var full = CoverLoader.Load(_coverSourcePath, decodeWidth: 0);
        if (full is null) return;
        var dialog = new CoverCropWindow(full, _coverCrop) { Owner = this };
        if (dialog.ShowDialog() != true || dialog.Result is null) return;
        _coverCrop = dialog.Result;
        ShowCover();
    }

    private void ShowCover()
    {
        // Exactly what the Switch tile will show (crop or centre crop), not the raw picture.
        var image = CoverLoader.LoadAsTile(_coverSourcePath, _coverCrop);
        CoverImage.Source = image;
        NoCoverText.Visibility = image is null ? Visibility.Visible : Visibility.Collapsed;
        CropCoverButton.IsEnabled = image is not null;
        ClearCoverButton.IsEnabled = image is not null;
    }

    private void Save()
    {
        var type = SelectedType;
        var problem = type switch
        {
            GameEntryType.Native when !File.Exists(ExePathBox.Text.Trim()) => "Укажите существующий файл игры (.exe).",
            GameEntryType.Steam when !SteamAppIdBox.Text.Trim().All(char.IsDigit) || SteamAppIdBox.Text.Trim().Length == 0 => "Выберите игру Steam или укажите числовой AppID.",
            GameEntryType.Emulator when !File.Exists(EmulatorPathBox.Text.Trim()) => "Укажите существующий файл эмулятора (.exe).",
            GameEntryType.Emulator when RomPathBox.Text.Trim().Length == 0 => "Укажите файл игры для эмулятора.",
            GameEntryType.Script when string.IsNullOrWhiteSpace(ScriptBox.Text) => "Скрипт запуска пуст.",
            _ => null,
        };
        if (problem is null && string.IsNullOrWhiteSpace(NameBox.Text)) problem = "Укажите название.";
        if (problem is not null)
        {
            System.Windows.MessageBox.Show(this, problem, "SolarNX", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        _entry.Name = NameBox.Text.Trim();
        _entry.Type = type;
        _entry.ExePath = ExePathBox.Text.Trim();
        _entry.Arguments = ArgumentsBox.Text.Trim();
        _entry.WorkingDirectory = WorkingDirBox.Text.Trim();
        _entry.SteamAppId = SteamAppIdBox.Text.Trim();
        _entry.EmulatorPath = EmulatorPathBox.Text.Trim();
        _entry.RomPath = RomPathBox.Text.Trim();
        _entry.ArgumentTemplate = ArgumentTemplateBox.Text.Trim();
        _entry.Script = ScriptBox.Text;
        _entry.Controller = Controllers[Math.Max(0, ControllerCombo.SelectedIndex)].Mode;
        _entry.CaptureGameWindow = CaptureWindowCheck.IsChecked == true;
        _entry.RunAsAdmin = RunAsAdminCheck.IsChecked == true;
        _entry.CoverCrop = _coverSourcePath is null ? null : _coverCrop?.Clone();

        try
        {
            if (_entry.Id == 0) _library.Upsert(_entry); // assigns the id the cover file is named after

            if (_coverChanged)
            {
                var oldCover = GameLibrary.CoverPath(_entry);
                _entry.CoverFile = _coverSourcePath is null ? "" : GameLibrary.ImportCover(_entry.Id, _coverSourcePath);
                if (oldCover is not null && oldCover != GameLibrary.CoverPath(_entry))
                {
                    try { File.Delete(oldCover); } catch { /* best-effort */ }
                }
            }

            _library.Upsert(_entry);
            Close();
        }
        catch (Exception ex)
        {
            Logger.Log("GameEditWindow", $"saving '{_entry.Name}' failed: {ex}");
            System.Windows.MessageBox.Show(this, $"Не удалось сохранить: {ex.Message}", "SolarNX", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }
}
