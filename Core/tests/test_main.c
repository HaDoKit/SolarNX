// Unit tests for libsolarnx-core. Tiny harness, no external framework.
// This file is part of SolarNX (GPL-3.0).

#include <stdio.h>
#include <stdlib.h>

int test_wire(void);
int test_route(void);
int test_video(void);
int test_audio(void);
int test_library(void);

static int failures = 0;

void tcheck(int ok, const char *what)
{
    if (!ok)
    {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

int main(void)
{
    failures = 0;
    test_wire();
    test_route();
    test_video();
    test_audio();
    test_library();
    if (failures > 0)
    {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
