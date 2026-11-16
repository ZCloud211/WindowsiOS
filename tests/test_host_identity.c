/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host_identity.h"
#include "acpi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *smbios_string(const uint8_t *record, const uint8_t *end, unsigned index) {
    const uint8_t *p = record + record[1];
    for (unsigned n = 1; p < end && *p; n++) {
        if (n == index) return (const char *)p;
        p += strlen((const char *)p) + 1;
    }
    return "";
}

static void check_smbios(const char *machine, const char *chip) {
    HvHostIdentity identity;
    hv_host_identity_for_machine(machine, &identity);
    assert(!strcmp(identity.chip, chip));
    HvMachine m = {0};
    HvFwCfg *fw = hv_fw_cfg_create(&m, 0x09020000, 4);
    assert(fw);
    hv_acpi_build_and_register(fw, 4, 3ULL << 30, 0, 0, &identity);
    const uint8_t *table = NULL, *anchor = NULL;
    size_t size = 0;
    for (int n = 0; n < fw->nfiles; n++) {
        if (!strcmp(fw->files[n].name, "etc/smbios/smbios-tables")) {
            table = fw->files[n].data;
            size = fw->files[n].len;
        }
        if (!strcmp(fw->files[n].name, "etc/smbios/smbios-anchor")) anchor = fw->files[n].data;
    }
    assert(table && anchor && !memcmp(anchor, "_SM3_", 5));
    unsigned sum = 0;
    for (int n = 0; n < 24; n++) sum += anchor[n];
    assert((sum & 255) == 0);
    uint32_t advertised;
    memcpy(&advertised, anchor + 12, 4);
    assert(advertised == size);
    unsigned seen = 0;
    for (const uint8_t *p = table, *end = table + size; p < end;) {
        assert(p + 4 <= end && p[1] >= 4 && p + p[1] + 2 <= end);
        const uint8_t *next = p + p[1];
        while (next + 1 < end && (next[0] || next[1])) next++;
        assert(next + 1 < end);
        const uint8_t *record_end = next + 2;
        if (p[0] == 1) {
            assert(!strcmp(smbios_string(p, record_end, p[5]), identity.product));
            seen |= 1;
        } else if (p[0] == 3) {
            assert(!strcmp(smbios_string(p, record_end, p[6]), machine));
            seen |= 2;
        } else if (p[0] == 4) {
            assert(!strcmp(smbios_string(p, record_end, p[16]), chip));
            assert(p[35] == 4 && p[36] == 4 && p[37] == 4);
            uint64_t processor_id;
            memcpy(&processor_id, p + 8, 8);
            assert(processor_id == 0x414fd0b1ULL); /* Keep the guest's compatible ISA identity. */
            seen |= 4;
        } else if (p[0] == 127) {
            assert(record_end == end);
            seen |= 8;
        }
        p = record_end;
    }
    assert(seen == 15);
    for (int n = 0; n < fw->nfiles; n++) free(fw->files[n].data);
    free(fw->dir_blob);
    free(fw);
}

int main(void) {
    const char *m1[] = {"iPad13,4", "iPad13,5", "iPad13,6", "iPad13,7", "iPad13,8", "iPad13,9", "iPad13,10", "iPad13,11", "iPad13,16", "iPad13,17"};
    for (size_t n = 0; n < sizeof m1 / sizeof *m1; n++) check_smbios(m1[n], "Apple M1");
    check_smbios("iPad14,3", "Apple M2");
    check_smbios("iPad15,3", "Apple M3");
    check_smbios("iPad16,3", "Apple M4");
    check_smbios("iPad17,1", "Apple M5");
    check_smbios("iPad13,1", "Apple A14 Bionic");
    check_smbios("iPad13,18", "Apple A14 Bionic");
    check_smbios("iPad14,1", "Apple A15 Bionic");
    check_smbios("iPad99,1", "Apple Silicon (model unknown)");
    HvHostIdentity unknown;
    assert(!hv_host_identity_for_machine("iPad13,400", &unknown));
    assert(!hv_host_identity_for_machine("iPad13,4junk", &unknown));
    assert(!hv_host_identity_for_machine(NULL, &unknown));
    puts("PASS: host model matching, M1 variants, M2/M3/M4/M5, unknown fallback, SMBIOS record boundaries/checksum and unchanged guest core/ISA identity");
}
