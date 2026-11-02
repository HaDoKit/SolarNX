using System.Diagnostics;
using System.Management;
using System.Text.Json.Serialization;

namespace SolarNX.Capture;

/// <summary>H.264 encoder used for the video stream. Auto picks the best one that actually works
/// on this machine (see <see cref="EncoderSelection.Resolve"/>).</summary>
[JsonConverter(typeof(JsonStringEnumConverter))]
public enum EncoderKind
{
    Auto,
    Nvenc,
    Amf,
    Qsv,
    MediaFoundation,
    Software,
}

public enum GpuVendor
{
    Nvidia,
    Amd,
    Intel,
    Other,
}

public sealed record GpuInfo(string Name, GpuVendor Vendor);

/// <summary>
/// Detects the GPUs and which hardware H.264 encoders of the bundled ffmpeg really work here.
///
/// Availability is decided by a real probe - encoding one throwaway frame with the encoder - not by
/// the GPU list: an encoder can be missing even on its own vendor's card (too old a driver: this
/// ffmpeg build's NVENC needs NVIDIA driver 610+, "Driver does not support the required nvenc API
/// version"; AMF needs amfrt64.dll from the AMD driver; QSV needs a supported Intel GPU), and
/// Media Foundation works on any GPU whose driver ships an H.264 encoder MFT. Probes are cached for
/// the process lifetime - the answer can't change without a driver update, which needs a restart.
/// </summary>
internal static class EncoderSelection
{
    private static readonly object Lock = new();
    private static IReadOnlyList<GpuInfo>? _gpus;
    private static readonly Dictionary<EncoderKind, bool> Available = new() { [EncoderKind.Software] = true };

    public static string DisplayName(EncoderKind kind) => kind switch
    {
        EncoderKind.Auto => "Авто",
        EncoderKind.Nvenc => "NVIDIA NVENC",
        // Not tested on real AMD/Intel hardware yet (the dev machine is NVIDIA-only) - labelled
        // experimental until a working stream is confirmed there.
        EncoderKind.Amf => "AMD AMF (экспериментально)",
        EncoderKind.Qsv => "Intel Quick Sync (экспериментально)",
        EncoderKind.MediaFoundation => "Media Foundation (любая видеокарта)",
        EncoderKind.Software => "Процессор (x264)",
        _ => kind.ToString(),
    };

    /// <summary>Physical display adapters (virtual ones - remote desktop, virtual monitors - are
    /// left out), vendor from the PCI vendor id in PNPDeviceID.</summary>
    public static IReadOnlyList<GpuInfo> Gpus
    {
        get
        {
            lock (Lock)
            {
                return _gpus ??= DetectGpus();
            }
        }
    }

    private static IReadOnlyList<GpuInfo> DetectGpus()
    {
        var result = new List<GpuInfo>();
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Name, PNPDeviceID FROM Win32_VideoController");
            foreach (var item in searcher.Get())
            {
                using (item)
                {
                    var name = item["Name"] as string ?? "";
                    var pnp = (item["PNPDeviceID"] as string ?? "").ToUpperInvariant();
                    if (!pnp.StartsWith("PCI\\", StringComparison.Ordinal)) continue; // virtual adapters
                    var vendor = pnp.Contains("VEN_10DE", StringComparison.Ordinal) ? GpuVendor.Nvidia
                        : pnp.Contains("VEN_1002", StringComparison.Ordinal) || pnp.Contains("VEN_1022", StringComparison.Ordinal) ? GpuVendor.Amd
                        : pnp.Contains("VEN_8086", StringComparison.Ordinal) ? GpuVendor.Intel
                        : GpuVendor.Other;
                    result.Add(new GpuInfo(name.Trim(), vendor));
                }
            }
        }
        catch (Exception ex)
        {
            Logger.Log("encoder", $"GPU detection failed: {ex.Message}");
        }
        Logger.Log("encoder", result.Count == 0
            ? "no physical GPUs detected"
            : "GPUs: " + string.Join("; ", result.Select(g => $"{g.Name} ({g.Vendor})")));
        return result;
    }

    /// <summary>Whether the encoder can actually open here (probes once, then cached). Auto is never
    /// "available" itself - resolve it first.</summary>
    public static bool IsAvailable(EncoderKind kind)
    {
        if (kind == EncoderKind.Auto) return false;
        lock (Lock)
        {
            if (Available.TryGetValue(kind, out var known)) return known;
            var ok = Probe(kind);
            Available[kind] = ok;
            Logger.Log("encoder", $"probe {DisplayName(kind)}: {(ok ? "available" : "not available")}");
            return ok;
        }
    }

    /// <summary>The encoder to use for <paramref name="preference"/>. A specific choice is used when it
    /// works, otherwise this falls back to Auto. Auto: each detected GPU's own encoder - discrete
    /// vendors first (NVIDIA, AMD, then Intel), so a laptop's dedicated card wins over its integrated
    /// one - then Media Foundation (any GPU), and the CPU only when nothing else works.</summary>
    public static EncoderKind Resolve(EncoderKind preference)
    {
        if (preference != EncoderKind.Auto)
        {
            if (IsAvailable(preference)) return preference;
            Logger.Log("encoder", $"{DisplayName(preference)} is not available - falling back to Auto");
        }

        var candidates = Gpus
            .Select(g => g.Vendor)
            .Distinct()
            .OrderBy(v => v)
            .Select(v => v switch
            {
                GpuVendor.Nvidia => EncoderKind.Nvenc,
                GpuVendor.Amd => EncoderKind.Amf,
                GpuVendor.Intel => EncoderKind.Qsv,
                _ => EncoderKind.MediaFoundation,
            })
            .Append(EncoderKind.MediaFoundation)
            .Distinct();
        foreach (var kind in candidates)
        {
            if (IsAvailable(kind)) return kind;
        }
        return EncoderKind.Software;
    }

    /// <summary>Pixel format the encoder is fed in - the last step of the capture filter chain.
    /// QSV and Media Foundation take NV12 only; the others take yuv420p directly.</summary>
    public static string PixelFormat(EncoderKind kind) =>
        kind is EncoderKind.Qsv or EncoderKind.MediaFoundation ? "nv12" : "yuv420p";

    /// <summary>ffmpeg encoder arguments, all tuned the same way: constant bitrate (the bitrate bounds
    /// themselves are added by VideoEncoder), High profile, no B-frames and no lookahead, so nothing
    /// holds frames back, and a keyframe every <paramref name="keyframeInterval"/> frames.</summary>
    public static string EncodeArgs(EncoderKind kind, int keyframeInterval) => kind switch
    {
        // -delay 0 -rc-lookahead 0: NVENC still buffers a few frames in a low-latency preset
        // without them. "p4" keeps real motion-search effort while staying well within real time.
        EncoderKind.Nvenc =>
            $"-c:v h264_nvenc -gpu 0 -preset p4 -tune ll -rc cbr -zerolatency 1 -delay 0 -rc-lookahead 0 -bf 0 -profile:v high -g {keyframeInterval}",
        // header_spacing = the keyframe interval: SPS/PPS with every keyframe, so the Switch can start
        // decoding mid-stream (MPEG-TS has no out-of-band headers). async_depth 1: AMF otherwise keeps
        // up to 16 frames in flight.
        EncoderKind.Amf =>
            $"-c:v h264_amf -usage ultralowlatency -quality speed -rc cbr -latency 1 -async_depth 1 -preencode 0 -header_spacing {keyframeInterval} -bf 0 -profile:v high -g {keyframeInterval}",
        EncoderKind.Qsv =>
            $"-c:v h264_qsv -preset veryfast -look_ahead 0 -async_depth 1 -repeat_pps 1 -bf 0 -profile:v high -g {keyframeInterval}",
        // hw_encoding 1: only a hardware MFT - never Microsoft's software one, which is slower than x264.
        // h264_mf takes the profile only as a number: 100 = High.
        EncoderKind.MediaFoundation =>
            $"-c:v h264_mf -hw_encoding 1 -scenario display_remoting -rate_control cbr -bf 0 -profile:v 100 -g {keyframeInterval}",
        _ =>
            $"-c:v libx264 -preset faster -tune zerolatency -profile:v high -crf 18 -g {keyframeInterval}",
    };

    /// <summary>Encodes one throwaway 256x256 frame with the encoder's real arguments. Not smaller:
    /// NVENC rejects 64x64 ("Frame Dimension less than the minimum supported value"), which would be
    /// a false negative.</summary>
    private static bool Probe(EncoderKind kind)
    {
        var ffmpegPath = Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
        try
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo(ffmpegPath,
                    $"-hide_banner -f lavfi -i color=black:s=256x256:d=0.1 -frames:v 1 -pix_fmt {PixelFormat(kind)} " +
                    $"{EncodeArgs(kind, 30)} -b:v 2000k -f null -")
                {
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                },
            };
            process.Start();
            var stderr = process.StandardError.ReadToEnd();
            if (!process.WaitForExit(10000))
            {
                process.Kill();
                return false;
            }
            if (process.ExitCode != 0)
            {
                var lastLine = stderr.Split('\n', StringSplitOptions.RemoveEmptyEntries).LastOrDefault()?.Trim();
                Logger.Log("encoder", $"{DisplayName(kind)} probe failed: {lastLine}");
            }
            return process.ExitCode == 0;
        }
        catch (Exception ex)
        {
            Logger.Log("encoder", $"{DisplayName(kind)} probe could not run: {ex.Message}");
            return false;
        }
    }
}
