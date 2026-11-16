/* SPDX-License-Identifier: GPL-2.0-or-later
 * VirtIO GPU with the existing 2D viogpudo path and optional Neptune 3D transport.
 * Dirty rectangles are gathered once from guest scatter pages into a persistent
 * Metal-shareable surface. No custom guest probe or test signing is required.
 * 3D capabilities are advertised only with the native Neptune/DXMT backend.
 */
#include "virtio_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HV_NEPTUNE
#include "neptune_runtime.h"
#include "gpu_aperture.h"
#include <sys/mman.h>
#endif

#define MAX_RESOURCES 4096
#define MAX_BACKING 16384
#define MAX_BYTES (64u << 20)
#define TOTAL_BYTES (128u << 20)
enum { GET_INFO=0x100, CREATE=0x101, UNREF=0x102, SET_SCANOUT=0x103,
       FLUSH=0x104, TRANSFER=0x105, ATTACH=0x106, DETACH=0x107, GET_EDID=0x10a,
       GET_CAPSET_INFO=0x108, GET_CAPSET=0x109, CREATE_BLOB=0x10c, SET_SCANOUT_BLOB=0x10d,
       CTX_CREATE=0x200, CTX_DESTROY=0x201, CTX_ATTACH=0x202, CTX_DETACH=0x203,
       SUBMIT_3D=0x207, MAP_BLOB=0x208, UNMAP_BLOB=0x209 };
enum { OK=0x1100, OK_INFO=0x1101, OK_EDID=0x1104, OK_CAPSET_INFO=0x1102, OK_CAPSET=0x1103, OK_MAP=0x1106,
       ERR=0x1200, ERR_MEMORY=0x1201, ERR_SCANOUT=0x1202, ERR_RESOURCE=0x1203, ERR_PARAM=0x1205 };
typedef struct Header { uint32_t type, flags; uint64_t fence; uint32_t context, padding; } Header;
typedef struct Rect { uint32_t x,y,w,h; } Rect;
typedef struct Entry { uint64_t address; uint32_t length, padding; } Entry;
typedef struct Resource {
    uint32_t id,w,h;
    HvFbSurface *surface;
    Entry *entries;
    uint32_t count;
    uint64_t backing_bytes;
    uint64_t blob_size;
    uint32_t blob_memory, blob_flags, stride, format;
    uint64_t offset;
} Resource;
#ifdef HV_NEPTUNE
typedef struct Context {uint32_t id;struct Context *next;} Context;
typedef struct Pending {
    HvVirtqElem elem;
    uint8_t *response;
    uint32_t length,context;
    uint64_t token;
    int backend_done;
    struct Pending *next;
} Pending;
#endif
struct HvVirtioGpu {
    HvVirtio vio;
    HvFbDisplay *display;
    Resource resources[MAX_RESOURCES];
    uint64_t resident;
    uint32_t scanout;
    Rect view;
    uint8_t edid[128];
    uint32_t monitor_width, monitor_height;
    pthread_mutex_t lock;
    HvVirtioGpuStats stats;
#ifdef HV_NEPTUNE
    HvNeptune *neptune;
    HvGpuAperture *aperture;
    uint64_t fence_token;
    uint32_t pending_count;
    Pending *pending;
    Context *contexts;
#endif
};
_Static_assert(sizeof(Header)==24 && sizeof(Entry)==16 && sizeof(Rect)==16,"VirtIO GPU wire layout");

static Resource *find(HvVirtioGpu *g, uint32_t id) {
    if (!id) return NULL;
    for (int n=0;n<MAX_RESOURCES;n++) if (g->resources[n].id==id) return &g->resources[n];
    return NULL;
}
static int rectangle(Resource *r, Rect a) {
    return a.w && a.h && a.x<r->w && a.y<r->h && a.w<=r->w-a.x && a.h<=r->h-a.y;
}
static void discard(HvVirtioGpu *g, Resource *r) {
#ifdef HV_NEPTUNE
    if(r->blob_size) {
        hv_gpu_aperture_unmap(g->aperture,r->id);
        hv_neptune_blob_unref(g->neptune,r->id);
    } else
#endif
    g->resident-=(uint64_t)r->w*r->h*4;
    hv_fb_surface_release(r->surface); free(r->entries); memset(r,0,sizeof *r);
}
static void reset(HvVirtio *v) {
    HvVirtioGpu *g=v->dev;
    pthread_mutex_lock(&g->lock);
    hv_fb_display_restore_firmware(g->display);
    g->scanout=0; memset(&g->view,0,sizeof g->view);
    for(int n=0;n<MAX_RESOURCES;n++) if(g->resources[n].id) discard(g,&g->resources[n]);
#ifdef HV_NEPTUNE
    while(g->contexts) {
        Context *c=g->contexts;g->contexts=c->next;
        hv_neptune_context_destroy(g->neptune,c->id);free(c);
    }
    while(g->pending) {Pending *p=g->pending;g->pending=p->next;free(p->response);free(p);}
    g->pending_count=0;g->stats.contexts_3d=0;
#endif
    g->stats.width=g->stats.height=0;
    pthread_mutex_unlock(&g->lock);
}
static void *guest(HvMachine *m,uint64_t pa,uint64_t length) {
    return length?hv_phys_ptr(m,pa,length):NULL;
}
typedef struct BackingCursor { uint32_t index; uint64_t start; } BackingCursor;
static int copy_backing(HvVirtioGpu *g, Resource *r, BackingCursor *cursor, uint64_t offset, uint8_t *out, size_t bytes) {
    for(;cursor->index<r->count && offset-cursor->start>=r->entries[cursor->index].length;cursor->index++)
        cursor->start+=r->entries[cursor->index].length;
    offset-=cursor->start;
    for(uint32_t i=cursor->index;i<r->count && bytes;i++) {
        Entry *e=&r->entries[i];
        size_t take=e->length-(size_t)offset; if(take>bytes) take=bytes;
        const void *source=guest(g->vio.m,e->address+offset,take);
        if(!source) return -1;
        memcpy(out,source,take); out+=take; bytes-=take; offset=0;
    }
    return bytes ? -1:0;
}
static int read_request(HvVirtio *v, const HvVirtqElem *e,void *request,uint32_t length) {
    return hv_virtq_read(v,e,0,request,length)==length;
}
#ifdef HV_NEPTUNE
static Context **context_slot(HvVirtioGpu *g,uint32_t id) {
    Context **c=&g->contexts;while(*c && (*c)->id!=id)c=&(*c)->next;return c;
}
static Resource *empty_resource(HvVirtioGpu *g) {
    for(unsigned i=0;i<MAX_RESOURCES;i++)if(!g->resources[i].id)return &g->resources[i];
    return NULL;
}
static const void *request_span(HvVirtio *v,const HvVirtqElem *e,uint32_t offset,uint32_t bytes) {
    for(int i=0;i<e->nseg;i++)if(!e->seg[i].write) {
        if(offset>=e->seg[i].len) {offset-=e->seg[i].len;continue;}
        if(bytes>e->seg[i].len-offset)return NULL;
        return hv_phys_ptr(v->m,e->seg[i].addr+offset,bytes);
    }
    return NULL;
}
static uint32_t command_3d(HvVirtioGpu *g,const HvVirtqElem *e,const Header *h,uint8_t *out,uint32_t *length) {
    HvVirtio *v=&g->vio;
    switch(h->type) {
    case GET_CAPSET_INFO: {
        struct {Header h;uint32_t index,pad;} req;
        if(!read_request(v,e,&req,sizeof req) || req.index)return ERR_PARAM;
        struct {Header h;uint32_t id,version,size,pad;} *cap=(void *)out;
        cap->id=HV_NEPTUNE_CAPSET;
        if(hv_neptune_caps(g->neptune,&cap->version,NULL,&cap->size))return ERR;
        *length=sizeof *cap;return OK_CAPSET_INFO;
    }
    case GET_CAPSET: {
        struct {Header h;uint32_t id,version;} req;
        uint32_t version=0,size=65536;
        if(!read_request(v,e,&req,sizeof req) || req.id!=HV_NEPTUNE_CAPSET ||
           hv_neptune_caps(g->neptune,&version,NULL,&size) || req.version>version || size>65536)return ERR_PARAM;
        if(hv_neptune_caps(g->neptune,&version,out+sizeof(Header),&size))return ERR;
        *length=sizeof(Header)+size;return OK_CAPSET;
    }
    case CTX_CREATE: {
        struct {Header h;uint32_t name_length,flags;char name[64];} req;
        if(!read_request(v,e,&req,sizeof req) || !h->context || req.name_length>64 || *context_slot(g,h->context))return ERR_PARAM;
        Context *c=calloc(1,sizeof *c);if(!c)return ERR_MEMORY;
        if(hv_neptune_context_create(g->neptune,h->context,req.flags,req.name,req.name_length)) {free(c);return ERR;}
        c->id=h->context;c->next=g->contexts;g->contexts=c;g->stats.contexts_3d++;return OK;
    }
    case CTX_DESTROY: {
        Context **slot=context_slot(g,h->context),*c=*slot;if(!c)return ERR_PARAM;
        hv_neptune_context_destroy(g->neptune,c->id);*slot=c->next;free(c);g->stats.contexts_3d--;
        for(Pending *p=g->pending;p;p=p->next)if(p->context==h->context) {
            ((Header *)p->response)->type=ERR;p->backend_done=1;
        }
        return OK;
    }
    case CTX_ATTACH:case CTX_DETACH: {
        struct {Header h;uint32_t id,pad;} req;
        if(!read_request(v,e,&req,sizeof req) || !*context_slot(g,h->context))return ERR_PARAM;
        Resource *r=find(g,req.id);if(!r || !r->blob_size)return ERR_RESOURCE;
        hv_neptune_attach(g->neptune,h->context,req.id,h->type==CTX_ATTACH);return OK;
    }
    case CREATE_BLOB: {
        struct {Header h;uint32_t id,memory,flags,count;uint64_t blob,size;} req;
        if(!read_request(v,e,&req,sizeof req) || !req.id || find(g,req.id) || !req.size ||
           req.size>(UINT64_C(1)<<30) || req.count>MAX_BACKING || req.memory<1 || req.memory>3 ||
           (req.flags&~7u) || (req.memory!=1 && !*context_slot(g,h->context)))return ERR_PARAM;
        Resource *r=empty_resource(g);if(!r)return ERR_MEMORY;
        struct iovec *iov=req.count?calloc(req.count,sizeof *iov):NULL;
        if(req.count && !iov)return ERR_MEMORY;
        uint64_t total=0;
        for(uint32_t i=0;i<req.count;i++) {
            Entry entry;
            if(hv_virtq_read(v,e,sizeof req+i*sizeof entry,&entry,sizeof entry)!=sizeof entry ||
               entry.padding || !entry.length || total>req.size || entry.length>req.size-total ||
               !(iov[i].iov_base=guest(v->m,entry.address,entry.length))) {free(iov);return ERR_PARAM;}
            iov[i].iov_len=entry.length;total+=entry.length;
        }
        if(req.count && total<req.size) {free(iov);return ERR_PARAM;}
        HvNeptuneBlob b={.resource=req.id,.context=h->context,.memory=req.memory,.flags=req.flags,
            .blob_id=req.blob,.size=req.size,.iov=iov,.iov_count=req.count};
        int rc=hv_neptune_blob_create(g->neptune,&b);free(iov);if(rc)return ERR;
        *r=(Resource){.id=req.id,.blob_size=req.size,.blob_memory=req.memory,.blob_flags=req.flags};g->stats.blob_creates++;return OK;
    }
    case MAP_BLOB: {
        struct {Header h;uint32_t id,pad;uint64_t offset;} req;
        if(!read_request(v,e,&req,sizeof req))return ERR_PARAM;
        Resource *r=find(g,req.id);if(!r || !r->blob_size || !(r->blob_flags&1))return ERR_RESOURCE;
        void *view=NULL;uint64_t bytes=0;uint32_t info=0;
        if(hv_neptune_map_info(g->neptune,req.id,&info) || hv_neptune_map_view(g->neptune,req.id,&view,&bytes))return ERR;
        if(hv_gpu_aperture_map(g->aperture,req.id,req.offset,view,bytes)) {munmap(view,(size_t)bytes);return ERR_PARAM;}
        struct {Header h;uint32_t info,pad;} *response=(void *)out;
        response->info=info;*length=sizeof *response;g->stats.blob_maps++;return OK_MAP;
    }
    case UNMAP_BLOB: {
        struct {Header h;uint32_t id,pad;} req;
        if(!read_request(v,e,&req,sizeof req))return ERR_PARAM;
        Resource *r=find(g,req.id);if(!r || !r->blob_size)return ERR_RESOURCE;
        return hv_gpu_aperture_unmap(g->aperture,req.id)?ERR_PARAM:OK;
    }
    case SUBMIT_3D: {
        struct {Header h;uint32_t bytes,pad;} req;
        if(!read_request(v,e,&req,sizeof req) || !*context_slot(g,h->context) ||
           !req.bytes || req.bytes>(16u<<20) || (req.bytes&3))return ERR_PARAM;
        const void *commands=request_span(v,e,sizeof req,req.bytes);void *copy=NULL;
        if(!commands || ((uintptr_t)commands&7)) {
            copy=malloc(req.bytes);if(!copy)return ERR_MEMORY;
            if(hv_virtq_read(v,e,sizeof req,copy,req.bytes)!=req.bytes) {free(copy);return ERR_PARAM;}
            commands=copy;
        }
        int rc=hv_neptune_submit(g->neptune,h->context,commands,req.bytes);free(copy);if(!rc)g->stats.submissions_3d++;return rc?ERR:OK;
    }
    case SET_SCANOUT_BLOB: {
        struct {Header h;Rect rect;uint32_t scanout,id,w,height,format,pad,stride[4],offset[4];} req;
        _Static_assert(sizeof req==96,"scanout blob wire size");
        if(!read_request(v,e,&req,sizeof req))return ERR_PARAM;
        if(req.scanout)return ERR_SCANOUT;
        if(!req.id) {g->scanout=0;hv_fb_display_restore_firmware(g->display);return OK;}
        Resource *r=find(g,req.id);if(!r || !r->blob_size)return ERR_RESOURCE;
        if(!req.w || !req.height || req.w>8192 || req.height>8192 || !req.rect.w || !req.rect.h ||
           req.rect.x>=req.w || req.rect.y>=req.height || req.rect.w>req.w-req.rect.x || req.rect.h>req.height-req.rect.y ||
           (req.format!=1 && req.format!=2 && req.format!=67 && req.format!=68) || req.stride[0]<(uint64_t)req.w*4)return ERR_PARAM;
        uint64_t offset=(uint64_t)req.offset[0]+(uint64_t)req.rect.y*req.stride[0]+(uint64_t)req.rect.x*4;
        int rgba=req.format==67 || req.format==68;
        if(hv_neptune_present(g->neptune,g->display,req.id,req.rect.w,req.rect.h,req.stride[0],offset,rgba))return ERR;
        r->w=req.w;r->h=req.height;r->stride=req.stride[0];r->offset=offset;r->format=rgba;
        g->scanout=r->id;g->view=req.rect;g->stats.width=req.rect.w;g->stats.height=req.rect.h;return OK;
    }
    default:return 0;
    }
}
static void aperture_config(HvPciDev *pci) {
    HvVirtio *v=pci->opaque;HvVirtioGpu *g=v->dev;
    uint64_t base=hv_pci_bar_addr(pci,2),size=pci->bar_size[2];HvPcie *bus=pci->bus;
    int in_high=bus && base>=bus->high_mmio_base && base-bus->high_mmio_base<bus->high_mmio_size &&
        size<=bus->high_mmio_size-(base-bus->high_mmio_base);
    int in_low=bus && base>=bus->mmio_base && base-bus->mmio_base<bus->mmio_size &&
        size<=bus->mmio_size-(base-bus->mmio_base);
    hv_gpu_aperture_relocate(g->aperture,hv_pci_mem_enabled(pci) && (in_high || in_low)?base:0);
}
#endif
static uint32_t command(HvVirtioGpu *g,const HvVirtqElem *e,const Header *h,uint8_t *response,uint32_t *length) {
    HvVirtio *v=&g->vio;
#ifdef HV_NEPTUNE
    if(h->flags&~3u)return ERR_PARAM;
    if(g->neptune) {
        uint32_t result=command_3d(g,e,h,response,length);
        if(result)return result;
    } else
#endif
    if(h->context || (h->flags&~1u)) return ERR_PARAM;
    switch(h->type) {
    case GET_INFO: {
        struct Info { Header h; struct { Rect r; uint32_t enabled,flags; } heads[16]; } *info=(void *)response;
        info->heads[0].r=(Rect){0,0,g->monitor_width,g->monitor_height}; info->heads[0].enabled=1;
        *length=sizeof *info; return OK_INFO;
    }
    case GET_EDID: {
        struct Request { Header h; uint32_t scanout,padding; } req;
        if(!read_request(v,e,&req,sizeof req)) return ERR_PARAM;
        if(req.scanout) return ERR_SCANOUT;
        struct Edid { Header h; uint32_t size,padding; uint8_t data[1024]; } *edid=(void *)response;
        edid->size=sizeof g->edid; memcpy(edid->data,g->edid,sizeof g->edid);
        *length=sizeof *edid; return OK_EDID;
    }
    case CREATE: {
        struct Request { Header h; uint32_t id,format,w,hgt; } req;
        if(!read_request(v,e,&req,sizeof req) || !req.id || find(g,req.id) ||
           !req.w || !req.hgt || req.w>4096 || req.hgt>4096 || (req.format!=1 && req.format!=2)) return ERR_PARAM;
        uint64_t bytes=(uint64_t)req.w*req.hgt*4;
        if(bytes>MAX_BYTES || bytes>TOTAL_BYTES-g->resident) return ERR_MEMORY;
        Resource *r=NULL;
        for(int n=0;n<MAX_RESOURCES;n++) if(!g->resources[n].id) {r=&g->resources[n];break;}
        if(!r) return ERR_MEMORY;
        HvFbSurface *surface=hv_fb_surface_create((size_t)bytes); if(!surface) return ERR_MEMORY;
        *r=(Resource){.id=req.id,.w=req.w,.h=req.hgt,.surface=surface}; g->resident+=bytes;
        return OK;
    }
    case UNREF:
    case DETACH: {
        struct Request { Header h; uint32_t id,padding; } req;
        if(!read_request(v,e,&req,sizeof req)) return ERR_PARAM;
        Resource *r=find(g,req.id); if(!r) return ERR_RESOURCE;
        if(h->type==DETACH && r->blob_size)return ERR_PARAM;
        if(h->type==DETACH) {free(r->entries);r->entries=NULL;r->count=0;r->backing_bytes=0;}
        else {
            if(g->scanout==r->id) { g->scanout=0; hv_fb_display_restore_firmware(g->display); }
            discard(g,r);
        }
        return OK;
    }
    case ATTACH: {
        struct Request { Header h; uint32_t id,count; } req;
        if(!read_request(v,e,&req,sizeof req)) return ERR_PARAM;
        Resource *r=find(g,req.id); if(!r) return ERR_RESOURCE;
        if(!req.count || req.count>MAX_BACKING || r->entries) return ERR_PARAM;
        Entry *entries=calloc(req.count,sizeof *entries); if(!entries) return ERR_MEMORY;
        uint64_t total=0;
        for(uint32_t n=0;n<req.count;n++) {
            Entry *entry=&entries[n];
            if(hv_virtq_read(v,e,sizeof req+n*sizeof *entry,entry,sizeof *entry)!=sizeof *entry ||
               entry->padding || !guest(v->m,entry->address,entry->length) || entry->length>MAX_BYTES-total) {
                free(entries);return ERR_PARAM;
            }
            total+=entry->length;
        }
        if(total<(uint64_t)r->w*r->h*4) {free(entries);return ERR_PARAM;}
        r->entries=entries;r->count=req.count;r->backing_bytes=total;return OK;
    }
    case TRANSFER: {
        struct Request { Header h; Rect rect; uint64_t offset; uint32_t id,padding; } req;
        if(!read_request(v,e,&req,sizeof req)) return ERR_PARAM;
        Resource *r=find(g,req.id); if(!r) return ERR_RESOURCE;
        if(r->blob_size || !r->entries || !rectangle(r,req.rect)) return ERR_PARAM;
        uint64_t stride=(uint64_t)r->w*4, bytes=(uint64_t)(req.rect.h-1)*stride+(uint64_t)req.rect.w*4;
        if(req.offset>r->backing_bytes || bytes>r->backing_bytes-req.offset) return ERR_PARAM;
        /* Validate the complete current backing before any partial update. */
        for(uint32_t n=0;n<r->count;n++) if(!guest(v->m,r->entries[n].address,r->entries[n].length)) return ERR_PARAM;
        uint8_t *pixels=hv_fb_surface_pixels(r->surface)+(size_t)req.rect.y*stride+req.rect.x*4;
        BackingCursor cursor={0};
        for(uint32_t y=0;y<req.rect.h;y++) if(copy_backing(g,r,&cursor,req.offset+y*stride,pixels+y*stride,(size_t)req.rect.w*4)) return ERR_PARAM;
        g->stats.transfers++;g->stats.transferred_bytes+=(uint64_t)req.rect.w*req.rect.h*4;return OK;
    }
    case SET_SCANOUT: {
        struct Request { Header h; Rect rect; uint32_t scanout,id; } req;
        if(!read_request(v,e,&req,sizeof req)) return ERR_PARAM;
        if(req.scanout) return ERR_SCANOUT;
        if(!req.id) {g->scanout=0;hv_fb_display_restore_firmware(g->display);return OK;}
        Resource *r=find(g,req.id); if(!r) return ERR_RESOURCE;
        if(r->blob_size || !rectangle(r,req.rect) || hv_fb_display_set_surface(g->display,r->surface,
             ((size_t)req.rect.y*r->w+req.rect.x)*4,req.rect.w,req.rect.h,r->w*4)) return ERR_PARAM;
        g->scanout=r->id;g->view=req.rect;g->stats.width=req.rect.w;g->stats.height=req.rect.h;
        fprintf(stderr,"winvm: VirtIO Windows display mode %ux%u resource=%u\n",req.rect.w,req.rect.h,req.id);
        return OK;
    }
    case FLUSH: {
        struct Request { Header h; Rect rect; uint32_t id,padding; } req;
        if(!read_request(v,e,&req,sizeof req)) return ERR_PARAM;
        Resource *r=find(g,req.id);if(!r) return ERR_RESOURCE;
        if(!rectangle(r,req.rect)) return ERR_PARAM;
#ifdef HV_NEPTUNE
        if(g->scanout==r->id && r->blob_size) {
            if(hv_neptune_present(g->neptune,g->display,r->id,g->view.w,g->view.h,r->stride,r->offset,r->format))return ERR;
        } else
#endif
        if(g->scanout==r->id) hv_fb_display_dirty(g->display);
        g->stats.flushes++;return OK;
    }
    default:return ERR;
    }
}
#ifdef HV_NEPTUNE
static void poll_locked(HvVirtioGpu *g) {
    if(!g->neptune)return;
    hv_gpu_aperture_poll(g->aperture);
    HvNeptuneFence f;
    int rc;
    while((rc=hv_neptune_pop_fence(g->neptune,&f))>0)
        for(Pending *p=g->pending;p;p=p->next)
            if(p->token==f.value && p->context==f.context)p->backend_done=1;
    if(rc<0) {g->stats.errors++;g->vio.status|=128;return;}
    if(!hv_gpu_aperture_ready(g->aperture))return;
    int any=0;Pending **slot=&g->pending;
    while(*slot) {
        Pending *p=*slot;if(!p->backend_done) {slot=&p->next;continue;}
        uint32_t written=hv_virtq_write(&g->vio,&p->elem,0,p->response,p->length);
        hv_virtq_push(&g->vio,0,&p->elem,written);any=1;
        *slot=p->next;g->pending_count--;g->stats.retired_fences++;free(p->response);free(p);
    }
    if(any)hv_virtio_queue_interrupt(&g->vio,0);
}
#endif
void hv_virtio_gpu_poll(HvVirtioGpu *g) {
#ifdef HV_NEPTUNE
    if(!g || !g->neptune)return;
    pthread_mutex_lock(&g->lock);poll_locked(g);pthread_mutex_unlock(&g->lock);
#else
    (void)g;
#endif
}
static void notify(HvVirtio *v,int queue) {
    HvVirtioGpu *g=v->dev;
    HvVirtqElem e;
    int any=0;
    pthread_mutex_lock(&g->lock);
#ifdef HV_NEPTUNE
    poll_locked(g);
#endif
    while(hv_virtq_pop(v,queue,&e)>0) {
        if(queue==1) { hv_virtq_push(v,queue,&e,0);any=1;continue; }
        uint8_t response[65560]={0}; Header h={0};
        uint32_t length=sizeof(Header),type=ERR_PARAM;
        if(read_request(v,&e,&h,sizeof h) && hv_virtq_writable(&e)>=sizeof(Header)) {
            type=command(g,&e,&h,response,&length); g->stats.commands++;
        }
        if(type>=ERR) {g->stats.errors++;length=sizeof(Header);}
        Header *out=(Header *)response;out->type=type;out->flags=h.flags&3;out->fence=(h.flags&1)?h.fence:0;
        out->context=h.context;out->padding=(h.flags&2)?h.padding&255:0;
        if(hv_virtq_writable(&e)<length) {out->type=ERR_PARAM;length=sizeof(Header);}
#ifdef HV_NEPTUNE
        if(g->neptune && type<ERR && ((h.context && (h.flags&1) && h.type!=CTX_DESTROY) || !hv_gpu_aperture_ready(g->aperture))) {
            Pending *p=g->pending_count<HV_VIRTQ_MAX_SIZE?calloc(1,sizeof *p):NULL;
            if(p)p->response=malloc(length);
            if(!p || !p->response) {
                if(p)free(p);out->type=ERR_MEMORY;length=sizeof(Header);
            } else {
                p->elem=e;p->length=length;p->context=h.context;
                p->backend_done=!(h.context && (h.flags&1) && h.type!=CTX_DESTROY);
                p->token=++g->fence_token;memcpy(p->response,response,length);
                p->next=g->pending;g->pending=p;g->pending_count++;
                if(!p->backend_done && hv_neptune_fence(g->neptune,h.context,(h.flags&2)?h.padding&255:0,p->token)) {
                    ((Header *)p->response)->type=ERR;p->backend_done=1;g->stats.errors++;
                }
                continue;
            }
        }
#endif
        uint32_t written=hv_virtq_write(v,&e,0,response,length);
        hv_virtq_push(v,queue,&e,written);any=1;
    }
#ifdef HV_NEPTUNE
    poll_locked(g);
#endif
    pthread_mutex_unlock(&g->lock);
    if(any) hv_virtio_queue_interrupt(v,queue);
}
static void timing(uint8_t *d,unsigned w,unsigned h,unsigned blankw,unsigned blankh) {
    unsigned clock=((w+blankw)*(h+blankh)*60+5000)/10000;
    d[0]=clock;d[1]=clock>>8;d[2]=w;d[3]=blankw;d[4]=((w>>8)<<4)|(blankw>>8);
    d[5]=h;d[6]=blankh;d[7]=((h>>8)<<4)|(blankh>>8);
    d[8]=48;d[9]=32;d[10]=(3<<4)|6;d[17]=0x1e;
}
static void edid(uint8_t *d,uint32_t width,uint32_t height,int native_panel) {
    const uint8_t prefix[]={0,255,255,255,255,255,255,0,0x5d,0x24,1,0,1,0,0,0,1,36,1,4,0x80,30,23,120,0x0a};
    memcpy(d,prefix,sizeof prefix);d[35]=0x21;d[36]=0x08;
    memset(d+38,1,16);
    d[38]=0x81;d[39]=0x40; /* 1280x960 @ 60 */
    d[40]=0xa9;d[41]=0x40; /* 1600x1200 @ 60 */
    d[42]=0xd1;d[43]=0x00; /* 1920x1200 @ 60 */
    timing(d+54,1600,1200,160,35);
    timing(d+72,2048,1536,160,40);
    timing(d+90,1920,1080,280,45);
    d[111]=0xfc;memcpy(d+113,"WindowsiOS\n  ",13);
    if(native_panel) {
        /* Advertise the actual panel geometry instead of scaling a 4:3
           desktop. A distinct monitor identity lets Windows select its native
           preferred timing rather than reuse the old 1600x1200 monitor mode. */
        d[10]=0x04;d[11]=0x13;
        uint32_t serial=(width<<16)|height;memcpy(d+12,&serial,4);
        d[21]=width>height?23:16;d[22]=width>height?16:23;
        d[35]=d[36]=d[37]=0;memset(d+38,1,16);
        memset(d+54,0,54);
        timing(d+54,width,height,160,35);
        timing(d+72,width*2/3,height*2/3,160,35);
        d[93]=0x10; /* Unused descriptor. */
        memcpy(d+113,"iPad Pro M1\n ",13);
    }
    unsigned sum=0;for(int n=0;n<127;n++)sum+=d[n];d[127]=(uint8_t)-sum;
}
HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m,HvPcie *bus,HvFbDisplay *display) {
    HvVirtioGpu *g=calloc(1,sizeof *g);if(!g)return NULL;
    pthread_mutex_init(&g->lock,NULL);g->display=display;
    g->monitor_width=1600;g->monitor_height=1200;
    const char *panel_w=getenv("HV_DISPLAY_WIDTH"),*panel_h=getenv("HV_DISPLAY_HEIGHT");
    unsigned long width=panel_w?strtoul(panel_w,NULL,10):0,height=panel_h?strtoul(panel_h,NULL,10):0;
    int native_panel=width>=640 && width<=4095 && height>=640 && height<=4095;
    if(native_panel) {g->monitor_width=(uint32_t)width;g->monitor_height=(uint32_t)height;}
    uint64_t features=1u<<1;
#ifdef HV_NEPTUNE
    const char *backend=getenv("HV_NEPTUNE_DXMT");
    if(backend && *backend) {
        char error[256]={0};
        g->neptune=hv_neptune_create(backend,error,sizeof error);
        if(g->neptune)g->aperture=hv_gpu_aperture_create(m,UINT64_C(512)<<20);
        if(!g->neptune || !g->aperture) {
            fprintf(stderr,"winvm: 3D backend unavailable: %s\n",error);
            hv_neptune_destroy(g->neptune);g->neptune=NULL;
        } else features|=(1u<<0)|(1u<<3)|(1u<<4);
    }
#endif
    hv_virtio_init(&g->vio,m,16,0x038000,2,features);
    g->vio.pci.config[0x2e]=0x00;g->vio.pci.config[0x2f]=0x11; /* Official viogpudo exact subsystem ID. */
    /* Match the stock driver's config/control/cursor vector layout. BAR0 is
       the MSI-X table, not a framebuffer or a claim of 3D acceleration. */
    hv_pci_add_bar(&g->vio.pci,0,0x1000,0);
    hv_pci_add_msix(&g->vio.pci,0x90,0,0,0x800,3);
    g->vio.dev=g;g->vio.notify=notify;g->vio.reset=reset;
#ifdef HV_NEPTUNE
    if(g->neptune) {
        hv_pci_add_bar_flags(&g->vio.pci,2,UINT64_C(512)<<20,1,1);
        /* VirtIO PCI shared-memory capability, region ID 1 = host visible. */
        uint8_t *cap=g->vio.pci.config+0xa0;
        cap[0]=9;cap[1]=g->vio.pci.config[0x34];cap[2]=24;cap[3]=8;cap[4]=2;cap[5]=1;
        uint32_t bytes=512u<<20;memcpy(cap+12,&bytes,4);
        g->vio.pci.config[0x34]=0xa0;g->vio.pci.on_config_write=aperture_config;
        uint32_t caps=1;memcpy(g->vio.config+12,&caps,4);
    }
#endif
    uint32_t outputs=1;memcpy(g->vio.config+8,&outputs,4);g->vio.config_len=16;
    edid(g->edid,g->monitor_width,g->monitor_height,native_panel);
    if(hv_pcie_attach(bus,&g->vio.pci)<0) {hv_virtio_gpu_destroy(g);return NULL;}
    return g;
}
HvVirtio *hv_virtio_gpu_device(HvVirtioGpu *g) {return g?&g->vio:NULL;}
void hv_virtio_gpu_stats(HvVirtioGpu *g,HvVirtioGpuStats *stats) {
    memset(stats,0,sizeof *stats);if(!g)return;
    /* Common PCI registers are guarded by dev_mu, command counters by lock.
       Use the same lock order as the MMIO notify path. */
    pthread_mutex_lock(&g->vio.m->dev_mu);
    pthread_mutex_lock(&g->lock);*stats=g->stats;
#ifdef HV_NEPTUNE
    stats->backend_3d_ready=g->neptune!=NULL;stats->pending_fences=g->pending_count;
#endif
    pthread_mutex_unlock(&g->lock);
    stats->driver_status=g->vio.status;stats->msix_enabled=hv_pci_msix_enabled(&g->vio.pci);
    stats->config_vector=g->vio.config_msix_vector;
    stats->control_vector=g->vio.q[0].msix_vector;stats->cursor_vector=g->vio.q[1].msix_vector;
    stats->control_enabled=g->vio.q[0].enabled;stats->cursor_enabled=g->vio.q[1].enabled;
    pthread_mutex_unlock(&g->vio.m->dev_mu);
}
void hv_virtio_gpu_destroy(HvVirtioGpu *g) {
    if(!g)return;reset(&g->vio);
#ifdef HV_NEPTUNE
    hv_neptune_destroy(g->neptune);hv_gpu_aperture_destroy(g->aperture);
#endif
    pthread_mutex_destroy(&g->lock);free(g);
}
