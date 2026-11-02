using System.Globalization;
using System.Text.Json.Serialization;
using System.Windows;

namespace SolarNX;

[JsonConverter(typeof(JsonStringEnumConverter))]
public enum UiLanguage
{
    /// <summary>Russian if Windows' display language is Russian, English otherwise.</summary>
    Auto,
    Russian,
    English,
}

/// <summary>
/// Interface language. The strings live in UI/Strings/Strings.{ru,en}.xaml (same keys in both);
/// the active one is merged into the application resources, so XAML that uses
/// {DynamicResource Key} follows a switch at once. Text built in code reads Loc.T/Loc.F and is
/// rebuilt on <see cref="Changed"/>.
/// </summary>
internal static class Loc
{
    private static ResourceDictionary? _strings;

    // The user's display language, taken before Apply replaces CurrentUICulture with the chosen one.
    private static readonly string WindowsLanguage = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;

    /// <summary>"ru" or "en".</summary>
    public static string Code { get; private set; } = "ru";

    /// <summary>Raised on the UI thread after the language changed.</summary>
    public static event Action? Changed;

    public static void Apply(UiLanguage language)
    {
        var code = language switch
        {
            UiLanguage.Russian => "ru",
            UiLanguage.English => "en",
            _ => WindowsLanguage == "ru" ? "ru" : "en",
        };
        var strings = new ResourceDictionary { Source = new Uri($"pack://application:,,,/UI/Strings/Strings.{code}.xaml") };

        var merged = System.Windows.Application.Current?.Resources.MergedDictionaries;
        if (merged is not null)
        {
            if (_strings is not null) merged.Remove(_strings);
            merged.Add(strings);
        }
        _strings = strings;
        var changed = code != Code;
        Code = code;
        CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.CurrentUICulture = new CultureInfo(code);
        if (changed) Changed?.Invoke();
    }

    public static string T(string key) => _strings?[key] as string ?? key;

    public static string F(string key, params object?[] args) => string.Format(T(key), args);
}
