/* SPDX-License-Identifier: GPL-2.0-or-later
 * Device/queue integration with a controlled asynchronous renderer. The actual
 * native Metal/DXMT backend is tested separately, not simulated here. */
#define main existing_display_tests
#include "test_virtio_gpu.c"
#undef main
#include "neptune_runtime.h"
#include <sys/mman.h>
struct HvNeptune {int unused;};
static struct HvNeptune backend;
static HvNeptuneFence completion;
static int fence_available,allow_completion,submissions,presents;
HvNeptune *hv_neptune_create(const char *path,char *error,size_t size) {(void)error;(void)size;assert(!strcmp(path,"test-backend"));return &backend;}
void hv_neptune_destroy(HvNeptune *r) {assert(!r || r==&backend);}
int hv_neptune_caps(HvNeptune *r,uint32_t *version,void *data,uint32_t *size) {
 assert(r==&backend);*version=0;*size=16;if(data)memset(data,0x74,16);return 0;
}
int hv_neptune_context_create(HvNeptune *r,uint32_t id,uint32_t flags,const char *name,uint32_t len) {
 assert(r==&backend && id==12 && flags==7 && len==4 && !memcmp(name,"test",4));return 0;
}
void hv_neptune_context_destroy(HvNeptune *r,uint32_t id) {assert(r==&backend && id==12);}
int hv_neptune_submit(HvNeptune *r,uint32_t ctx,const void *commands,uint32_t bytes) {
 assert(r==&backend && ctx==12 && bytes==8 && *(const uint64_t *)commands==UINT64_C(0x123456789abcdef));submissions++;return 0;
}
int hv_neptune_fence(HvNeptune *r,uint32_t ctx,uint32_t ring,uint64_t value) {
 assert(r==&backend);completion=(HvNeptuneFence){ctx,ring,value};fence_available=1;return 0;
}
int hv_neptune_pop_fence(HvNeptune *r,HvNeptuneFence *f) {
 assert(r==&backend);if(!allow_completion || !fence_available)return 0;*f=completion;fence_available=0;return 1;
}
int hv_neptune_blob_create(HvNeptune *r,const HvNeptuneBlob *d) {
 assert(r==&backend && d->resource==31 && d->context==12 && d->size==4096 && d->memory==2 && d->flags==1);return 0;
}
void hv_neptune_blob_unref(HvNeptune *r,uint32_t id) {assert(r==&backend && id==31);}
void hv_neptune_attach(HvNeptune *r,uint32_t ctx,uint32_t id,int attach) {assert(r==&backend && ctx==12 && id==31);(void)attach;}
int hv_neptune_map_info(HvNeptune *r,uint32_t id,uint32_t *info) {assert(r==&backend && id==31);*info=1;return 0;}
int hv_neptune_map_view(HvNeptune *r,uint32_t id,void **address,uint64_t *length) {
 assert(r==&backend && id==31);*length=4096;*address=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);return *address==MAP_FAILED?-1:0;
}
int hv_neptune_present(HvNeptune *r,HvFbDisplay *fb,uint32_t id,uint32_t w,uint32_t h,uint32_t stride,uint64_t off,int rgba) {
 assert(r==&backend && fb && id==31 && w==16 && h==16 && stride==64 && !off && !rgba);presents++;return 0;
}
static const Header *send3d(HvVirtioGpu *gpu,void *buffer,uint32_t bytes,uint32_t type,uint32_t ctx,uint32_t expected,int deferred) {
 Header *h=buffer;*h=(Header){.type=type,.flags=3,.fence=++fence,.context=ctx,.padding=3};
 assert(!hv_phys_write(machine,INPUT,buffer,bytes));
 struct {uint64_t addr;uint32_t len;uint16_t flags,next;} d[2]={{INPUT,bytes,1,1},{OUTPUT,1080,2,0}};
 assert(!hv_phys_write(machine,DESC,d,sizeof d));uint16_t *a=hv_phys_ptr(machine,AVAIL,64);a[2+sequence%8]=0;
 __atomic_store_n(a+1,++sequence,__ATOMIC_RELEASE);wr(BAR+0x3000,0);
 if(deferred) {
  assert(*(uint16_t *)hv_phys_ptr(machine,USED+2,2)==sequence-1);
  allow_completion=1;hv_virtio_gpu_poll(gpu);allow_completion=0;
 }
 const Header *out=hv_phys_ptr(machine,OUTPUT,1080);
 assert(*(uint16_t *)hv_phys_ptr(machine,USED+2,2)==sequence && out->type==expected && out->fence==fence && out->context==ctx && out->padding==3);
 return out;
}
int main(void) {
 assert(!existing_display_tests());using_msix=0;sequence=0;
 setenv("HV_NEPTUNE_DXMT","test-backend",1);
 machine=calloc(1,sizeof *machine);pthread_mutex_init(&machine->dev_mu,NULL);
 void *ram=calloc(1,32u<<20);assert(!hv_machine_add_ram(machine,RAM,32u<<20,ram,0));
 HvGic *gic=hv_gicv3_create(machine,0x8000000,0x80a0000);
 HvPcie *bus=hv_pcie_create(machine,gic,ECAM,0x10000000,0x10000000,0x1000000,35);
 hv_pcie_add_high_mmio(bus,UINT64_C(0x8000000000),UINT64_C(1)<<32);
 HvFbDisplay *fb=hv_fb_display_create(machine,NULL);HvVirtioGpu *gpu=hv_virtio_gpu_create(machine,bus,fb);assert(gpu);
 HvVirtio *v=hv_virtio_gpu_device(gpu);uint64_t config=ECAM+((uint64_t)v->pci.slot<<15);
 assert((v->features&27)==27 && v->pci.config[0x34]==0xa0 && v->pci.config[0xa3]==8 && v->config[12]==1);
 wr(config+0x20,BAR);wr(config+0x18,0);wr(config+0x1c,0x80);wr(config+4,2);
 wr(BAR+0x16,0);wr(BAR+0x18,8);address(0x20,DESC);address(0x28,AVAIL);address(0x30,USED);wr(BAR+0x1c,1);wr(BAR+0x14,15);
 struct {Header h;uint32_t id,pad;} cap={0};send3d(gpu,&cap,sizeof cap,0x108,0,0x1102,0);
 cap.id=7;send3d(gpu,&cap,sizeof cap,0x109,0,0x1103,0);
 struct {Header h;uint32_t len,flags;char name[64];} ctx={.len=4,.flags=7,.name="test"};
 send3d(gpu,&ctx,sizeof ctx,0x200,12,0x1100,1);
 struct {Header h;uint32_t id,mem,flags,count;uint64_t blob,size;} blob={.id=31,.mem=2,.flags=1,.size=4096};
 send3d(gpu,&blob,sizeof blob,0x10c,12,0x1100,1);
 struct {Header h;uint32_t id,pad;uint64_t offset;} map={.id=31,.offset=4096};
 send3d(gpu,&map,sizeof map,0x208,12,0x1106,1);
 assert(hv_phys_ptr(machine,UINT64_C(0x8000001000),4096));
 uint64_t value=0xabcdef;assert(!hv_phys_write(machine,UINT64_C(0x8000001000),&value,8));
 assert(*(uint64_t *)hv_phys_ptr(machine,UINT64_C(0x8000001000),8)==value);
 struct {Header h;uint32_t bytes,pad;uint64_t payload;} submit={.bytes=8,.payload=UINT64_C(0x123456789abcdef)};
 send3d(gpu,&submit,sizeof submit,0x207,12,0x1100,1);assert(submissions==1);
 struct {Header h;Rect rect;uint32_t scanout,id,w,height,fmt,pad,stride[4],offset[4];} scan={.rect={0,0,16,16},.id=31,.w=16,.height=16,.fmt=1,.stride={64}};
 send3d(gpu,&scan,sizeof scan,0x10d,12,0x1100,1);assert(presents==1);
 struct {Header h;Rect rect;uint32_t id,pad;} flush={.rect={0,0,16,16},.id=31};
 send3d(gpu,&flush,sizeof flush,0x104,12,0x1100,1);assert(presents==2);
 cap.id=31;send3d(gpu,&cap,sizeof cap,0x209,12,0x1100,1);assert(!hv_phys_ptr(machine,UINT64_C(0x8000001000),1));
 send3d(gpu,&cap,sizeof cap,0x102,12,0x1100,1);
 Header destroy={0};send3d(gpu,&destroy,sizeof destroy,0x201,12,0x1100,0);
 hv_virtio_gpu_destroy(gpu);
 puts("PASS: Neptune capset/context/blob/host-visible PCI BAR/submit/scanout/flush/unmap/unref/destroy and deferred per-context ring fences through real PCI split queues");
 return 0;
}
