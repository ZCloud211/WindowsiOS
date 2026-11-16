/* SPDX-License-Identifier: GPL-2.0-or-later
 * Offline-only FAT32 driver media builder. Never opens any physical device.
 * Usage: driver_media output.img path/to/file [...]. Copies flat files only.
 */
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTORS (64u * 1024u * 1024u / 512u)
static int media = -1;
DSTATUS disk_initialize(BYTE drive) { return drive || media < 0 ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }
static DRESULT transfer(BYTE drive, void *buffer, LBA_t sector, UINT count, int writing) {
    if (drive || !count || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    size_t bytes = (size_t)count * 512;
    ssize_t done = writing ? pwrite(media, buffer, bytes, (off_t)sector * 512)
                           : pread(media, buffer, bytes, (off_t)sector * 512);
    return done == (ssize_t)bytes ? RES_OK : RES_ERROR;
}
DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) { return transfer(d,b,s,n,0); }
DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) { return transfer(d,(void *)b,s,n,1); }
DRESULT disk_ioctl(BYTE d, BYTE command, void *buffer) {
    if (d || media < 0) return RES_PARERR;
    switch (command) {
    case CTRL_SYNC: return fsync(media) ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buffer = SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buffer = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buffer = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
static void check(FRESULT result) {
    if (result != FR_OK) { fprintf(stderr, "FatFs error %u\n", result); exit(1); }
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int first=2;
    const char *label="WINGPUTEST";
    if(argc>=5 && !strcmp(argv[2],"--label")) {
        label=argv[3];first=4;
        if(!*label || strlen(label)>11 || strspn(label,"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")!=strlen(label)) return 2;
    }
    /* O_EXCL prevents accidental replacement of an existing image. */
    media = open(argv[1], O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (media < 0 || ftruncate(media, (off_t)SECTORS * 512)) { perror("create image"); return 1; }
    BYTE buffer[65536];
    MKFS_PARM format = {.fmt=FM_FAT32, .n_fat=2, .align=2048, .au_size=512};
    check(f_mkfs("0:", &format, buffer, sizeof buffer));
    FATFS fs;
    check(f_mount(&fs, "0:", 1));
    char fat_label[16];snprintf(fat_label,sizeof fat_label,"0:%s",label);
    check(f_setlabel(fat_label));
    for (int i = first; i < argc; i++) {
        const char *base = strrchr(argv[i], '/');
        base = base ? base + 1 : argv[i];
        if (!*base || strpbrk(base, "\\:")) return 2;
        char path[512];
        if (snprintf(path, sizeof path, "0:/%s", base) >= (int)sizeof path) return 2;
        int source = open(argv[i], O_RDONLY | O_NOFOLLOW);
        struct stat statbuf;
        if (source < 0 || fstat(source, &statbuf) || !S_ISREG(statbuf.st_mode) || statbuf.st_size > (16 << 20)) return 2;
        FIL file;
        check(f_open(&file, path, FA_CREATE_NEW | FA_WRITE));
        ssize_t bytes;
        while ((bytes = read(source, buffer, sizeof buffer)) > 0) {
            UINT written;
            check(f_write(&file, buffer, (UINT)bytes, &written));
            if (written != bytes) return 1;
        }
        if (bytes < 0) return 1;
        check(f_close(&file));
        close(source);
        /* Round-trip verify every byte through the FAT filesystem. */
        check(f_open(&file, path, FA_READ));
        source = open(argv[i], O_RDONLY | O_NOFOLLOW);
        BYTE compare[65536];
        while ((bytes = read(source, buffer, sizeof buffer)) > 0) {
            UINT got;
            check(f_read(&file, compare, (UINT)bytes, &got));
            if (got != bytes || memcmp(buffer, compare, got)) return 1;
        }
        if (bytes < 0 || !f_eof(&file)) return 1;
        check(f_close(&file)); close(source);
    }
    check(f_mount(NULL, "0:", 0));
    if (pread(media, buffer, 512, 0) != 512 || buffer[510] != 0x55 || buffer[511] != 0xaa) return 1;
    uint32_t identity;
    do { arc4random_buf(&identity, sizeof identity); } while (!identity || identity == UINT32_MAX);
    memcpy(buffer + 440, &identity, 4);
    buffer[450] = 0x0c; /* Windows-visible FAT32 data partition, not EFI hidden. */
    if (pwrite(media, buffer, 512, 0) != 512 || fsync(media)) return 1;
    close(media);
    printf("Verified FAT32 %s image with %d files, 64MiB, nonzero MBR identity.\n",label, argc - first);
    return 0;
}
