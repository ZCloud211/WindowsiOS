CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

SLIRP_CFLAGS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --cflags slirp)
SLIRP_LIBS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --libs slirp)

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
ENGINE = $(CORE) $(HOST)

DEV = $(B)/acpi.o $(B)/fb_display.o $(B)/fb_metal.o $(B)/fdt.o $(B)/fw_cfg.o $(B)/gicv3.o $(B)/hda.o \
      $(B)/nvme.o $(B)/pcie.o $(B)/pflash.o $(B)/pl011.o $(B)/pl031.o $(B)/pl061.o $(B)/timer.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_input.o $(B)/virtio_net.o $(B)/virtio_snd.o \
      $(B)/winvirt.o $(B)/xhci.o $(B)/wingpu.o $(B)/wingpu_metal.o $(B)/virtio_gpu.o

VM_HOST = $(B)/winvm.o $(B)/net_slirp.o $(B)/share.o $(B)/audio_mac.o $(B)/hvsparse.o $(B)/host_identity.o

MAC_FRAMEWORKS = -framework Cocoa -framework Foundation -framework QuartzCore -framework Metal \
                 -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
                 -framework AudioToolbox

CORE_TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
             $(B)/test_mmu $(B)/test_exc

all: $(CORE_TESTS) $(B)/hvsparse $(B)/winvm

# mmu.c: no SIMD (hv_tlb_refill runs with the guest's SIMD registers live).
$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@
$(B)/net_slirp.o: host/net_slirp.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(B)/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/wingpu_metal.o: devices/wingpu_metal.m gpu/wingpu_protocol.h devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/wingpu.o: devices/wingpu.c gpu/wingpu_protocol.h devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/main_mac.o: host/main_mac.m host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/%.o: core/%.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/winvm: $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST)
	$(CC) $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST) $(SLIRP_LIBS) $(MAC_FRAMEWORKS) -o $@

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) -o $@

test-core: $(CORE_TESTS)
	@ulimit -s 65520; for t in $(CORE_TESTS); do echo "== $$t"; $$t || exit 1; done

$(B)/test_host_identity: tests/test_host_identity.c host/host_identity.c devices/acpi.c devices/fw_cfg.c host/host_identity.h $(ENGINE)
	$(CC) $(CFLAGS) tests/test_host_identity.c host/host_identity.c devices/acpi.c devices/fw_cfg.c $(ENGINE) -o $@
test-host-identity: $(B)/test_host_identity
	$(B)/test_host_identity

$(B)/test_fb_metal: tests/test_fb_metal.m $(B)/fb_metal.o $(B)/fb_display.o $(B)/fw_cfg.o $(ENGINE)
	$(CC) $(OBJCFLAGS) $^ -framework Foundation -framework Metal -framework QuartzCore -framework CoreGraphics -framework ImageIO -o $@
test-fb-metal: $(B)/test_fb_metal
	$(B)/test_fb_metal

$(B)/wingpu_probe.o: gpu/wingpu_probe.c gpu/wingpu_probe.h gpu/wingpu_protocol.h
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/wingpu_probe_arm64.obj: gpu/wingpu_probe.c gpu/wingpu_probe.h gpu/wingpu_protocol.h
	$(CC) --target=aarch64-pc-windows-msvc -ffreestanding -fno-stack-protector -std=c11 -Wall -Wextra -Werror -O2 -c $< -o $@
$(B)/test_wingpu: tests/test_wingpu.c $(B)/wingpu.o $(B)/wingpu_metal.o $(B)/wingpu_probe.o $(B)/pcie.o $(B)/gicv3.o $(ENGINE)
	$(CC) $(CFLAGS) $^ -framework Foundation -framework Metal -o $@
test-wingpu: $(B)/test_wingpu $(B)/wingpu_probe_arm64.obj
	$(B)/test_wingpu

$(B)/test_virtio_gpu: tests/test_virtio_gpu.c $(B)/virtio_gpu.o $(B)/virtio.o $(B)/fb_display.o $(B)/fw_cfg.o $(B)/pcie.o $(B)/gicv3.o $(ENGINE)
	$(CC) $(CFLAGS) $^ -o $@

$(B)/test_gpu_aperture: tests/test_gpu_aperture.c host/gpu_aperture.c $(ENGINE)
	$(CC) $(CFLAGS) $^ -o $@

$(B)/test_virtio_gpu3d: tests/test_virtio_gpu3d.c tests/test_virtio_gpu.c devices/virtio_gpu.c host/gpu_aperture.c $(B)/virtio.o $(B)/fb_display.o $(B)/fw_cfg.o $(B)/pcie.o $(B)/gicv3.o $(ENGINE)
	$(CC) $(CFLAGS) -D_DARWIN_C_SOURCE -DHV_NEPTUNE=1 tests/test_virtio_gpu3d.c devices/virtio_gpu.c host/gpu_aperture.c $(filter %.o,$^) -o $@

ISO_VENDOR = third_party/libudfread/udfread.c third_party/libudfread/ecma167.c third_party/libudfread/default_blockinput.c \
             third_party/fatfs/ff.c third_party/fatfs/ffunicode.c third_party/fatfs/ffsystem.c
ISO_MAC_WIM = work/iso-deps/wimlib-mac/.libs/libwim.a
ISO_MAC_FLAGS = -DHAVE_UNISTD_H=1 -DHAVE_FCNTL_H=1 -Ithird_party/libudfread -Ithird_party/fatfs -Iwork/iso-deps/wimlib-1.14.5/include
ISO_TEST_SOURCES = host/iso_install.c tools/iso_prepare_main.c $(ISO_VENDOR)
$(B)/iso_prepare: $(ISO_TEST_SOURCES) host/iso_install.h third_party/fatfs/ffconf.h $(ISO_MAC_WIM) | $(B)
	$(CC) $(CFLAGS) $(ISO_MAC_FLAGS) $(ISO_TEST_SOURCES) $(ISO_MAC_WIM) -liconv -lpthread -o $@
$(B)/iso_prepare_split_test: $(ISO_TEST_SOURCES) host/iso_install.h third_party/fatfs/ffconf.h $(ISO_MAC_WIM) | $(B)
	$(CC) $(CFLAGS) $(ISO_MAC_FLAGS) -DHV_ISO_FAT_MAX_FILE=524288 -DHV_ISO_SPLIT_PART_BYTES=262144 $(ISO_TEST_SOURCES) $(ISO_MAC_WIM) -liconv -lpthread -o $@
$(B)/driver_media: tools/driver_media_main.c third_party/fatfs/ff.c third_party/fatfs/ffsystem.c third_party/fatfs/ffunicode.c third_party/fatfs/ffconf.h | $(B)
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -Wall -Wextra -Werror -O2 -Ithird_party/fatfs $(filter %.c,$^) -o $@

test-iso: $(B)/iso_prepare $(B)/iso_prepare_split_test
	python3 tests/test_iso_import.py

$(B):
	mkdir -p $(B)

# ---- iOS static library target: build/ios/libwinvm.a ----
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
IOS_SLIRP_INC ?= ios/deps/include/slirp
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -I$(IOS_SLIRP_INC) -Iios/deps/include -Ithird_party/libudfread -Ithird_party/fatfs
IOS_OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
IB = build/ios

IOS_C_SRC = $(wildcard core/*.c) \
            host/jit_region.c host/disk.c host/winvm.c host/net_slirp.c host/share.c \
            host/audio_mac.c host/hvsparse.c host/prof_ios.c host/iso_install.c host/host_identity.c \
            third_party/libudfread/udfread.c third_party/libudfread/ecma167.c third_party/libudfread/default_blockinput.c \
            third_party/fatfs/ff.c third_party/fatfs/ffunicode.c third_party/fatfs/ffsystem.c \
            $(wildcard devices/*.c)
# Built by scripts/build_ios_{dxmt,neptune}.sh and staged by prepare_gpu3d.py.
IOS_NEPTUNE ?= 1
ifeq ($(IOS_NEPTUNE),1)
IOS_C_SRC += host/neptune_runtime.c host/gpu_aperture.c
IOS_CFLAGS += -DHV_NEPTUNE=1 -DVIRGL_RENDERER_UNSTABLE_APIS=1 -Iios/deps/include/virgl
endif
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_C_SRC)) $(IB)/host/enter.o $(IB)/devices/fb_metal.o $(IB)/devices/wingpu_metal.o

$(IB)/third_party/libudfread/%.o: IOS_CFLAGS += -DHAVE_UNISTD_H=1 -DHAVE_FCNTL_H=1
$(IB)/third_party/fatfs/%.o: third_party/fatfs/ffconf.h third_party/fatfs/ff.h third_party/fatfs/diskio.h

$(IB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/devices/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_OBJCFLAGS) -c $< -o $@
$(IB)/devices/wingpu_metal.o: devices/wingpu_metal.m gpu/wingpu_protocol.h devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_OBJCFLAGS) -c $< -o $@
$(IB)/devices/wingpu.o: gpu/wingpu_protocol.h
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
$(IB)/libwinvm.a: $(IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)

ios-lib: $(IB)/libwinvm.a

clean:
	rm -rf $(B)

.PHONY: all test-core test-host-identity test-fb-metal test-wingpu test-iso ios-lib clean
