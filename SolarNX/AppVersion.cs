using System.Reflection;

namespace SolarNX;

/// <summary>The app version from ../VERSION (stamped into the assembly by SolarNX.csproj).</summary>
internal static class AppVersion
{
    // InformationalVersion is "0.1.0-alpha+<commit sha>" - the SDK appends the source revision.
    public static string Current { get; } =
        (typeof(AppVersion).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0")
        .Split('+')[0];
}
