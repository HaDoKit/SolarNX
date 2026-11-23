// wire.h contracts and the semantic-input serializer.
// This file is part of SolarNX (GPL-3.0).

#include <stdint.h>
#include <string.h>

#include "solarnx/input.h"
#include "solarnx/wire.h"

#include "test_harness.h"

int test_wire(void)
{
    // Sizes are frozen by the host-side parsers - a change breaks the stream.
    tcheck(sizeof(SnxControllerState) == 44, "controller state is 44 bytes");
    tcheck(sizeof(SnxHidPacket) == SNX_HID_PAYLOAD_SIZE, "hid packet is 224 bytes");
    tcheck(sizeof(SnxSessionConfig) == SNX_SESSION_CONFIG_SIZE, "session config is 28 bytes");
    tcheck(SNX_VERSION_WIRE_SIZE == 28, "version wire block is 28 bytes");

    // The serializer fills the packet field-for-field from the semantic state.
    SnxInputState in;
    memset(&in, 0, sizeof(in));
    in.padCount = 2;
    in.pads[1].buttons = 0xA5A5;
    in.pads[1].lx = -1000;
    in.pads[1].ly = 2000;
    in.pads[1].accelX = 1.5f;
    in.pads[1].gyroZ = -0.25f;
    in.touchX1 = 400;
    in.touchY1 = 240;
    in.libraryEntryId = 77;
    in.appTarget = SNX_APP_LIBRARY;
    in.displayHeight = 1080;

    SnxHidPacket pkt;
    snx_input_to_hid(&in, &pkt);

    tcheck(pkt.controllers[1].heldKeys == 0xA5A5, "buttons land in the right slot");
    tcheck(pkt.controllers[1].lJoyX == -1000 && pkt.controllers[1].lJoyY == 2000, "sticks");
    tcheck(pkt.controllers[1].accelX == 1.5f && pkt.controllers[1].gyroZ == -0.25f, "motion");
    tcheck(pkt.touchX1 == 400 && pkt.touchY1 == 240, "touch");
    tcheck(pkt.libraryEntryId == 77, "library entry id");
    tcheck(pkt.appTarget == SNX_APP_LIBRARY, "app target");
    tcheck(pkt.displayHeight == 1080, "display height");
    tcheck(pkt.controllerCount == 2, "controller count");
    tcheck(pkt.controllers[3].heldKeys == 0, "unused pad zeroed");

    // NULL inputs don't explode.
    SnxHidPacket zero;
    memset(&zero, 0xAB, sizeof(zero));
    snx_input_to_hid(NULL, &zero);
    snx_input_to_hid(&in, NULL);
    return 0;
}
