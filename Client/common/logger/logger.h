#pragma once

#include <stdbool.h>

// Brings up nxlink stdio redirection so logWrite() output streams live to the PC that launched
// this .nro via `nxlink -s` through hbmenu's netloader (see the Makefile's DEBUG=1 / `make debug`
// variant). Requires the BSD socket service to already be initialized - call this after
// networkInit() (network/network.h). In a non-DEBUG build this is a no-op that always returns
// true; logWrite() still works, it just has nowhere to send its output.
bool logInit(void);

// printf-style, category-tagged log line - deliberately mirrors the PC host's
// Logger.Log(category, message) (SolarNX/Logger.cs) so logs from both sides read the same way
// side by side. Safe to call from any thread (internally mutex-guarded).
__attribute__((format(printf, 2, 3)))
void logWrite(const char *category, const char *fmt, ...);

// Closes the nxlink socket opened by logInit(), if any. Call once at shutdown, before
// networkExit() tears down the socket service.
void logExit(void);

// Routes FFmpeg's own internal logging (av_log) through logWrite()'s mutex instead of FFmpeg's
// own independent, unrelated logging lock. Call once from any thread that's about to use
// libavformat/libavcodec (audio.c and video.c both do) - idempotent, safe to call from both.
//
// Why this matters: nxlinkStdio() (logInit(), DEBUG builds) redirects both stdout and stderr to
// the same socket. Without this, av_log's default handler writes to stderr under FFmpeg's own
// lock while logWrite() writes to stdout under s_logMutex - two independently-locked writers to
// the same underlying fd from different threads, which is exactly the kind of unsynchronized
// concurrent access that caused a live, reproducible crash (consistently ~2s into HID+audio
// running concurrently, unaffected by unrelated changes like bsd session count or poll interval -
// both ruled out as the cause by not changing the crash timing at all).
void logInstallFfmpegCallback(void);
