using System.Diagnostics;
using System.Runtime.InteropServices;

namespace SolarNX.Native;

/// <summary>
/// Ties long-running child processes (ffmpeg.exe, touchhelper.exe) to this process's lifetime via a
/// Windows Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: the job handle is held for as long
/// as SolarNX runs and never closed explicitly, so the OS closes it - and kills every process in
/// the job - whenever this process ends, however it ends.
///
/// Confirmed live: VideoEncoder.Stop() only runs on an orderly shutdown. Stopping the debugger,
/// a crash or a kill left ffmpeg.exe orphaned and still streaming video to the Switch indefinitely
/// (found one running for well over an hour after its parent died) - audio and HID stopped as
/// expected because those live inside this process, which made it look like a client-side bug.
/// </summary>
internal static class ChildProcessJob
{
    private const int JobObjectExtendedLimitInformation = 9;
    private const uint JobObjectLimitKillOnJobClose = 0x00002000;

    private static readonly Lazy<IntPtr> Job = new(CreateKillOnCloseJob);

    /// <summary>Best-effort: a failure is logged and the process just runs unmanaged, exactly as
    /// before this existed. Call right after Process.Start().</summary>
    public static void Assign(Process process)
    {
        try
        {
            var job = Job.Value;
            if (job == IntPtr.Zero) return;
            if (!AssignProcessToJobObject(job, process.Handle))
                Logger.Log("ChildProcessJob", $"AssignProcessToJobObject failed for pid={process.Id}, err={Marshal.GetLastWin32Error()}");
        }
        catch (Exception ex)
        {
            // Process already exited between Start() and here, etc.
            Logger.Log("ChildProcessJob", $"could not assign pid to job: {ex.Message}");
        }
    }

    private static IntPtr CreateKillOnCloseJob()
    {
        var job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero)
        {
            Logger.Log("ChildProcessJob", $"CreateJobObject failed, err={Marshal.GetLastWin32Error()}");
            return IntPtr.Zero;
        }

        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            BasicLimitInformation = new JOBOBJECT_BASIC_LIMIT_INFORMATION { LimitFlags = JobObjectLimitKillOnJobClose },
        };
        var size = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
        var ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, ptr, false);
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ptr, (uint)size))
            {
                Logger.Log("ChildProcessJob", $"SetInformationJobObject failed, err={Marshal.GetLastWin32Error()}");
                CloseHandle(job);
                return IntPtr.Zero;
            }
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }

        // Deliberately never closed - see the class doc comment.
        return job;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr hJob, int jobObjectInfoClass, IntPtr lpJobObjectInfo, uint cbJobObjectInfoLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr hObject);
}
