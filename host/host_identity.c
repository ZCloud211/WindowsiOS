/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host_identity.h"
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

/* Public hardware identifiers: DeviceKit's Device.generated.swift and the
 * Apple product specifications linked there. Match exact model ranges; a
 * future iPad in the same identifier family must not inherit an older chip.
 * https://github.com/devicekit/DeviceKit/blob/master/Source/Device.generated.swift
 */
static const struct {
    unsigned family, first, last;
    const char *chip, *product;
} ipads[] = {
    {8, 1, 4, "Apple A12X Bionic", "iPad Pro 11-inch (2018)"},
    {8, 5, 8, "Apple A12X Bionic", "iPad Pro 12.9-inch (2018)"},
    {8, 9, 10, "Apple A12Z Bionic", "iPad Pro 11-inch (2020)"},
    {8, 11, 12, "Apple A12Z Bionic", "iPad Pro 12.9-inch (2020)"},
    {11, 1, 2, "Apple A12 Bionic", "iPad mini (5th generation)"},
    {11, 3, 4, "Apple A12 Bionic", "iPad Air (3rd generation)"},
    {11, 6, 7, "Apple A12 Bionic", "iPad (8th generation)"},
    {12, 1, 2, "Apple A13 Bionic", "iPad (9th generation)"},
    {13, 1, 2, "Apple A14 Bionic", "iPad Air (4th generation)"},
    {13, 4, 7, "Apple M1", "iPad Pro 11-inch (2021)"},
    {13, 8, 11, "Apple M1", "iPad Pro 12.9-inch (2021)"},
    {13, 16, 17, "Apple M1", "iPad Air (5th generation)"},
    {13, 18, 19, "Apple A14 Bionic", "iPad (10th generation)"},
    {14, 1, 2, "Apple A15 Bionic", "iPad mini (6th generation)"},
    {14, 3, 4, "Apple M2", "iPad Pro 11-inch (2022)"},
    {14, 5, 6, "Apple M2", "iPad Pro 12.9-inch (2022)"},
    {14, 8, 9, "Apple M2", "iPad Air 11-inch (M2)"},
    {14, 10, 11, "Apple M2", "iPad Air 13-inch (M2)"},
    {15, 3, 4, "Apple M3", "iPad Air 11-inch (M3)"},
    {15, 5, 6, "Apple M3", "iPad Air 13-inch (M3)"},
    {15, 7, 8, "Apple A16", "iPad (A16)"},
    {16, 1, 2, "Apple A17 Pro", "iPad mini (A17 Pro)"},
    {16, 3, 4, "Apple M4", "iPad Pro 11-inch (M4)"},
    {16, 5, 6, "Apple M4", "iPad Pro 13-inch (M4)"},
    {16, 8, 9, "Apple M4", "iPad Air 11-inch (M4)"},
    {16, 10, 11, "Apple M4", "iPad Air 13-inch (M4)"},
    {17, 1, 2, "Apple M5", "iPad Pro 11-inch (M5)"},
    {17, 3, 4, "Apple M5", "iPad Pro 13-inch (M5)"},
};

int hv_host_identity_for_machine(const char *machine, HvHostIdentity *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->machine, sizeof out->machine, "%s", machine && *machine ? machine : "unknown");
    snprintf(out->product, sizeof out->product, "WindowsiOS ARM64 (%s)", out->machine);
    snprintf(out->chip, sizeof out->chip, "Apple Silicon (model unknown)");
    unsigned family, model;
    char trailing;
    if (sscanf(out->machine, "iPad%u,%u%c", &family, &model, &trailing) != 2) return 0;
    for (size_t n = 0; n < sizeof ipads / sizeof *ipads; n++) {
        if (family != ipads[n].family || model < ipads[n].first || model > ipads[n].last) continue;
        snprintf(out->chip, sizeof out->chip, "%s", ipads[n].chip);
        snprintf(out->product, sizeof out->product, "%s", ipads[n].product);
        return 1;
    }
    return 0;
}

void hv_host_identity_detect(HvHostIdentity *out) {
    char machine[64] = {0};
    size_t length = sizeof machine - 1;
    if (sysctlbyname("hw.machine", machine, &length, NULL, 0)) machine[0] = 0;
    if (hv_host_identity_for_machine(machine, out)) return;
    /* macOS exposes the real chip brand, whereas iPadOS usually restricts it. */
    char brand[64] = {0};
    length = sizeof brand - 1;
    if (!sysctlbyname("machdep.cpu.brand_string", brand, &length, NULL, 0) &&
        !strncmp(brand, "Apple ", 6)) {
        snprintf(out->chip, sizeof out->chip, "%s", brand);
    }
}
