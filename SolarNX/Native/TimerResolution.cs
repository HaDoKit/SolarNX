using System.Runtime.InteropServices;

namespace SolarNX.Native;

/// <summary>
/// Windows' default scheduler tick is ~15.6ms - during a busy game/GPU-driver/video-encoder moment
/// that's long enough for AudioCaptureSender's capture thread to occasionally miss its turn and
/// produce an audible glitch. Requesting 1ms scheduler granularity for the duration of a session
/// tightens how promptly *every* thread on the system -
/// including ffmpeg's, which this app doesn't control the internals of - gets scheduled again after
/// being preempted. This is the standard, low-risk mitigation for this class of problem (games and
/// pro-audio software both do it routinely); unlike raising a process to Realtime priority, it can't
/// itself starve other processes.
/// </summary>
public static class TimerResolution
{
    private static bool _active;

    public static void RequestHighResolution()
    {
        if (_active) return;
        var result = timeBeginPeriod(1);
        if (result != 0)
        {
            Logger.Log("TimerResolution", $"timeBeginPeriod(1) failed, result={result}");
            return;
        }
        _active = true;
    }

    public static void Release()
    {
        if (!_active) return;
        timeEndPeriod(1);
        _active = false;
    }

    [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod")]
    private static extern uint timeBeginPeriod(uint uPeriod);

    [DllImport("winmm.dll", EntryPoint = "timeEndPeriod")]
    private static extern uint timeEndPeriod(uint uPeriod);
}
