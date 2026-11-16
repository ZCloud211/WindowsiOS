/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "iso_install.h"
#include "udfread.h"
#include "ff.h"
#include "diskio.h"
#include <wimlib.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIB (UINT64_C(1) << 20)
#define GIB (UINT64_C(1) << 30)
#define BUFFER_BYTES (2u << 20)
#ifndef HV_ISO_FAT_MAX_FILE
#define HV_ISO_FAT_MAX_FILE UINT64_C(0xffffffff)
#endif
#ifndef HV_ISO_SPLIT_PART_BYTES
#define HV_ISO_SPLIT_PART_BYTES (3800 * MIB)
#endif
#define FAT_MAX_FILE HV_ISO_FAT_MAX_FILE

typedef struct Entry { char *path; uint64_t size; int directory; } Entry;
typedef struct Import {
    udfread *udf;
    Entry *entries;
    size_t count, capacity;
    uint64_t total, iso_size;
    HvIsoProgress progress;
    void *context;
    char *error;
    size_t error_size;
    uint8_t *buffer;
    double last_progress;
    int original_created;
} Import;

/* FatFs is private to the importer. Serialize imports; no VM thread uses it. */
static pthread_mutex_t import_mutex = PTHREAD_MUTEX_INITIALIZER;
static int media_fd = -1;
static LBA_t media_sectors;

DSTATUS disk_initialize(BYTE drive) { return drive == 0 && media_fd >= 0 ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }

static DRESULT transfer(BYTE drive, void *buffer, LBA_t sector, UINT count, int writing) {
    if (drive || media_fd < 0 || !count || sector >= media_sectors || count > media_sectors - sector) return RES_PARERR;
    uint8_t *bytes = buffer;
    size_t remaining = (size_t)count * 512;
    off_t offset = (off_t)sector * 512;
    while (remaining) {
        ssize_t n = writing ? pwrite(media_fd, bytes, remaining, offset) : pread(media_fd, bytes, remaining, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return RES_ERROR;
        bytes += n; offset += n; remaining -= (size_t)n;
    }
    return RES_OK;
}
DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) { return transfer(drive, buffer, sector, count, 0); }
DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) { return transfer(drive, (void *)buffer, sector, count, 1); }
DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
    if (drive || media_fd < 0) return RES_NOTRDY;
    switch (command) {
    case CTRL_SYNC: return fsync(media_fd) == 0 ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *(LBA_t *)buffer = media_sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buffer = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buffer = 2048; return RES_OK;
    default: return RES_PARERR;
    }
}

static int fail(Import *i, const char *format, ...) {
    if (i->error && i->error_size) {
        va_list args; va_start(args, format);
        vsnprintf(i->error, i->error_size, format, args); va_end(args);
    }
    return -1;
}
static int report(Import *i, double fraction, const char *message) {
    if (fraction < i->last_progress) fraction = i->last_progress;
    i->last_progress = fraction;
    if (i->progress && i->progress(fraction, message, i->context)) return fail(i, "已取消导入。");
    return 0;
}
static int add(Import *i, const char *path, uint64_t size, int directory) {
    if (i->count >= 100000 || size > i->iso_size || size > 64 * GIB || i->total > 64 * GIB - size)
        return fail(i, "ISO 内容超出支持范围。");
    if (i->count == i->capacity) {
        size_t capacity = i->capacity ? i->capacity * 2 : 256;
        Entry *entries = realloc(i->entries, capacity * sizeof *entries);
        if (!entries) return fail(i, "没有足够内存读取 ISO 目录。");
        i->entries = entries; i->capacity = capacity;
    }
    char *name = strdup(path);
    if (!name) return fail(i, "没有足够内存读取 ISO 文件名。");
    i->entries[i->count++] = (Entry){name, size, directory};
    i->total += size;
    return 0;
}
static int scan(Import *i, const char *path, int depth) {
    if (depth > 32) return fail(i, "ISO 目录层级过深。");
    UDFDIR *dir = udfread_opendir(i->udf, path);
    if (!dir) return fail(i, "无法读取 ISO 目录：%s", path);
    int rc = 0;
    struct udfread_dirent entry;
    while (udfread_readdir(dir, &entry)) {
        const char *name = entry.d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!*name || strpbrk(name, "/\\:") || strlen(name) > 255) { rc = fail(i, "ISO 包含不支持的文件名。"); break; }
        char child[1024];
        int length = snprintf(child, sizeof child, "%s%s%s", path, strcmp(path, "/") ? "/" : "", name);
        if (length < 0 || (size_t)length >= sizeof child) { rc = fail(i, "ISO 文件路径过长。"); break; }
        if (entry.d_type == UDF_DT_DIR) {
            if ((rc = add(i, child, 0, 1)) || (rc = scan(i, child, depth + 1))) break;
        } else if (entry.d_type == UDF_DT_REG) {
            UDFFILE *file = udfread_file_openat(dir, name);
            int64_t size = file ? udfread_file_size(file) : -1;
            if (file) udfread_file_close(file);
            if (size < 0) { rc = fail(i, "无法读取 ISO 文件：%s", child); break; }
            if ((rc = add(i, child, (uint64_t)size, 0))) break;
        } else { rc = fail(i, "ISO 包含不支持的文件类型。"); break; }
        if ((rc = report(i, 0.01, "正在检查 ISO 内容…"))) break;
    }
    udfread_closedir(dir);
    return rc;
}
static Entry *find(Import *i, const char *path) {
    for (size_t n = 0; n < i->count; n++) if (!strcasecmp(i->entries[n].path, path)) return &i->entries[n];
    return NULL;
}
static int validate_arm64(Import *i) {
    Entry *boot = find(i, "/efi/boot/bootaa64.efi");
    if (!boot || boot->directory || boot->size < 64) return fail(i, "请选择 Windows ARM64 ISO；镜像中缺少 BOOTAA64.EFI。");
    UDFFILE *file = udfread_file_open(i->udf, boot->path);
    uint8_t header[64], signature[6];
    int ok = file && udfread_file_read(file, header, 64) == 64 && header[0] == 'M' && header[1] == 'Z';
    uint32_t offset = ok ? (uint32_t)header[60] | (uint32_t)header[61] << 8 | (uint32_t)header[62] << 16 | (uint32_t)header[63] << 24 : 0;
    ok = ok && offset <= boot->size - 6 && udfread_file_seek(file, offset, UDF_SEEK_SET) == offset &&
        udfread_file_read(file, signature, 6) == 6 && !memcmp(signature, "PE\0\0\x64\xaa", 6);
    if (file) udfread_file_close(file);
    if (!ok) return fail(i, "ISO 的启动程序不是有效的 ARM64 程序。");
    Entry *pe = find(i, "/sources/boot.wim");
    if (!pe || !pe->size) return fail(i, "ISO 中缺少 Windows 安装环境 boot.wim。");
    if (!find(i, "/sources/install.wim") && !find(i, "/sources/install.esd") && !find(i, "/sources/install.swm"))
        return fail(i, "ISO 中缺少 Windows 安装映像。");
    return 0;
}
static int fat_error(Import *i, FRESULT result, const char *operation) {
    return result == FR_OK ? 0 : fail(i, "%s失败（%d）。请检查剩余存储空间与 ISO。", operation, result);
}
static int copy_udf_to_posix(Import *i, Entry *entry, const char *destination) {
    int fd = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
    i->original_created = fd >= 0;
    UDFFILE *file = udfread_file_open(i->udf, entry->path);
    if (fd < 0 || !file) { if (fd >= 0) close(fd); if (file) udfread_file_close(file); return fail(i, "无法准备较大的 Windows 安装文件。"); }
    uint64_t done = 0; int rc = 0;
    while (done < entry->size) {
        size_t amount = entry->size - done > BUFFER_BYTES ? BUFFER_BYTES : (size_t)(entry->size - done);
        ssize_t n = udfread_file_read(file, i->buffer, amount);
        if (n <= 0) { rc = fail(i, "读取 Windows 安装文件失败。"); break; }
        size_t offset = 0;
        while (offset < (size_t)n) {
            ssize_t written = write(fd, i->buffer + offset, (size_t)n - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { rc = fail(i, "写入安装缓存失败，可能是存储空间不足。"); break; }
            offset += (size_t)written;
        }
        if (rc) break;
        done += (uint64_t)n;
        if ((rc = report(i, 0.03 + 0.22 * (double)done / (double)entry->size, "正在准备大于 4GB 的安装文件…"))) break;
    }
    udfread_file_close(file); close(fd);
    return rc;
}
static enum wimlib_progress_status wim_progress(enum wimlib_progress_msg message, union wimlib_progress_info *info, void *context) {
    Import *i = context;
    if (message == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART || message == WIMLIB_PROGRESS_MSG_SPLIT_END_PART) {
        double fraction = info->split.total_bytes ? (double)info->split.completed_bytes / (double)info->split.total_bytes : 0;
        if (report(i, 0.25 + 0.25 * fraction, "正在拆分 Windows 安装文件…")) return WIMLIB_PROGRESS_STATUS_ABORT;
    }
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}
static int copy_to_fat(Import *i, const char *destination, UDFFILE *udf, int local_fd, uint64_t size,
                       uint64_t *done, uint64_t total) {
    FIL file;
    if (fat_error(i, f_open(&file, destination, FA_CREATE_NEW | FA_WRITE), "创建安装盘文件")) return -1;
    int rc = 0; uint64_t copied = 0;
    while (copied < size) {
        size_t amount = size - copied > BUFFER_BYTES ? BUFFER_BYTES : (size_t)(size - copied);
        ssize_t n = udf ? udfread_file_read(udf, i->buffer, amount) : read(local_fd, i->buffer, amount);
        if (n < 0 && errno == EINTR && !udf) continue;
        if (n <= 0) { rc = fail(i, "安装文件读取不完整。"); break; }
        UINT written = 0;
        FRESULT result = f_write(&file, i->buffer, (UINT)n, &written);
        if (result != FR_OK || written != (UINT)n) { rc = fail(i, "写入安装盘失败，可能是存储空间不足。"); break; }
        copied += (uint64_t)n; *done += (uint64_t)n;
        if ((rc = report(i, 0.5 + 0.49 * (double)*done / (double)(total ? total : 1), "正在创建 Windows 安装盘…"))) break;
    }
    FRESULT closed = f_close(&file);
    return rc ? rc : fat_error(i, closed, "保存安装盘文件");
}

static int missing_disk_identity(const uint8_t *mbr) {
    uint32_t identity;
    memcpy(&identity, mbr + 440, sizeof identity);
    return identity == 0 || identity == UINT32_MAX;
}

static void set_disk_identity(uint8_t *mbr) {
    do {
        arc4random_buf(mbr + 440, 4);
    } while (missing_disk_identity(mbr));
}

int hv_iso_repair_media(const char *path, char *error, size_t error_size) {
    pthread_mutex_lock(&import_mutex);
    Import i = {.error = error, .error_size = error_size};
    if (error && error_size) *error = 0;
    int result = -1, registered = 0;
    FATFS fs;
    struct stat status;
    uint8_t mbr[512], boot[512];
    media_fd = open(path, O_RDWR | O_NOFOLLOW);
    if (media_fd < 0 || fstat(media_fd, &status) || !S_ISREG(status.st_mode) ||
        status.st_size < (off_t)(512 * MIB) || (uint64_t)status.st_size > 64 * GIB || status.st_size % 512 ||
        pread(media_fd, mbr, sizeof mbr, 0) != sizeof mbr) {
        fail(&i, "无法检查安装盘，请确认应用文件可读写。"); goto done;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xaa || (mbr[450] != 0xef && mbr[450] != 0x0c) ||
        (mbr[450] == 0x0c && !missing_disk_identity(mbr))) { result = 0; goto done; }
    /* Only migrate our original one-partition WIN11SETUP media. */
    for (int part = 1; part < 4; part++) if (mbr[446 + 16 * part + 4]) { result = 0; goto done; }
    uint32_t start = (uint32_t)mbr[454] | (uint32_t)mbr[455] << 8 | (uint32_t)mbr[456] << 16 | (uint32_t)mbr[457] << 24;
    uint32_t sectors = (uint32_t)mbr[458] | (uint32_t)mbr[459] << 8 | (uint32_t)mbr[460] << 16 | (uint32_t)mbr[461] << 24;
    if (!start || !sectors || (uint64_t)start + sectors > (uint64_t)status.st_size / 512 ||
        pread(media_fd, boot, sizeof boot, (off_t)start * 512) != sizeof boot ||
        memcmp(boot + 82, "FAT32   ", 8)) { result = 0; goto done; }
    media_sectors = (LBA_t)((uint64_t)status.st_size / 512);
    registered = 1;
    if (f_mount(&fs, "0:", 1) != FR_OK) { fail(&i, "安装盘文件系统无法读取，请重新导入 ISO。"); goto done; }
    char label[64];
    DWORD serial;
    if (f_getlabel("0:", label, &serial) != FR_OK || strcmp(label, "WIN11SETUP")) { result = 0; goto done; }
    FILINFO info;
    if (f_stat("0:/efi/boot/bootaa64.efi", &info) != FR_OK || f_stat("0:/sources/boot.wim", &info) != FR_OK) {
        fail(&i, "安装盘缺少启动文件，请重新导入 ISO。"); goto done;
    }
    f_mount(NULL, "0:", 0); registered = 0;
    /* FatFs leaves the Windows MBR disk identity zero. Initialize it before
     * WinPE enumerates the disk, preserving any existing nonzero identity.
     * The partition start/length and filesystem sectors remain unchanged. */
    if (missing_disk_identity(mbr)) set_disk_identity(mbr);
    mbr[450] = 0x0c;
    if (pwrite(media_fd, mbr, sizeof mbr, 0) != sizeof mbr || fsync(media_fd)) {
        fail(&i, "无法保存安装盘兼容修复，请检查存储空间。"); goto done;
    }
    result = 1;
done:
    if (registered) f_mount(NULL, "0:", 0);
    if (media_fd >= 0) close(media_fd);
    media_fd = -1; media_sectors = 0;
    pthread_mutex_unlock(&import_mutex);
    return result;
}

int hv_iso_prepare(const char *iso, const char *output, const char *scratch,
                   HvIsoProgress progress, void *context, char *error, size_t error_size) {
    pthread_mutex_lock(&import_mutex);
    Import i = {.progress = progress, .context = context, .error = error, .error_size = error_size};
    if (error && error_size) *error = 0;
    int rc = -1, mounted = 0, created = 0;
    FATFS fs;
    char original[2048] = {0}, split[2048] = {0};
    struct stat status;
    WIMStruct *wim = NULL;
    if (stat(iso, &status) || !S_ISREG(status.st_mode) || status.st_size < 32768) { fail(&i, "ISO 文件不存在或不完整。"); goto cleanup; }
    i.iso_size = (uint64_t)status.st_size;
    i.buffer = malloc(BUFFER_BYTES);
    i.udf = udfread_init();
    if (!i.buffer || !i.udf) { fail(&i, "没有足够内存导入 ISO。"); goto cleanup; }
    if (udfread_open(i.udf, iso)) { fail(&i, "无法读取 ISO 的 UDF 文件系统。请选择完整的 Windows ARM64 ISO。"); goto cleanup; }
    if (scan(&i, "/", 0) || validate_arm64(&i)) goto cleanup;
    Entry *large = NULL;
    for (size_t n = 0; n < i.count; n++) {
        Entry *entry = &i.entries[n];
        if (entry->size <= FAT_MAX_FILE) continue;
        if (strcasecmp(entry->path, "/sources/install.wim") || large) { fail(&i, "此 ISO 含有不支持的超大文件：%s。请选择使用 install.wim 的 Windows ARM64 ISO。", entry->path); goto cleanup; }
        large = entry;
    }
    if (large) {
        if (snprintf(original, sizeof original, "%s/install.wim", scratch) >= (int)sizeof original ||
            snprintf(split, sizeof split, "%s/install.swm", scratch) >= (int)sizeof split) { fail(&i, "安装缓存路径过长。"); goto cleanup; }
        if (copy_udf_to_posix(&i, large, original)) goto cleanup;
        int result = wimlib_open_wim_with_progress(original, 0, &wim, wim_progress, &i);
        if (!result) result = wimlib_split(wim, split, HV_ISO_SPLIT_PART_BYTES, 0);
        if (result) { if (!error || !error_size || !*error) fail(&i, "拆分 Windows 安装文件失败：%s", wimlib_get_error_string(result)); goto cleanup; }
        wimlib_free(wim); wim = NULL;
        unlink(original); original[0] = 0;
    }
    if (report(&i, 0.5, "正在创建 Windows 安装盘…")) goto cleanup;
    uint64_t disk_bytes = (i.total + i.total / 32 + 256 * MIB + MIB - 1) & ~(MIB - 1);
    if (disk_bytes < 512 * MIB) disk_bytes = 512 * MIB;
    if (disk_bytes > 64 * GIB) { fail(&i, "安装盘超出支持范围。"); goto cleanup; }
    media_fd = open(output, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (media_fd < 0) { fail(&i, "无法创建安装盘临时文件。"); goto cleanup; }
    created = 1; media_sectors = (LBA_t)(disk_bytes / 512);
    if (ftruncate(media_fd, (off_t)disk_bytes)) { fail(&i, "无法分配安装盘空间。"); goto cleanup; }
    MKFS_PARM options = {.fmt = FM_FAT32, .n_fat = 2, .align = 2048, .au_size = 4096};
    if (fat_error(&i, f_mkfs("0:", &options, i.buffer, BUFFER_BYTES), "格式化安装盘")) goto cleanup;
    /* Installation media is a normal FAT32 partition, not a hidden ESP.
     * UEFI loads BOOTAA64.EFI from FAT32; WinPE must also mount the volume
     * and assign a drive letter so Setup can find sources/install.*. */
    uint8_t sector[512];
    if (pread(media_fd, sector, sizeof sector, 0) != sizeof sector || sector[510] != 0x55 || sector[511] != 0xaa) { fail(&i, "安装盘分区表无效。"); goto cleanup; }
    sector[446] = 0x80; sector[450] = 0x0c;
    set_disk_identity(sector);
    if (pwrite(media_fd, sector, sizeof sector, 0) != sizeof sector) { fail(&i, "无法保存安装盘分区表。"); goto cleanup; }
    mounted = 1;
    if (fat_error(&i, f_mount(&fs, "0:", 1), "挂载安装盘")) goto cleanup;
    if (fat_error(&i, f_setlabel("0:WIN11SETUP"), "设置安装盘名称")) goto cleanup;
    uint64_t copied = 0;
    for (size_t n = 0; n < i.count; n++) {
        Entry *entry = &i.entries[n];
        char destination[1100];
        snprintf(destination, sizeof destination, "0:%s", entry->path);
        if (entry->directory) {
            FRESULT result = f_mkdir(destination);
            if (result != FR_EXIST && fat_error(&i, result, "创建安装盘目录")) goto cleanup;
        } else if (entry != large) {
            UDFFILE *file = udfread_file_open(i.udf, entry->path);
            if (!file) { fail(&i, "无法打开 ISO 文件：%s", entry->path); goto cleanup; }
            int result = copy_to_fat(&i, destination, file, -1, entry->size, &copied, i.total);
            udfread_file_close(file);
            if (result) goto cleanup;
        }
    }
    if (large) {
        for (unsigned part = 1; part <= 1000; part++) {
            char local[2048], destination[64];
            if (part == 1) { snprintf(local, sizeof local, "%s/install.swm", scratch); snprintf(destination, sizeof destination, "0:/sources/install.swm"); }
            else { snprintf(local, sizeof local, "%s/install%u.swm", scratch, part); snprintf(destination, sizeof destination, "0:/sources/install%u.swm", part); }
            if (stat(local, &status)) { if (part > 1 && errno == ENOENT) break; fail(&i, "拆分后的安装文件缺失。"); goto cleanup; }
            if ((uint64_t)status.st_size > FAT_MAX_FILE) { fail(&i, "拆分后的单个安装文件仍超过 4GB。"); goto cleanup; }
            int fd = open(local, O_RDONLY);
            if (fd < 0) { fail(&i, "无法读取拆分后的安装文件。"); goto cleanup; }
            int result = copy_to_fat(&i, destination, NULL, fd, (uint64_t)status.st_size, &copied, i.total);
            close(fd);
            if (result) goto cleanup;
            unlink(local);
        }
    }
    /* Stock Windows media checks for TPM/Secure Boot before installation. The
     * emulated board has neither; configure only these setup compatibility
     * checks, leaving account creation and disk selection to the user. */
    if (!find(&i, "/autounattend.xml")) {
        static const char answer[] =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">"
            "<settings pass=\"windowsPE\"><component name=\"Microsoft-Windows-Setup\" "
            "processorArchitecture=\"arm64\" publicKeyToken=\"31bf3856ad364e35\" "
            "language=\"neutral\" versionScope=\"nonSxS\" xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">"
            "<RunSynchronous>"
            "<RunSynchronousCommand wcm:action=\"add\"><Order>1</Order><Path>reg.exe add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>"
            "<RunSynchronousCommand wcm:action=\"add\"><Order>2</Order><Path>reg.exe add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>"
            "<RunSynchronousCommand wcm:action=\"add\"><Order>3</Order><Path>reg.exe add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>"
            "</RunSynchronous></component></settings></unattend>\n";
        FIL file;
        if (fat_error(&i, f_open(&file, "0:/autounattend.xml", FA_CREATE_NEW | FA_WRITE), "准备安装兼容设置")) goto cleanup;
        UINT written = 0;
        FRESULT result = f_write(&file, answer, sizeof answer - 1, &written);
        FRESULT closed = f_close(&file);
        if (result != FR_OK || written != sizeof answer - 1 || closed != FR_OK) { fail(&i, "无法保存安装兼容设置。"); goto cleanup; }
    }
    if (report(&i, 0.995, "正在保存安装盘…")) goto cleanup;
    if (fsync(media_fd)) { fail(&i, "保存安装盘失败。"); goto cleanup; }
    rc = report(&i, 1.0, "ISO 导入完成。");
cleanup:
    if (wim) wimlib_free(wim);
    if (*original && i.original_created) unlink(original);
    if (mounted) f_mount(NULL, "0:", 0);
    if (media_fd >= 0) close(media_fd);
    media_fd = -1; media_sectors = 0;
    if (rc && created) unlink(output);
    if (i.udf) udfread_close(i.udf);
    for (size_t n = 0; n < i.count; n++) free(i.entries[n].path);
    free(i.entries); free(i.buffer);
    pthread_mutex_unlock(&import_mutex);
    return rc;
}
