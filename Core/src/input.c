// Semantic input -> SolarNX HID packet serialization.
// This file is part of SolarNX (GPL-3.0).

#include "solarnx/input.h"

#include <string.h>

void snx_input_to_hid(const SnxInputState *in, SnxHidPacket *out)
{
    if (in == NULL || out == NULL)
        return;
    memset(out, 0, sizeof(*out));

    for (unsigned i = 0; i < 4; i++)
    {
        SnxControllerState *c = &out->controllers[i];
        const SnxInputPad *p = &in->pads[i];
        c->heldKeys = p->buttons;
        c->lJoyX = p->lx;
        c->lJoyY = p->ly;
        c->rJoyX = p->rx;
        c->rJoyY = p->ry;
        c->accelX = p->accelX;
        c->accelY = p->accelY;
        c->accelZ = p->accelZ;
        c->gyroX = p->gyroX;
        c->gyroY = p->gyroY;
        c->gyroZ = p->gyroZ;
    }

    out->touchX1 = in->touchX1;
    out->touchY1 = in->touchY1;
    out->libraryEntryId = in->libraryEntryId;
    out->controllerCount = in->padCount;
    out->appTarget = (uint32_t)in->appTarget;
    out->displayHeight = in->displayHeight;
}
