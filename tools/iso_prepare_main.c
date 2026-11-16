/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "iso_install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int progress(double fraction, const char *message, void *context) {
    int *last = context, value = (int)(fraction * 100);
    const char *cancel = getenv("ISO_PREPARE_CANCEL_AT");
    if (cancel && fraction >= strtod(cancel, NULL)) return 1;
    if (value >= *last + 10 || value == 100) { fprintf(stderr, "%d%% %s\n", value, message); *last = value; }
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--repair")) {
        char error[512] = {0};
        int result = hv_iso_repair_media(argv[2], error, sizeof error);
        if (result < 0) fprintf(stderr, "%s\n", error);
        else printf("%s\n", result ? "repaired" : "unchanged");
        return result < 0 ? 1 : 0;
    }
    if (argc != 4) { fprintf(stderr, "usage: iso_prepare ISO OUTPUT SCRATCH_DIRECTORY\n"); return 2; }
    char error[512] = {0}; int last = -10;
    int rc = hv_iso_prepare(argv[1], argv[2], argv[3], progress, &last, error, sizeof error);
    if (rc) fprintf(stderr, "%s\n", error);
    return rc ? 1 : 0;
}
