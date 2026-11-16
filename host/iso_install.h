/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_ISO_INSTALL_H
#define HV_ISO_INSTALL_H
#include <stddef.h>

/* Return nonzero from progress to cancel. This API prepares installation media
 * once; VM disk I/O never calls it. Output must be a new temporary file. */
typedef int (*HvIsoProgress)(double fraction, const char *message, void *context);
int hv_iso_prepare(const char *iso, const char *output, const char *scratch,
                   HvIsoProgress progress, void *context, char *error, size_t error_size);
/* Before attaching a stopped VM's installer, repair the generated ESP
 * partition type and missing MBR disk identity. Never call while
 * the VM is running. Returns 1 if repaired, 0 if unchanged, -1 on error. */
int hv_iso_repair_media(const char *path, char *error, size_t error_size);
#endif
