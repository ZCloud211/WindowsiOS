/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "virtio_gpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RAM UINT64_C(0x40000000)
#define ECAM UINT64_C(0x4010000000)
#define BAR UINT64_C(0x10100000)
#define MSIX UINT64_C(0x10200000)
#define DOORBELL UINT64_C(0x09000000)
#define DESC (RAM+0x1000)
#define AVAIL (RAM+0x2000)
#define USED (RAM+0x3000)
#define INPUT (RAM+0x10000)
#define OUTPUT (RAM+0x80000)
typedef struct Header { uint32_t type,flags;uint64_t fence;uint32_t context,padding; } Header;
typedef struct Rect {uint32_t x,y,w,h;} Rect;
static HvMachine *machine;
static uint16_t sequence;
static uint64_t fence;
static unsigned messages;
static uint32_t message;
static int using_msix;
static void msi_write(void *unused,uint64_t off,uint64_t value,unsigned size) {
 (void)unused;assert(off==0 && size==4);messages++;message=(uint32_t)value;
}
static void wr(uint64_t pa,uint32_t value) {assert(!hv_phys_write(machine,pa,&value,4));}
static uint32_t rd(uint64_t pa) {uint32_t v;assert(!hv_phys_read(machine,pa,&v,4));return v;}
static void address(uint32_t reg,uint64_t pa) {wr(BAR+reg,(uint32_t)pa);wr(BAR+reg+4,(uint32_t)(pa>>32));}
static const uint8_t *request(void *buffer,size_t size,uint32_t type,uint32_t expected) {
 Header *h=buffer;*h=(Header){.type=type,.flags=1,.fence=++fence};
 assert(!hv_phys_write(machine,INPUT,buffer,size));
 struct Descriptor {uint64_t addr;uint32_t len;uint16_t flags,next;} d[2]={{INPUT,(uint32_t)size,1,1},{OUTPUT,1080,2,0}};
 assert(!hv_phys_write(machine,DESC,d,sizeof d));
 uint16_t *a=hv_phys_ptr(machine,AVAIL,64);a[2+sequence%8]=0;
 __atomic_store_n(a+1,++sequence,__ATOMIC_RELEASE);
 wr(BAR+0x3000,0);
 const uint8_t *out=hv_phys_ptr(machine,OUTPUT,1080);const Header *response=(const void *)out;
 assert(response->type==expected && response->flags==1 && response->fence==fence);
 assert(*(uint16_t *)hv_phys_ptr(machine,USED+2,2)==sequence);
 assert(rd(BAR+0x1000)==(using_msix?0u:1u));
 return out;
}
static void mode(uint32_t id,uint32_t w,uint32_t h) {
 struct {Header h;uint32_t id,format,w,height;} create={.id=id,.format=2,.w=w,.height=h};
 request(&create,sizeof create,0x101,0x1100);
 uint32_t bytes=w*h*4;
 uint8_t *first=hv_phys_ptr(machine,RAM+0x200000,4096),*rest=hv_phys_ptr(machine,RAM+0x600000,bytes-4096);
 memset(first,0x32,4096);memset(rest,0x78,bytes-4096);
 struct {Header h;uint32_t id,count;struct {uint64_t pa;uint32_t len,pad;} entries[2];} attach={.id=id,.count=2,.entries={{RAM+0x200000,4096,0},{RAM+0x600000,bytes-4096,0}}};
 request(&attach,sizeof attach,0x106,0x1100);
 struct {Header h;Rect r;uint32_t screen,id;} scan={.r={0,0,w,h},.id=id};
 request(&scan,sizeof scan,0x103,0x1100);
 struct {Header h;Rect r;uint64_t offset;uint32_t id,pad;} transfer={.r={0,0,w,h},.id=id};
 request(&transfer,sizeof transfer,0x105,0x1100);
 struct {Header h;Rect r;uint32_t id,pad;} flush={.r={0,0,w,h},.id=id};
 request(&flush,sizeof flush,0x104,0x1100);
}
int main(void) {
 machine=calloc(1,sizeof *machine);pthread_mutex_init(&machine->dev_mu,NULL);
 uint8_t *ram=calloc(1,32u<<20);assert(!hv_machine_add_ram(machine,RAM,32u<<20,ram,0));
 HvGic *gic=hv_gicv3_create(machine,0x8000000,0x80a0000);
 HvPcie *bus=hv_pcie_create(machine,gic,ECAM,0x10000000,0x10000000,0x1000000,35);
 HvFbDisplay *fb=hv_fb_display_create(machine,NULL);
 HvVirtioGpu *gpu=hv_virtio_gpu_create(machine,bus,fb);assert(gpu);
 HvVirtio *v=hv_virtio_gpu_device(gpu);
 uint64_t config=ECAM+((uint64_t)v->pci.slot<<15);
 assert(rd(config)==0x10501af4 && rd(config+8)==0x03800001);
 wr(config+0x20,BAR);uint16_t command=2;assert(!hv_phys_write(machine,config+4,&command,2));
 wr(BAR+0x14,3);wr(BAR+8,1);wr(BAR+12,1);wr(BAR+8,0);wr(BAR+12,2);wr(BAR+0x14,11);
 wr(BAR+0x16,0);wr(BAR+0x18,8);address(0x20,DESC);address(0x28,AVAIL);address(0x30,USED);wr(BAR+0x1c,1);wr(BAR+0x14,15);
 Header info={0};const uint8_t *out=request(&info,sizeof info,0x100,0x1101);
 assert(*(uint32_t *)(out+24+8)==1600 && *(uint32_t *)(out+24+12)==1200);
 struct {Header h;uint32_t scanout,pad;} edid={0};out=request(&edid,sizeof edid,0x10a,0x1104);
 assert(*(uint32_t *)(out+24)==128);unsigned sum=0;for(unsigned n=0;n<128;n++)sum+=out[32+n];assert(!(sum&255));
 puts("PASS: standard VirtIO PCI device, split queue, fences, INTx, display info and EDID");
 for(unsigned n=0;n<3;n++) {
  uint32_t sizes[][2]={{1280,960},{1600,1200},{2048,1536}};
  mode(n+1,sizes[n][0],sizes[n][1]);
  HvFbSnapshot frame;assert(!hv_fb_display_acquire_frame(fb,&frame));
  assert(frame.width==sizes[n][0] && frame.height==sizes[n][1] && frame.surface);
  assert(frame.pixels[0]==0x32 && frame.pixels[4095]==0x32 && frame.pixels[4096]==0x78 && frame.pixels[(size_t)frame.stride*frame.height-1]==0x78);
  struct {Header h;uint32_t id,pad;} unref={.id=n+1};request(&unref,sizeof unref,0x102,0x1100);
  assert(frame.pixels[4096]==0x78);hv_fb_snapshot_release(&frame);
 }
 puts("PASS: Windows driver create/attach/set-scanout/transfer/flush sequence; 1280x960 -> 1600x1200 -> 2048x1536");
 puts("PASS: dirty rectangle scatter-page transfer and scanout lifetime across guest resource destruction");
 mode(9,1280,960);
 HvFbSnapshot frame;assert(!hv_fb_display_acquire_frame(fb,&frame));uint64_t revision=frame.revision;
 uint8_t *rest=hv_phys_ptr(machine,RAM+0x600000,1280*960*4-4096);rest[4100]=0xab;
 struct {Header h;Rect r;uint64_t offset;uint32_t id,pad;} dirty={.r={769,1,1,1},.offset=8196,.id=9};
 request(&dirty,sizeof dirty,0x105,0x1100);
 struct {Header h;Rect r;uint32_t id,pad;} flush={.r={769,1,1,1},.id=9};request(&flush,sizeof flush,0x104,0x1100);
 HvFbSnapshot next;assert(!hv_fb_display_acquire_frame(fb,&next));assert(next.revision>revision && next.pixels[8196]==0xab);hv_fb_snapshot_release(&next);
 HvVirtioGpuStats stats;hv_virtio_gpu_stats(gpu,&stats);assert(stats.width==1280 && stats.height==960 && stats.flushes==5 && stats.errors==0);
 wr(BAR+0x14,0);assert(frame.pixels[8196]==0xab);hv_fb_snapshot_release(&frame);
 puts("PASS: one-pixel flush triggers refresh without sampled hashing; normal VirtIO reset releases resources safely");
 /* Exercise real ECAM, MSI-X table/PBA MMIO and a completed split queue,
    including the exact vector numbers requested by Windows viogpudo. */
 assert(v->config_msix_vector==0xffff && v->q[0].msix_vector==0xffff);
 assert(v->pci.config[0x34]==0x90 && rd(config+0x90)==0x00024011);
 assert(!hv_machine_add_mmio(machine,DOORBELL,4,NULL,NULL,msi_write,"test MSI target"));
 wr(config+0x10,MSIX);
 for(unsigned n=0;n<3;n++) {wr(MSIX+n*16,(uint32_t)DOORBELL);wr(MSIX+n*16+8,0x90+n);wr(MSIX+n*16+12,0);}
 wr(config+0x90,0x80024011); /* Enable MSI-X, table size remains 3. */
 wr(BAR+0x10,0);assert((rd(BAR+0x10)&0xffff)==0);
 wr(BAR+0x16,0);wr(BAR+0x1a,1);assert((rd(BAR+0x1a)&0xffff)==1);
 wr(BAR+0x16,1);wr(BAR+0x1a,2);assert((rd(BAR+0x1a)&0xffff)==2);
 hv_virtio_queue_interrupt(v,1);assert(messages==1 && message==0x92 && !v->pci.intx);
 wr(BAR+0x16,0);wr(BAR+0x18,8);address(0x20,DESC);address(0x28,AVAIL);address(0x30,USED);wr(BAR+0x1c,1);wr(BAR+0x14,15);
 memset(hv_phys_ptr(machine,AVAIL,64),0,64);memset(hv_phys_ptr(machine,USED,80),0,80);sequence=0;using_msix=1;
 request(&info,sizeof info,0x100,0x1101);assert(messages==2 && message==0x91 && !v->pci.intx);
 wr(MSIX+16+12,1);request(&info,sizeof info,0x100,0x1101);
 assert(messages==2 && rd(MSIX+0x800)==2);wr(MSIX+16+12,0);
 assert(messages==3 && message==0x91 && rd(MSIX+0x800)==0);
 wr(config+0x90,0xc0024011);request(&info,sizeof info,0x100,0x1101);
 assert(messages==3 && rd(MSIX+0x800)==2);wr(config+0x90,0x80024011);assert(messages==4);
 wr(BAR+0x1a,3);assert((rd(BAR+0x1a)&0xffff)==0xffff);
 request(&info,sizeof info,0x100,0x1101);assert(messages==4 && !v->pci.intx);
 wr(BAR+0x14,0);assert(v->q[0].msix_vector==0xffff && v->config_msix_vector==0xffff);
 puts("PASS: Windows MSI-X vector readback, control/cursor completion, vector/function masking, pending replay, NO_VECTOR and reset");
 hv_virtio_gpu_destroy(gpu);
 puts("No iPad or Windows session was launched by this test.");return 0;
}
