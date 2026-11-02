using System.Text.Json;
using System.Text.Json.Serialization;

namespace SolarNX;

public sealed class Config
{
    public string ClientIp { get; set; } = "";
    public int VideoPort { get; set; } = 19000;
    public int HidPort { get; set; } = 19001;
    public int AudioPort { get; set; } = 19002;

    /// <summary>shadPS4 game-library picker channel (separate TCP port from HidPort - that one is
    /// fixed-size/fixed-rate/one-way, no room for a variable-length game list + icons). Next free
    /// port after 19000/19001/19002. Keep numerically identical to SOLARNX_LIBRARY_PORT in
    /// Client/network/network.h (no shared build).</summary>
    public int LibraryPort { get; set; } = 19003;
    public int TargetWidth { get; set; } = 1280;
    public int TargetHeight { get; set; } = 720;
    public int Fps { get; set; } = 60;

    /// <summary>Confirmed live: at 8000-10000 the video stream's constant (CBR) bitrate was
    /// saturating the real Wi-Fi link's actual sustainable throughput - not just a quality setting,
    /// this caused severe audio packet loss (a client-side heartbeat log showed one 54-second stretch
    /// with over 96% of expected RTP/Opus packets never arriving at all) and video's own H.264
    /// keyframes coming through corrupted (decoded frames with 0x0 dimensions, forcing that session
    /// to abort and restart) - both symptoms of a genuinely oversaturated channel, not an
    /// encoder-quality problem (raising the bitrate previously made no visible improvement to picture
    /// quality precisely because the bottleneck was already the network, not the encoder). 4000
    /// leaves real headroom for audio (~130kbps) and HID to coexist on the same link instead of
    /// being crowded out.</summary>
    public int BitrateKbps { get; set; } = 4000;

    /// <summary>When true, BitrateKbps above is treated as a ceiling rather than a fixed value -
    /// AdaptiveBitrateController scales the running video encoder's actual bitrate down (and
    /// cautiously back up) between a fixed floor and this ceiling based on link-quality telemetry
    /// the client reports every HidPacket (reconnects, concealed audio frames - see
    /// Input/HidProtocol.cs). MainForm disables the bitrate control while this is on, since the
    /// manual value no longer reflects what's actually being sent. See
    /// Session/AdaptiveBitrateController.cs - applies by restarting VideoEncoder's ffmpeg process
    /// with new args, since it has no live reconfigure of its own.</summary>
    public bool AdaptiveBitrate { get; set; } = false;

    /// <summary>Sample rate the PCM is captured/resampled to before Opus encoding (see
    /// AudioCaptureSender). RFC 7587 mandates Opus's RTP clock rate be 48000Hz regardless of this
    /// value, and the client (SolarNX/source/audio.c) decodes via libavcodec/libswresample rather than
    /// assuming any particular rate - but 48000 is still the practical choice since it matches the
    /// Switch's native audout rate and needs no client-side resampling on top of Opus's own.</summary>
    public int AudioSampleRate { get; set; } = 48000;

    /// <summary>1 (mono) or 2 (stereo) - halving to mono at the same sample rate roughly halves the
    /// Opus-encoded bitrate, an alternative lever to lowering AudioSampleRate when the client can't
    /// keep up.</summary>
    public int AudioChannels { get; set; } = 2;

    /// <summary>Linear gain applied to the audio signal before it's sent (1.0 = unchanged, 0.5 =
    /// half amplitude/-6dB). Distinct from AudioCaptureSender's peak limiter - that only clips the
    /// very tips of the loudest peaks, this turns the whole signal down. The client's decode/playback
    /// is a black box that can't be patched, so a lower overall level is the only lever available
    /// for amplitude-dependent client-side audio artifacts.</summary>
    public double AudioGain { get; set; } = 1.0;

    /// <summary>When true (default - matches the client's physical Nintendo Switch controller
    /// layout), A/B and X/Y are swapped relative to the wire protocol's bit positions before being
    /// applied to the virtual Xbox 360 controller - see VirtualGamepadHub.</summary>
    public bool SwitchButtonLayout { get; set; } = true;

    /// <summary>Off by default: after a game exits, Steam returns to Big Picture by itself. When on,
    /// AppOrchestrator additionally sends Escape and fully closes/reopens Big Picture - an old
    /// workaround for the gamepad staying dead in Big Picture after a non-Steam-game shortcut (e.g.
    /// "Luna Abyss") closes, see AppOrchestrator.RestartBigPictureAfterGameClosed. Turn this on
    /// (config.json only, no UI) only if that problem comes back.</summary>
    public bool RestartBigPictureAfterGame { get; set; } = false;

    /// <summary>Diagnostic-only: when true, AudioCaptureSender writes to a local "audio_debug.wav" file
    /// next to the exe instead of streaming to the client over UDP - isolates the host's own
    /// capture/encode chain from the network and client's decode, to tell whether an audible
    /// artifact is introduced before or after the audio leaves this machine. Deliberately not
    /// persisted (JsonIgnore) - always resets to off on the next launch, so it can't be
    /// accidentally left on and mistaken for a broken stream in a later, unrelated session.</summary>
    [JsonIgnore]
    public bool AudioDebugRecordToFile { get; set; }

    /// <summary>Absolute path to shadPS4's portable exe. shadPS4 has no installer and no registry
    /// entries (see ShadPs4Launcher) - unlike Steam there's no auto-detect, must be set manually via
    /// MainForm's browse button. Empty means "not configured."</summary>
    public string ShadPs4ExePath { get; set; } = "";

    private static string ConfigPath =>
        Path.Combine(AppContext.BaseDirectory, "config.json");

    public static Config Load()
    {
        try
        {
            if (File.Exists(ConfigPath))
            {
                var json = File.ReadAllText(ConfigPath);
                var cfg = JsonSerializer.Deserialize<Config>(json);
                if (cfg is not null)
                {
                    cfg.MigrateLegacyPorts();
                    return cfg;
                }
            }
        }
        catch
        {
            // fall through to defaults on corrupt/unreadable config
        }
        return new Config();
    }

    /// <summary>Ports moved from 2222-2225 to 19000-19003. A config.json saved before that still
    /// holds the old defaults, which the new Switch client no longer listens on - map exactly those
    /// old defaults to the new ones, but leave any port a user set by hand alone.</summary>
    private void MigrateLegacyPorts()
    {
        if (VideoPort == 2222) VideoPort = 19000;
        if (HidPort == 2223) HidPort = 19001;
        if (AudioPort == 2224) AudioPort = 19002;
        if (LibraryPort == 2225) LibraryPort = 19003;
    }

    public void Save()
    {
        var json = JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(ConfigPath, json);
    }

    public bool IsValid() => !string.IsNullOrWhiteSpace(ClientIp);
}
