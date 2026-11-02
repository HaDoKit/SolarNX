// Small native (non-.NET) helper that owns the actual InjectTouchInput calls.
//
// Exists because calling InjectTouchInput directly via P/Invoke from the managed
// SolarNX.exe process consistently failed with ERROR_INVALID_PARAMETER (87), for every
// combination tried: struct field order re-verified byte-for-byte via offsetof() against this
// exact machine's real headers, array vs ref vs raw-AllocHGlobal-pointer marshaling, elevated vs
// non-elevated, MTA vs a dedicated STA thread, and a real visible window at the injection point
// (confirmed via a MouseDown handler that never fired - the touch genuinely never happened, this
// wasn't a misleading return value). A plain native C program built from Microsoft's own
// InjectTouchInput sample, using the identical coordinates/flags, succeeded immediately with the
// exact same struct - both elevated and not. Whatever the root cause is (something specific to
// how the .NET CLR hosts/calls into this particular user32 entry point on this system), it doesn't
// reproduce in native code, so the pragmatic fix is to let native code own the actual call and have
// the managed side just feed it coordinates over stdin, one line at a time.
//
// Protocol (stdin, one command per line, flushed with \n):
//   D x y     - touch down at pixel (x,y)
//   M x y     - touch move (update) to pixel (x,y) - only valid while a contact is down
//   U         - touch up (releases the currently held contact)
// No output is written back; failures are logged to touchhelper.log next to the exe.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

// This MinGW toolchain's headers may predate these (added in Windows 10 1703) - defining them
// manually if missing rather than depending on a fresh-enough windows.h.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

static FILE *g_log;

static void log_line(const char *fmt, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fflush(g_log);
}

static void inject(int x, int y, DWORD flags) {
    POINTER_TOUCH_INFO contact;
    memset(&contact, 0, sizeof(POINTER_TOUCH_INFO));
    contact.pointerInfo.pointerType = PT_TOUCH;
    contact.pointerInfo.pointerId = 0;
    contact.pointerInfo.ptPixelLocation.x = x;
    contact.pointerInfo.ptPixelLocation.y = y;
    contact.pointerInfo.pointerFlags = flags;
    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    contact.orientation = 90;
    contact.pressure = 32000;
    contact.rcContact.top = y - 2;
    contact.rcContact.bottom = y + 2;
    contact.rcContact.left = x - 2;
    contact.rcContact.right = x + 2;

    if (!InjectTouchInput(1, &contact)) {
        log_line("InjectTouchInput failed for flags=0x%lx x=%d y=%d err=%lu\n", flags, x, y, GetLastError());
    }
}

int main(void) {
    g_log = fopen("touchhelper.log", "a");

    // Without this, this process defaults to DPI-unaware, and Windows may reinterpret/rescale
    // the physical-pixel coordinates SolarNX.exe (which IS Per-Monitor V2 aware) sends
    // over stdin according to *this* process's own (unaware) DPI context before handing them to
    // InjectTouchInput - confirmed live a small constant leftward shift remained even after fixing
    // the caller's own DPI awareness. SetProcessDpiAwarenessContext makes this process interpret
    // (and pass through) coordinates as true physical pixels too, matching the caller.
    typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        SetProcessDpiAwarenessContext_t setCtx =
            (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_NONE)) {
        log_line("InitializeTouchInjection failed err=%lu\n", GetLastError());
        return 1;
    }

    char line[256];
    int lastX = 0, lastY = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char cmd;
        int x, y;
        if (line[0] == 'D' && sscanf(line + 1, "%d %d", &x, &y) == 2) {
            inject(x, y, POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
            lastX = x; lastY = y;
        } else if (line[0] == 'M' && sscanf(line + 1, "%d %d", &x, &y) == 2) {
            inject(x, y, POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
            lastX = x; lastY = y;
        } else if (line[0] == 'U') {
            inject(lastX, lastY, POINTER_FLAG_UP);
        }
    }

    if (g_log) fclose(g_log);
    return 0;
}
