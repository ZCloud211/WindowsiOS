/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_HOST_IDENTITY_H
#define HV_HOST_IDENTITY_H

typedef struct HvHostIdentity {
    char machine[64];
    char product[96];
    char chip[64];
} HvHostIdentity;

/* Unknown machines keep an explicit generic chip name rather than guessing. */
int hv_host_identity_for_machine(const char *machine, HvHostIdentity *identity);
void hv_host_identity_detect(HvHostIdentity *identity);

#endif
