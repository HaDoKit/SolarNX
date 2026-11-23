// Semantic input: the platform collects gamepads/touch/motion and hands the state to
// the core; the active backend serializes it into its own wire format (SolarNX HID
// packet, or Moonlight's controller events once the Sunshine backend is wired).
//
// This file is part of SolarNX (GPL-3.0).
#pragma once

#include "wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t buttons;   // SolarNX bit layout - host reads the low 16 bits
    int32_t  lx, ly;    // stick, -0x8000..0x7FFF
    int32_t  rx, ry;
    float    accelX, accelY, accelZ; // g
    float    gyroX, gyroY, gyroZ;    // rad/s
} SnxInputPad;

typedef struct {
    SnxInputPad pads[4];
    uint32_t    padCount;          // 0..4
    uint32_t    touchX1, touchY1;  // primary touch point, pixels
    uint32_t    libraryEntryId;    // SNX_APP_LIBRARY target, else 0
    int         appTarget;         // SnxAppTarget
    uint32_t    displayHeight;     // client screen height (encoder resolution hint)
} SnxInputState;

// Serializes the semantic state into the SolarNX HID packet. Telemetry counters are
// left for the caller to bump (they are free-running; the host reads deltas).
void snx_input_to_hid(const SnxInputState *in, SnxHidPacket *out);

#ifdef __cplusplus
}
#endif
