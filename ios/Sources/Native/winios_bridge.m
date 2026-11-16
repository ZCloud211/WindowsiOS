/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "winios_bridge.h"

#include "hvsparse.h"
#include "iso_install.h"
#include "share.h"
#include "winios_jit.h"
#include "winvm.h"
#include "host_identity.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>

static WinIOSBridge *s_shared_bridge = nil;

@interface WinIOSBridge () {
    BOOL _vmRunning;
    BOOL _unpacking;
    BOOL _importingISO;
    atomic_bool _cancelISO;
    CFAbsoluteTime _isoLastProgressUpdate;
    double _unpackProgress;
    NSString *_vmStatus;
    dispatch_queue_t _diagQueue;
    dispatch_source_t _diagTimer;
    CFAbsoluteTime _bootStartTime;
    uint64_t _configuredRamMB;
    int _configuredCPUs;
    HvHostIdentity _hostIdentity;
    CAMetalLayer *__weak _pendingLayer;
    uint64_t _diagTickCount;
    uint64_t _lastMachTicks;
    uint64_t _lastWfiTicks;
    double _lastWfiIdlePct;

    os_unfair_lock _metricsLock;
    CGSize _screenSizePts;
    CGFloat _screenScale;
    NSInteger _hingeStatus;
    double _hingeAngleDegrees;
}
- (void)handleVMExitWithReason:(NSString *)reason;
- (void)updateUnpackProgress:(double)frac;
- (void)startDiagTimer;
- (void)stopDiagTimer;
- (BOOL)reportISOProgress:(double)progress message:(const char *)message;
@end

static int iso_progress_cb(double progress, const char *message, void *opaque) {
    return [(__bridge WinIOSBridge *)opaque reportISOProgress:progress message:message] ? 0 : 1;
}

static void vm_exit_trampoline(int reason) {
    NSString *r = [NSString stringWithUTF8String:hv_winvm_reason_name(reason)];
    dispatch_async(dispatch_get_main_queue(), ^{
        [s_shared_bridge handleVMExitWithReason:r];
    });
}

typedef struct UnpackCtx {
    __unsafe_unretained WinIOSBridge *bridge;
    CFAbsoluteTime lastUpdate;
} UnpackCtx;

static void unpack_progress_cb(uint64_t done, uint64_t total, void *opaque) {
    UnpackCtx *ctx = (UnpackCtx *)opaque;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (done < total && now - ctx->lastUpdate < 0.2) return;
    ctx->lastUpdate = now;
    double frac = total ? (double)done / (double)total : 0.0;
    WinIOSBridge *b = ctx->bridge;
    dispatch_async(dispatch_get_main_queue(), ^{
        [b updateUnpackProgress:frac];
    });
}

@implementation WinIOSBridge

+ (WinIOSBridge *)shared {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        s_shared_bridge = [[WinIOSBridge alloc] init];
    });
    return s_shared_bridge;
}

- (instancetype)init {
    if ((self = [super init])) {
        _vmStatus = @"Idle";
        hv_host_identity_detect(&_hostIdentity);
        _metricsLock = OS_UNFAIR_LOCK_INIT;
        _screenSizePts = CGSizeZero;
        _screenScale = 3.0;
        _hingeStatus = 0;
        _hingeAngleDegrees = -1.0;
        NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
        BOOL safariDefault = ([ud objectForKey:@"winios.safariOAuthV2"] == nil) ? YES : [ud boolForKey:@"winios.safariOAuthV2"];
        hv_share_set_url_handler_enabled(safariDefault ? 1 : 0);
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        _diagQueue = dispatch_queue_create("me.ammaar.windowsios.diag", attr);
        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:self selector:@selector(onAppBackgroundOrTerminate:) name:UIApplicationDidEnterBackgroundNotification object:nil];
        [nc addObserver:self selector:@selector(onAppBackgroundOrTerminate:) name:UIApplicationWillTerminateNotification object:nil];
        [nc addObserver:self selector:@selector(onAudioInterruption:) name:AVAudioSessionInterruptionNotification object:nil];
    }
    return self;
}

- (void)configureAudioSessionAndMicPermission {
    AVAudioSession *session = [AVAudioSession sharedInstance];
    AVAudioSessionCategoryOptions opts = AVAudioSessionCategoryOptionDefaultToSpeaker |
                                         AVAudioSessionCategoryOptionAllowBluetooth |
                                         AVAudioSessionCategoryOptionAllowBluetoothA2DP |
                                         AVAudioSessionCategoryOptionMixWithOthers;
    NSError *err = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault options:opts error:&err]) {
        /* Fallback to Playback if PlayAndRecord temporarily rejected before permission */
        [session setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:nil];
    }
    [session setPreferredSampleRate:48000.0 error:nil];
    [session setPreferredIOBufferDuration:0.010 error:nil];
    [session setActive:YES error:nil];

    if (@available(iOS 17.0, *)) {
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
            if (granted) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    AVAudioSession *s = [AVAudioSession sharedInstance];
                    [s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault options:opts error:nil];
                    [s setActive:YES error:nil];
                });
            }
        }];
    }
}

- (void)onAudioInterruption:(NSNotification *)note {
    NSNumber *typeNum = note.userInfo[AVAudioSessionInterruptionTypeKey];
    if (typeNum && typeNum.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded) {
        [[AVAudioSession sharedInstance] setActive:YES error:nil];
    }
}

- (void)onAppBackgroundOrTerminate:(NSNotification *)note {
    (void)note;
    if (_vmRunning) {
        dispatch_async(_diagQueue, ^{
            hv_winvm_flush_disks();
        });
    }
}

- (void)notifyChanged {
    if ([NSThread isMainThread]) {
        if (self.onStateChanged) self.onStateChanged();
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (self.onStateChanged) self.onStateChanged();
        });
    }
}

- (void)updateScreenSize:(CGSize)size scale:(CGFloat)scale {
    os_unfair_lock_lock(&_metricsLock);
    _screenSizePts = size;
    if (scale > 0.0) _screenScale = scale;
    os_unfair_lock_unlock(&_metricsLock);
}

- (void)updateHingeStatus:(NSInteger)status angleDegrees:(double)angleDegrees {
    os_unfair_lock_lock(&_metricsLock);
    _hingeStatus = status;
    _hingeAngleDegrees = angleDegrees;
    os_unfair_lock_unlock(&_metricsLock);
}

- (void)installTrapGuard {
    winios_jit_install_trap_handler();
    [[UIApplication sharedApplication] setIdleTimerDisabled:YES];
    UIScreen *mainScreen = [UIScreen mainScreen];
    [self updateScreenSize:mainScreen.bounds.size scale:mainScreen.scale];
    [self notifyChanged];
}

- (BOOL)tryAcquireJIT:(BOOL)detachAfter {
    BOOL ok = winios_jit_acquire(WINIOS_JIT_DEFAULT_BYTES, detachAfter);
    dispatch_async(_diagQueue, ^{
        [self writeDiagnosticsSnapshot];
    });
    [self notifyChanged];
    return ok;
}

- (BOOL)isJITLive {
    return winios_jit_is_live();
}

- (BOOL)isDebuggerAttached {
    return winios_jit_is_debugger_attached();
}

- (BOOL)isVMRunning {
    return _vmRunning;
}

- (BOOL)isUnpacking {
    return _unpacking;
}

- (BOOL)isImportingISO { return _importingISO; }

- (double)unpackProgress {
    return _unpackProgress;
}

- (NSString *)jitStatusText {
    return [NSString stringWithUTF8String:winios_jit_status_text()];
}

- (NSString *)vmStatusText {
    return _vmStatus;
}

- (uint32_t)framebufferWidth {
    uint32_t w = 0, h = 0;
    if (_vmRunning && hv_winvm_display_size(&w, &h) == 0) return w;
    return 0;
}

- (uint32_t)framebufferHeight {
    uint32_t w = 0, h = 0;
    if (_vmRunning && hv_winvm_display_size(&w, &h) == 0) return h;
    return 0;
}

- (uint64_t)presentedFrames {
    return _vmRunning ? hv_winvm_display_frames() : 0;
}

- (uint64_t)availableMemoryMB {
    return (uint64_t)(winios_available_memory_bytes() >> 20);
}

- (uint64_t)requestedMemoryMB {
    NSInteger value = [[NSUserDefaults standardUserDefaults] integerForKey:@"winios.guestMemoryMB"];
    return value == 2048 || value == 3072 || value == 4096 ? (uint64_t)value : 4096;
}

- (void)setRequestedMemoryMB:(uint64_t)value {
    if (_vmRunning || _unpacking) return;
    if (value == 2048 || value == 3072 || value == 4096)
        [[NSUserDefaults standardUserDefaults] setInteger:(NSInteger)value forKey:@"winios.guestMemoryMB"];
}

- (uint64_t)configuredMemoryMB { return _configuredRamMB; }

- (BOOL)firmwareOnly {
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"winios.firmwareOnly"];
}

- (void)setFirmwareOnly:(BOOL)value {
    if (_vmRunning || _unpacking) return;
    [[NSUserDefaults standardUserDefaults] setBool:value forKey:@"winios.firmwareOnly"];
}

- (BOOL)installerAttached {
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"winios.installerAttached"];
}
- (void)setInstallerAttached:(BOOL)value {
    if (_vmRunning || _unpacking) return;
    [[NSUserDefaults standardUserDefaults] setBool:value forKey:@"winios.installerAttached"];
}
- (NSString *)installerName {
    return [[NSUserDefaults standardUserDefaults] stringForKey:@"winios.installerName"] ?: @"Windows ARM64 ISO";
}
- (BOOL)virtioDisplayEnabled {
    return [NSUserDefaults.standardUserDefaults boolForKey:@"winios.virtioDisplayEnabled"];
}
- (void)setVirtioDisplayEnabled:(BOOL)value {
    if (_vmRunning || _unpacking || _importingISO) return;
    [NSUserDefaults.standardUserDefaults setBool:value forKey:@"winios.virtioDisplayEnabled"];
}
- (BOOL)neptune3DEnabled {
    return [NSUserDefaults.standardUserDefaults boolForKey:@"winios.neptune3DEnabled"];
}
- (void)setNeptune3DEnabled:(BOOL)value {
    if (_vmRunning || _unpacking || _importingISO) return;
    [NSUserDefaults.standardUserDefaults setBool:value forKey:@"winios.neptune3DEnabled"];
}
- (BOOL)toolsDiskAttached {
    return [NSUserDefaults.standardUserDefaults boolForKey:@"winios.toolsDiskAttached"];
}
- (void)setToolsDiskAttached:(BOOL)value {
    if (_vmRunning || _unpacking || _importingISO) return;
    [NSUserDefaults.standardUserDefaults setBool:value forKey:@"winios.toolsDiskAttached"];
}

/* Preserve linear RAMFB GOP during boot. The original code.fd and vars.fd stay
 * in place; only the PCI display driver's firmware binding is omitted here. */
- (NSString *)prepareDisplayBootFirmwareInDirectory:(NSString *)dir {
    NSString *folder = [NSBundle.mainBundle pathForResource:@"Firmware" ofType:nil];
    NSData *data = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"ramfb-manifest.json"]];
    NSDictionary *manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *digest = [manifest[@"sha256"] isKindOfClass:NSString.class] ? manifest[@"sha256"] : @"";
    NSCharacterSet *nonHex = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
    if (digest.length != 64 || [digest rangeOfCharacterFromSet:nonHex].location != NSNotFound ||
        [manifest[@"size"] unsignedLongLongValue] != (64ull << 20) || ![manifest[@"ramfb_boot"] boolValue]) {
        _vmStatus = @"启动显示固件不完整，请安装完整构建。";
        return nil;
    }
    NSString *name = [NSString stringWithFormat:@"code-ramfb-%@.fd", [digest substringToIndex:16]];
    NSString *destination = [dir stringByAppendingPathComponent:name];
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:destination]) {
        NSError *error = nil;
        NSString *temporary = [destination stringByAppendingString:@".copying"];
        [fm removeItemAtPath:temporary error:nil];
        if (![fm copyItemAtPath:[folder stringByAppendingPathComponent:@"code-ramfb.fd"] toPath:temporary error:&error] ||
            ![fm moveItemAtPath:temporary toPath:destination error:&error]) {
            [fm removeItemAtPath:temporary error:nil];
            _vmStatus = [NSString stringWithFormat:@"无法准备启动显示固件：%@", error.localizedDescription];
            return nil;
        }
    }
    NSDictionary *attributes = [fm attributesOfItemAtPath:destination error:nil];
    if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes fileSize] != (64ull << 20)) {
        _vmStatus = @"启动显示固件大小异常，已取消启动。";
        return nil;
    }
    return destination;
}

/* A separate, non-bootable driver disk; never edits the system or ISO media. */
- (NSString *)prepareVirtioDisplayMediaInDirectory:(NSString *)dir {
    NSString *folder = [NSBundle.mainBundle pathForResource:@"DisplayDrivers" ofType:nil];
    NSData *data = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"manifest.json"]];
    NSDictionary *manifest = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSString *digest = [manifest[@"image_sha256"] isKindOfClass:NSString.class] ? manifest[@"image_sha256"] : @"";
    NSCharacterSet *nonHex = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
    if (digest.length != 64 || [digest rangeOfCharacterFromSet:nonHex].location != NSNotFound ||
        [manifest[@"image_bytes"] unsignedLongLongValue] != (64ull << 20)) {
        _vmStatus = @"显示驱动盘不完整，请安装完整构建。";
        return nil;
    }
    NSString *destination = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"virtio-display-%@.img", [digest substringToIndex:16]]];
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:destination]) {
        NSError *error = nil;
        NSString *source = [folder stringByAppendingPathComponent:@"drivers.img"];
        NSString *temporary = [destination stringByAppendingString:@".copying"];
        [fm removeItemAtPath:temporary error:nil];
        if (![fm copyItemAtPath:source toPath:temporary error:&error] ||
            ![fm moveItemAtPath:temporary toPath:destination error:&error]) {
            [fm removeItemAtPath:temporary error:nil];
            _vmStatus = [NSString stringWithFormat:@"无法准备 显示驱动盘：%@", error.localizedDescription];
            return nil;
        }
    }
    NSDictionary *attributes = [fm attributesOfItemAtPath:destination error:nil];
    if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes fileSize] != (64ull << 20)) {
        _vmStatus = @"显示驱动盘大小异常，请保留日志。";
        return nil;
    }
    return destination;
}
- (BOOL)installerReady {
    NSString *path = [[self machineDirectory] stringByAppendingPathComponent:@"installer.img"];
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    return [attributes[NSFileType] isEqual:NSFileTypeRegular] && [attributes fileSize] >= (512ull << 20);
}

- (BOOL)usbKeyboardReady {
    if (!_vmRunning) return NO;
    int kr = 0;
    hv_winvm_usb_stats(&kr, NULL, NULL);
    return kr != 0;
}

- (BOOL)usbTabletReady {
    if (!_vmRunning) return NO;
    int tr = 0;
    hv_winvm_usb_stats(NULL, &tr, NULL);
    return tr != 0;
}

- (uint64_t)usbEventsDelivered {
    if (!_vmRunning) return 0;
    uint64_t d = 0;
    hv_winvm_usb_stats(NULL, NULL, &d);
    return d;
}

- (NSString *)documentsPath {
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject ?: NSTemporaryDirectory();
}

- (NSString *)machineDirectory {
    NSString *docs = [self documentsPath];
    NSString *win11Dir = [docs stringByAppendingPathComponent:@"Machines/Win11"];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:[win11Dir stringByAppendingPathComponent:@"code.fd"]]) {
        return win11Dir;
    }
    if ([fm fileExistsAtPath:[docs stringByAppendingPathComponent:@"code.fd"]]) {
        return docs;
    }
    [fm createDirectoryAtPath:win11Dir withIntermediateDirectories:YES attributes:nil error:nil];
    return win11Dir;
}

- (BOOL)hasRawDiskImageAt:(NSString *)dir {
    NSString *raw = [dir stringByAppendingPathComponent:@"windows11.img"];
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:raw error:nil];
    return attrs && [attrs fileSize] >= (64ull << 20);
}

- (BOOL)prepareBundledFirmware {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *dir = [self machineDirectory];
    for (NSString *name in @[@"code.fd", @"vars.fd"]) {
        NSString *destination = [dir stringByAppendingPathComponent:name];
        if ([fm fileExistsAtPath:destination]) continue;
        NSString *source = [NSBundle.mainBundle pathForResource:name ofType:nil inDirectory:@"Firmware"];
        NSError *error = nil;
        if (!source || ![fm copyItemAtPath:source toPath:destination error:&error]) {
            _vmStatus = source
                ? [NSString stringWithFormat:@"Cannot prepare UEFI firmware: %@", error.localizedDescription]
                : @"UEFI firmware is missing from this app. Install the complete build.";
            [self notifyChanged];
            return NO;
        }
    }
    return YES;
}

- (BOOL)needsSparseUnpack {
    NSString *dir = [self machineDirectory];
    if ([self hasRawDiskImageAt:dir]) return NO;
    NSString *sparse = [dir stringByAppendingPathComponent:@"windows11.hvsparse"];
    return [[NSFileManager defaultManager] fileExistsAtPath:sparse];
}

- (BOOL)hasProvisionedMachine {
    NSString *dir = [self machineDirectory];
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"code.fd"]]) return NO;
    if (![fm fileExistsAtPath:[dir stringByAppendingPathComponent:@"vars.fd"]]) return NO;
    if (self.firmwareOnly) return YES;
    return [self hasRawDiskImageAt:dir] || [self needsSparseUnpack];
}

- (void)updateUnpackProgress:(double)frac {
    _unpackProgress = frac;
    _vmStatus = [NSString stringWithFormat:@"Unpacking Windows 11 NVMe image (%.0f%%)...", frac * 100.0];
    dispatch_async(_diagQueue, ^{
        [self writeDiagnosticsSnapshot];
    });
    [self notifyChanged];
}

- (BOOL)reportISOProgress:(double)progress message:(const char *)message {
    if (atomic_load(&_cancelISO)) return NO;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (progress < 1.0 && now - _isoLastProgressUpdate < 0.15) return YES;
    _isoLastProgressUpdate = now;
    NSString *text = [NSString stringWithUTF8String:message] ?: @"正在导入 ISO…";
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self->_importingISO) return;
        self->_unpackProgress = progress;
        self->_vmStatus = text;
        [self notifyChanged];
    });
    return YES;
}

- (void)cancelISOImport { atomic_store(&_cancelISO, true); }

- (void)importISOFromURL:(NSURL *)url {
    if (_vmRunning || _unpacking) return;
    if (![url.pathExtension.lowercaseString isEqualToString:@"iso"]) {
        _vmStatus = @"请选择 Windows ARM64 的 .iso 文件。";
        [self notifyChanged]; return;
    }
    if (![self prepareBundledFirmware]) return;
    BOOL scoped = [url startAccessingSecurityScopedResource];
    _importingISO = YES; _unpacking = YES; _unpackProgress = 0;
    atomic_store(&_cancelISO, false);
    _isoLastProgressUpdate = 0;
    _vmStatus = @"正在读取 ISO，请保持应用在前台…";
    [self notifyChanged];
    NSString *machine = [self machineDirectory];
    NSString *token = NSUUID.UUID.UUIDString;
    NSString *temporary = [machine stringByAppendingPathComponent:[NSString stringWithFormat:@"installer.%@.importing", token]];
    NSString *installer = [machine stringByAppendingPathComponent:@"installer.img"];
    NSString *disk = [machine stringByAppendingPathComponent:@"windows11.img"];
    NSString *scratch = [NSTemporaryDirectory() stringByAppendingPathComponent:[@"iso-" stringByAppendingString:token]];
    [[UIApplication sharedApplication] setIdleTimerDisabled:YES];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSFileManager *fm = NSFileManager.defaultManager;
        __block NSString *failure = nil;
        __block BOOL createdDisk = NO;
        NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        NSError *coordinationError = nil;
        [coordinator coordinateReadingItemAtURL:url options:0 error:&coordinationError byAccessor:^(NSURL *readURL) {
            NSError *error = nil;
            NSDictionary *attributes = [fm attributesOfItemAtPath:readURL.path error:&error];
            uint64_t size = [attributes fileSize];
            if (!attributes || size < 32768 || size > (64ull << 30)) {
                failure = @"无法读取 ISO，或镜像大小超出支持范围。请先在“文件”App中下载完整文件。"; return;
            }
            NSDictionary *space = [fm attributesOfFileSystemForPath:machine error:&error];
            uint64_t freeBytes = [space[NSFileSystemFreeSize] unsignedLongLongValue];
            uint64_t required = size * 3 + (512ull << 20);
            if (![self hasRawDiskImageAt:machine]) required = MAX(24ull << 30, required);
            if (!space || freeBytes < required) {
                failure = [NSString stringWithFormat:@"空间不足。导入和安装至少需要约 %llu GiB 可用空间，当前约 %llu GiB。", (required + (1ull << 30) - 1) >> 30, freeBytes >> 30]; return;
            }
            if (![fm createDirectoryAtPath:scratch withIntermediateDirectories:YES attributes:nil error:&error]) {
                failure = @"无法创建 ISO 导入缓存。"; return;
            }
            char detail[768] = {0};
            if (hv_iso_prepare(readURL.fileSystemRepresentation, temporary.fileSystemRepresentation, scratch.fileSystemRepresentation,
                               iso_progress_cb, (__bridge void *)self, detail, sizeof detail)) {
                failure = [NSString stringWithUTF8String:detail] ?: @"ISO 导入失败。"; return;
            }
            if (atomic_load(&self->_cancelISO)) { failure = @"已取消导入。"; return; }
            int fd = open(disk.fileSystemRepresentation, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) {
                createdDisk = YES;
                BOOL ok = ftruncate(fd, (off_t)(64ull << 30)) == 0 && fsync(fd) == 0;
                close(fd);
                if (!ok) { failure = @"无法创建 Windows 系统盘，请检查存储空间。"; return; }
            } else if (errno != EEXIST || ![self hasRawDiskImageAt:machine]) {
                failure = @"无法使用 Windows 系统盘；已有数据未被覆盖。"; return;
            }
            if (rename(temporary.fileSystemRepresentation, installer.fileSystemRepresentation)) {
                failure = @"无法保存安装盘；已有系统盘未被修改。";
            }
        }];
        if (coordinationError && !failure) failure = coordinationError.localizedDescription;
        if (scoped) [url stopAccessingSecurityScopedResource];
        [fm removeItemAtPath:scratch error:nil];
        if (failure) {
            unlink(temporary.fileSystemRepresentation);
            if (createdDisk) unlink(disk.fileSystemRepresentation);
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_importingISO = NO; self->_unpacking = NO;
            [[UIApplication sharedApplication] setIdleTimerDisabled:NO];
            if (failure) {
                self->_vmStatus = failure;
            } else {
                self->_unpackProgress = 1;
                self.firmwareOnly = NO;
                self.installerAttached = YES;
                [NSUserDefaults.standardUserDefaults setObject:url.lastPathComponent forKey:@"winios.installerName"];
                self->_vmStatus = @"安装盘已准备好。启用 JIT 后点击启动，进入 Windows 安装程序。已有系统盘不会被自动清空。";
            }
            dispatch_async(self->_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
            [self notifyChanged];
        });
    });
}

- (void)bindMetalLayer:(nullable CAMetalLayer *)layer {
    _pendingLayer = layer;
    if (_vmRunning) {
        hv_winvm_set_display_layer((__bridge void *)layer);
    }
}

- (void)prepareAndStartWithLayer:(nullable CAMetalLayer *)layer {
    if (layer) _pendingLayer = layer;
    if (_vmRunning || _unpacking) return;
    if (![self prepareBundledFirmware]) {
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        return;
    }

    if (!self.firmwareOnly && self.installerAttached && self.installerReady) {
        NSString *installer = [[self machineDirectory] stringByAppendingPathComponent:@"installer.img"];
        char detail[512] = {0};
        if (hv_iso_repair_media(installer.fileSystemRepresentation, detail, sizeof detail) < 0) {
            _vmStatus = [NSString stringWithUTF8String:detail] ?: @"无法准备安装盘。";
            dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
            [self notifyChanged]; return;
        }
    }

    if (!self.isJITLive) {
        [self tryAcquireJIT:YES];
        if (!self.isJITLive) {
            _vmStatus = @"Enable JIT for Windows 11 in StikDebug, then return here.";
            dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
            [self notifyChanged];
            return;
        }
    }

    if (![self hasProvisionedMachine]) {
        _vmStatus = self.firmwareOnly
            ? @"Place code.fd and vars.fd in Documents/Machines/Win11 for the UEFI test."
            : @"Place code.fd, vars.fd, and windows11.img (or .hvsparse) in Documents/Machines/Win11.";
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        [self notifyChanged];
        return;
    }

    NSString *dir = [self machineDirectory];
    if (!self.firmwareOnly && [self needsSparseUnpack]) {
        _unpacking = YES;
        _unpackProgress = 0.0;
        _vmStatus = @"Unpacking Windows 11 NVMe image (0%)...";
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        [self notifyChanged];

        NSString *src = [dir stringByAppendingPathComponent:@"windows11.hvsparse"];
        NSString *dst = [dir stringByAppendingPathComponent:@"windows11.img"];
        NSString *tmpDst = [dir stringByAppendingPathComponent:@"windows11.img.unpacking"];

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            unlink(tmpDst.fileSystemRepresentation);
            UnpackCtx ctx = {.bridge = self, .lastUpdate = 0};
            int rc = hv_sparse_unpack(src.fileSystemRepresentation, tmpDst.fileSystemRepresentation, unpack_progress_cb, &ctx);
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_unpacking = NO;
                if (rc == 0 && rename(tmpDst.fileSystemRepresentation, dst.fileSystemRepresentation) == 0) {
                    self->_unpackProgress = 1.0;
                    [self startConfiguredVMInDirectory:dir];
                } else {
                    unlink(tmpDst.fileSystemRepresentation);
                    self->_vmStatus = @"Error: failed to unpack windows11.hvsparse";
                    dispatch_async(self->_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
                    [self notifyChanged];
                }
            });
        });
        return;
    }

    [self startConfiguredVMInDirectory:dir];
}

- (void)startConfiguredVMInDirectory:(NSString *)dir {
    if (_vmRunning) return;
    if (!self.firmwareOnly) [self configureAudioSessionAndMicPermission];

    WinIosDualMapping jit = winios_jit_mapping();
    if (!jit.rx_addr || !jit.rw_addr) {
        _vmStatus = @"Error: JIT region missing";
        [self notifyChanged];
        return;
    }

    NSString *ramBacking = [NSTemporaryDirectory() stringByAppendingPathComponent:@"win11_guest_ram.bin"];
    setenv("HV_RAM_FILE", ramBacking.fileSystemRepresentation, 1);

    uint64_t preallocBytes = 0;
    void *preallocRam = winios_jit_guest_ram(&preallocBytes);
    uint64_t ramMB = self.firmwareOnly ? 2048 : self.requestedMemoryMB;
    if (preallocRam && preallocBytes >= (ramMB << 20)) {
        // The backing can be larger than the requested guest allocation.
    } else {
        preallocRam = NULL;
        uint64_t availMB = [self availableMemoryMB];
        // Leave space for JIT tables, framebuffers, audio and UIKit. Do not
        // silently allocate more than the OS reports this process can use.
        if (availMB < ramMB + 768) {
            _vmStatus = [NSString stringWithFormat:@"Not enough memory for %llu MiB. Close other apps or choose a smaller memory size (%llu MiB available).", ramMB, availMB];
            [self notifyChanged];
            return;
        }
    }

    NSString *sharedDir = [[self documentsPath] stringByAppendingPathComponent:@"Shared"];
    [[NSFileManager defaultManager] createDirectoryAtPath:sharedDir withIntermediateDirectories:YES attributes:nil error:nil];
    /* Keep AF_UNIX sun_path well under Darwin's 104-byte limit (~82 bytes vs 106 bytes) */
    NSString *rawTmp = [NSTemporaryDirectory() stringByReplacingOccurrencesOfString:@"/private/var/" withString:@"/var/"];
    NSString *shareSock = [rawTmp stringByAppendingPathComponent:@"s"];
    unlink(shareSock.fileSystemRepresentation);

    NSString *codePath = [dir stringByAppendingPathComponent:@"code.fd"];
    NSString *varsPath = [dir stringByAppendingPathComponent:@"vars.fd"];
    NSString *diskPath = [dir stringByAppendingPathComponent:@"windows11.img"];
    NSString *installerPath = [dir stringByAppendingPathComponent:@"installer.img"];
    BOOL displayGPU = self.virtioDisplayEnabled && !self.firmwareOnly;
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        CGSize nativeSize = UIScreen.mainScreen.nativeBounds.size;
        os_unfair_lock_lock(&_metricsLock);
        CGSize viewport = _screenSizePts;
        os_unfair_lock_unlock(&_metricsLock);
        BOOL landscape = viewport.width >= viewport.height;
        uint32_t panelWidth = (uint32_t)(landscape ? MAX(nativeSize.width, nativeSize.height) : MIN(nativeSize.width, nativeSize.height));
        uint32_t panelHeight = (uint32_t)(landscape ? MIN(nativeSize.width, nativeSize.height) : MAX(nativeSize.width, nativeSize.height));
        setenv("HV_DISPLAY_WIDTH", [[NSString stringWithFormat:@"%u", panelWidth] UTF8String], 1);
        setenv("HV_DISPLAY_HEIGHT", [[NSString stringWithFormat:@"%u", panelHeight] UTF8String], 1);
    }
    if (displayGPU) {
        codePath = [self prepareDisplayBootFirmwareInDirectory:dir];
        if (!codePath) { [self notifyChanged]; return; }
    }
    NSString *secondaryPath = !self.firmwareOnly && self.installerAttached && self.installerReady ? installerPath : nil;
    if (self.toolsDiskAttached && !self.firmwareOnly) {
        if (self.installerAttached) {
            _vmStatus = @"请先关闭安装盘挂载，再挂载驱动／日志盘。";
            [self notifyChanged];
            return;
        }
        secondaryPath = [self prepareVirtioDisplayMediaInDirectory:dir];
        if (!secondaryPath) { [self notifyChanged]; return; }
    }
    setenv("HV_VIRTIO_GPU", displayGPU ? "1" : "0", 1);
    setenv("HV_WINGPU_PROBE", "0", 1);
    if (displayGPU && self.neptune3DEnabled) {
        NSString *nativeDXMT = [NSBundle.mainBundle.privateFrameworksPath stringByAppendingPathComponent:@"DXMT.framework/DXMT"];
        if (![NSFileManager.defaultManager fileExistsAtPath:nativeDXMT]) {
            _vmStatus = @"3D 渲染组件缺失，请安装完整 IPA。";
            [self notifyChanged]; return;
        }
        setenv("HV_NEPTUNE_DXMT", nativeDXMT.fileSystemRepresentation, 1);
    } else unsetenv("HV_NEPTUNE_DXMT");

    _configuredRamMB = ramMB;
    _configuredCPUs = 4;

    HvWinVmConfig cfg = {
        .code_fd = codePath.fileSystemRepresentation,
        .vars_fd = varsPath.fileSystemRepresentation,
        .disk0 = self.firmwareOnly ? NULL : diskPath.fileSystemRepresentation,
        .disk1 = secondaryPath.fileSystemRepresentation,
        .share_dir = sharedDir.fileSystemRepresentation,
        .share_sock = shareSock.fileSystemRepresentation,
        .ncpus = _configuredCPUs,
        .ram_mb = _configuredRamMB,
        .jit_bytes = jit.size,
        .jit_rx = jit.rx_addr,
        .jit_rw = jit.rw_addr,
        .prealloc_ram = preallocRam,
        .no_net = 0,
        .enable_sound = self.firmwareOnly ? 0 : 1,
        .log = 0,
        .stats_interval = 0,
    };

    char err[256] = {0};
    if (hv_winvm_create(&cfg, err, sizeof err) != 0) {
        _vmStatus = [NSString stringWithFormat:@"VM create failed: %s", err];
        dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
        [self notifyChanged];
        return;
    }

    CAMetalLayer *layer = _pendingLayer;
    if (layer) {
        hv_winvm_set_display_layer((__bridge void *)layer);
    }

    _vmRunning = YES;
    _bootStartTime = CFAbsoluteTimeGetCurrent();
    _diagTickCount = 0;
    _lastMachTicks = mach_absolute_time();
    _lastWfiTicks = 0;
    _lastWfiIdlePct = 0.0;
    _vmStatus = [NSString stringWithFormat:@"Running %@ (%d vCPUs, %llu MiB RAM)", self.firmwareOnly ? @"UEFI test" : @"Windows 11 ARM64", _configuredCPUs, _configuredRamMB];
    [[UIApplication sharedApplication] setIdleTimerDisabled:YES];

    hv_winvm_start(1, vm_exit_trampoline);

    [self startDiagTimer];
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
    [self notifyChanged];
}

- (void)startDiagTimer {
    [self stopDiagTimer];
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _diagQueue);
    dispatch_source_set_timer(timer,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)(5 * NSEC_PER_SEC)),
                              8ull * NSEC_PER_SEC,
                              1ull * NSEC_PER_SEC);
    __weak typeof(self) weakSelf = self;
    dispatch_source_set_event_handler(timer, ^{
        typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;
        [strongSelf onDiagTickBackground];
    });
    _diagTimer = timer;
    dispatch_resume(timer);
}

- (void)stopDiagTimer {
    if (_diagTimer) {
        dispatch_source_cancel(_diagTimer);
        _diagTimer = nil;
    }
}

- (void)onDiagTickBackground {
    _diagTickCount++;
    [self writeDiagnosticsSnapshot];
    if (_vmRunning) {
        if ((_diagTickCount % 4u) == 0u) {
            hv_winvm_flush_disks();
        }
        double uptime = CFAbsoluteTimeGetCurrent() - _bootStartTime;
        if (uptime <= 240.0 || getenv("WINIOS_ALWAYS_SCREENSHOT") != NULL) {
            NSString *docs = [self documentsPath];
            NSString *tmpPng = [docs stringByAppendingPathComponent:@"latest_screen.png.tmp"];
            NSString *dstPng = [docs stringByAppendingPathComponent:@"latest_screen.png"];
            if (hv_winvm_save_png(tmpPng.fileSystemRepresentation) == 0) {
                rename(tmpPng.fileSystemRepresentation, dstPng.fileSystemRepresentation);
            }
        }
    }
    [self notifyChanged];
}

- (void)writeDiagnosticsSnapshot {
    NSString *docs = [self documentsPath];
    NSString *statusFile = [docs stringByAppendingPathComponent:@"boot_status.txt"];
    BOOL running = _vmRunning;
    double uptime = running ? (CFAbsoluteTimeGetCurrent() - _bootStartTime) : 0.0;
    uint32_t w = [self framebufferWidth], h = [self framebufferHeight];
    uint64_t frames = [self presentedFrames];
    HvFbMetalStats graphics = {0};
    if (running) hv_winvm_graphics_stats(&graphics);
    HvVirtioGpuStats displayGPU = {0};
    if (running) hv_virtio_gpu_stats(hv_winvm_board()->display_gpu, &displayGPU);

    int kbdReady = 0, tabReady = 0;
    uint64_t usbDelivered = 0;
    uint64_t jitUsedBytes = 0, jitCapBytes = 0, jitFullFlushes = 0, wfiTicks = 0;
    uint64_t systemBytes = 0, installerBytes = 0, systemReads = 0, installerReads = 0;
    double wfiIdlePct = _lastWfiIdlePct;
    if (running) {
        hv_winvm_usb_stats(&kbdReady, &tabReady, &usbDelivered);
        hv_winvm_engine_stats(&jitUsedBytes, &jitCapBytes, &jitFullFlushes, &wfiTicks);
        hv_winvm_storage_stats(&systemBytes, &installerBytes, &systemReads, &installerReads);
        uint64_t nowMach = mach_absolute_time();
        if (_lastMachTicks > 0 && nowMach > _lastMachTicks && wfiTicks >= _lastWfiTicks && _configuredCPUs > 0) {
            double pct = 100.0 * (double)(wfiTicks - _lastWfiTicks) /
                         ((double)(nowMach - _lastMachTicks) * (double)_configuredCPUs);
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;
            wfiIdlePct = pct;
            _lastWfiIdlePct = pct;
        }
        _lastMachTicks = nowMach;
        _lastWfiTicks = wfiTicks;
    }

    os_unfair_lock_lock(&_metricsLock);
    CGSize pt = _screenSizePts;
    CGFloat scale = _screenScale;
    NSInteger hingeStatus = _hingeStatus;
    double hingeDeg = _hingeAngleDegrees;
    os_unfair_lock_unlock(&_metricsLock);

    NSString *txt = [NSString stringWithFormat:
        @"host_os: %@ %@\n"
        @"host_machine: %s\n"
        @"host_cpu: %s\n"
        @"metal_device: %s\n"
        @"metal_ready: %d\n"
        @"metal_shared_scanout: %d\n"
        @"metal_completed_frames: %llu\n"
        @"metal_failed_frames: %llu\n"
        @"metal_error: %s\n"
        @"virtio_gpu_present: %d\n"
        @"tools_disk_attached: %d\n"
        @"boot_gop: ramfb\n"
        @"virtio_firmware_gop: disabled\n"
        @"virtio_gpu_commands: %llu\n"
        @"virtio_gpu_transfers: %llu\n"
        @"virtio_gpu_transfer_bytes: %llu\n"
        @"virtio_gpu_flushes: %llu\n"
        @"virtio_gpu_errors: %llu\n"
        @"virtio_gpu_mode: %ux%u\n"
        @"virtio_gpu_driver_status: 0x%02x\n"
        @"virtio_gpu_msix_enabled: %u\n"
        @"virtio_gpu_vectors: config=%u control=%u cursor=%u\n"
        @"virtio_gpu_queues_enabled: control=%u cursor=%u\n"
        @"guest_graphics: %@\n"
        @"guest_3d_acceleration: %@\n"
        @"neptune_backend_ready: %u\n"
        @"neptune_contexts: %u\n"
        @"neptune_submissions: %llu\n"
        @"neptune_blob_creates: %llu\n"
        @"neptune_blob_maps: %llu\n"
        @"neptune_retired_fences: %llu\n"
        @"neptune_pending_fences: %u\n"
        @"boot_mode: %@\n"
        @"requested_ram_mb: %llu\n"
        @"vm_running: %@\n"
        @"vm_status: %@\n"
        @"jit_live: %@\n"
        @"jit_status: %@\n"
        @"uptime_sec: %.1f\n"
        @"cpus: %d\n"
        @"ram_mb: %llu\n"
        @"avail_mem_mb: %llu\n"
        @"system_disk_mb: %llu\n"
        @"installer_disk_mb: %llu\n"
        @"secondary_disk_role: %@\n"
        @"installer_attached: %d\n"
        @"system_read_bytes: %llu\n"
        @"installer_read_bytes: %llu\n"
        @"jit_used_mb: %.1f\n"
        @"jit_cap_mb: %.0f\n"
        @"jit_full_flushes: %llu\n"
        @"wfi_idle_pct: %.1f\n"
        @"fb_size: %ux%u\n"
        @"frames_presented: %llu\n"
        @"usb_kbd_ready: %d\n"
        @"usb_tab_ready: %d\n"
        @"usb_delivered: %llu\n"
        @"share_up: %d\n"
        @"oauth_safari: %d\n"
        @"hinge_status: %ld\n"
        @"hinge_angle_deg: %.1f\n"
        @"screen_pts: %.0fx%.0f @%.0fx\n",
        UIDevice.currentDevice.systemName, UIDevice.currentDevice.systemVersion,
        _hostIdentity.machine, _hostIdentity.chip,
        graphics.device, graphics.ready, graphics.shared_scanout,
        graphics.completed, graphics.failed, graphics.error,
        running && hv_winvm_board()->display_gpu != NULL,
        running && installerBytes > 0 && self.toolsDiskAttached,
        displayGPU.commands, displayGPU.transfers, displayGPU.transferred_bytes,
        displayGPU.flushes, displayGPU.errors, displayGPU.width, displayGPU.height,
        displayGPU.driver_status, displayGPU.msix_enabled,
        displayGPU.config_vector, displayGPU.control_vector, displayGPU.cursor_vector,
        displayGPU.control_enabled, displayGPU.cursor_enabled,
        displayGPU.backend_3d_ready ? @"neptune-native-dxmt-metal" : (displayGPU.width ? @"virtio-gpu-display-only" : @"firmware-framebuffer"),
        displayGPU.backend_3d_ready ? @"pending-windows-validation" : @"false",
        displayGPU.backend_3d_ready,displayGPU.contexts_3d,displayGPU.submissions_3d,
        displayGPU.blob_creates,displayGPU.blob_maps,displayGPU.retired_fences,displayGPU.pending_fences,
        self.firmwareOnly ? @"uefi" : @"windows",
        self.firmwareOnly ? 2048ull : self.requestedMemoryMB,
        running ? @"true" : @"false",
        _vmStatus ?: @"",
        self.isJITLive ? @"true" : @"false",
        self.jitStatusText ?: @"",
        uptime,
        _configuredCPUs,
        _configuredRamMB,
        [self availableMemoryMB],
        systemBytes >> 20, installerBytes >> 20,
        running && installerBytes ? (self.toolsDiskAttached ? @"tools-and-logs" : @"installer") : @"none",
        running ? (installerBytes > 0 && self.installerAttached) : self.installerAttached,
        systemReads, installerReads,
        (double)jitUsedBytes / (1024.0 * 1024.0),
        (double)jitCapBytes / (1024.0 * 1024.0),
        jitFullFlushes,
        wfiIdlePct,
        w, h,
        frames,
        kbdReady,
        tabReady,
        usbDelivered,
        running ? hv_winvm_share_up() : 0,
        hv_share_url_handler_enabled(),
        (long)hingeStatus,
        hingeDeg,
        pt.width, pt.height, scale];
    [txt writeToFile:statusFile atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

- (void)handleVMExitWithReason:(NSString *)reason {
    _vmRunning = NO;
    [self stopDiagTimer];
    [[UIApplication sharedApplication] setIdleTimerDisabled:NO];
    _vmStatus = [NSString stringWithFormat:@"Stopped (%@)", reason];
    dispatch_async(_diagQueue, ^{ [self writeDiagnosticsSnapshot]; });
    [self notifyChanged];
}

- (void)sendTabletNormalizedX:(double)x y:(double)y buttons:(uint8_t)buttons wheel:(int8_t)wheel {
    if (!_vmRunning) return;
    hv_winvm_tablet(x, y, buttons, wheel);
}

- (void)sendKeyHid:(uint8_t)usage down:(BOOL)down {
    if (!_vmRunning) return;
    hv_winvm_key_hid(usage, down ? 1 : 0);
}

- (void)tapKeyHid:(uint8_t)usage {
    if (!_vmRunning) return;
    hv_winvm_key_hid(usage, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(35 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        hv_winvm_key_hid(usage, 0);
    });
}

- (void)sendKeyComboWithModifiers:(NSArray<NSNumber *> *)modifiers key:(uint8_t)usage {
    if (!_vmRunning) return;
    for (NSNumber *m in modifiers) {
        hv_winvm_key_hid((uint8_t)m.unsignedCharValue, 1);
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(18 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        if (usage != 0) hv_winvm_key_hid(usage, 1);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(38 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
            if (usage != 0) hv_winvm_key_hid(usage, 0);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(15 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
                for (NSNumber *m in [modifiers reverseObjectEnumerator]) {
                    hv_winvm_key_hid((uint8_t)m.unsignedCharValue, 0);
                }
            });
        });
    });
}

- (void)sendWindowsKey {
    [self tapKeyHid:0xe3]; /* Left GUI / Windows key */
}

- (void)sendEscape {
    [self tapKeyHid:0x29]; /* Escape */
}

- (void)sendAltTab {
    [self sendKeyComboWithModifiers:@[@(0xe2)] key:0x2b];
}

- (void)sendCtrlAltDel {
    if (!_vmRunning) return;
    hv_winvm_send_ctrl_alt_del();
}

- (void)pressPowerButton {
    if (!_vmRunning) return;
    hv_winvm_power_button();
}

- (void)flushDisks {
    if (!_vmRunning) return;
    hv_winvm_flush_disks();
}

- (void)stopVM {
    if (!_vmRunning) return;
    hv_winvm_stop();
}

- (BOOL)useSafariForGuestLinks {
    return hv_share_url_handler_enabled() != 0;
}

- (void)setUseSafariForGuestLinks:(BOOL)useSafariForGuestLinks {
    hv_share_set_url_handler_enabled(useSafariForGuestLinks ? 1 : 0);
    [[NSUserDefaults standardUserDefaults] setBool:useSafariForGuestLinks forKey:@"winios.safariOAuthV2"];
}

- (nullable NSString *)takeGuestOpenURL {
    char *raw = hv_share_take_open_url();
    if (!raw) return nil;
    NSString *s = [NSString stringWithUTF8String:raw];
    free(raw);
    return s;
}

- (BOOL)takeGuestOAuthDone {
    return hv_share_take_oauth_done() != 0;
}

@end
