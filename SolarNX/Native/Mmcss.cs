using System.Runtime.InteropServices;

namespace SolarNX.Native;

/// <summary>
/// Registers the calling thread with Windows' Multimedia Class Scheduler Service under the
/// "Pro Audio" task category - the same mechanism real-time audio software (DAWs, ASIO drivers,
/// etc.) uses to get consistently prompt scheduling regardless of what else the system is doing at
/// the time. Far more targeted than raising a whole process to a higher priority class: only the
/// thread that calls Register gets the boost, and MMCSS itself (not this app) decides moment to
/// moment how much of one is safe to give, which is why this can't destabilize the rest of the
/// system the way a RealTime-priority process theoretically could.
/// </summary>
public static class Mmcss
{
    /// <summary>Call from the thread that needs the boost - MMCSS characteristics are per-thread,
    /// not inheritable/transferable. Returns a handle to pass to <see cref="Revert"/>, or
    /// IntPtr.Zero if registration failed (logged; the thread just runs at its normal priority).</summary>
    public static IntPtr Register(string taskName)
    {
        var handle = AvSetMmThreadCharacteristics(taskName, out _);
        if (handle == IntPtr.Zero)
        {
            Logger.Log("Mmcss", $"AvSetMmThreadCharacteristics(\"{taskName}\") failed, err={Marshal.GetLastWin32Error()}");
        }
        return handle;
    }

    public static void Revert(IntPtr handle)
    {
        if (handle == IntPtr.Zero) return;
        AvRevertMmThreadCharacteristics(handle);
    }

    [DllImport("avrt.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr AvSetMmThreadCharacteristics(string taskName, out uint taskIndex);

    [DllImport("avrt.dll", SetLastError = true)]
    private static extern bool AvRevertMmThreadCharacteristics(IntPtr avrtHandle);
}
