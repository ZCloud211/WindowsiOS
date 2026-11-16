/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "neptune_runtime.h"
#include <virglrenderer.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define FENCE_COUNT 4096u
#define RESOURCE_BUCKETS 1024u
typedef struct Blob {
    uint32_t id;
    uint64_t size;
    struct iovec *iov;
    HvFbNativeSurface *surface;
    uint32_t w,h,stride;
    uint64_t offset;
    int rgba;
    struct Blob *next;
} Blob;
struct HvNeptune {
    struct virgl_renderer_callbacks callbacks;
    pthread_mutex_t fence_lock;
    HvNeptuneFence fences[FENCE_COUNT];
    uint32_t fence_head,fence_count;
    int fence_overflow,closing;
    Blob *resources[RESOURCE_BUCKETS];
};
static atomic_flag active=ATOMIC_FLAG_INIT;
static Blob **find_blob(HvNeptune *r,uint32_t id) {
    Blob **slot=&r->resources[id%RESOURCE_BUCKETS];
    while(*slot && (*slot)->id!=id)slot=&(*slot)->next;
    return slot;
}
static void complete(void *cookie,uint32_t context,uint32_t ring,uint64_t value) {
    HvNeptune *r=cookie;
    pthread_mutex_lock(&r->fence_lock);
    if(!r->closing) {
        if(r->fence_count==FENCE_COUNT)r->fence_overflow=1;
        else {r->fences[(r->fence_head+r->fence_count)%FENCE_COUNT]=(HvNeptuneFence){context,ring,value};r->fence_count++;}
    }
    pthread_mutex_unlock(&r->fence_lock);
}
static void complete_legacy(void *cookie,uint32_t value) {complete(cookie,0,0,value);}
HvNeptune *hv_neptune_create(const char *path,char *error,size_t error_size) {
    if(!path || !*path || atomic_flag_test_and_set(&active)) {
        if(error && error_size)snprintf(error,error_size,"Neptune requires a native backend and a single VM runtime.");
        return NULL;
    }
    HvNeptune *r=calloc(1,sizeof *r);
    if(!r) {atomic_flag_clear(&active);return NULL;}
    pthread_mutex_init(&r->fence_lock,NULL);
    setenv("NPT_BACKEND","dxmt",1);
    setenv("NPT_D3D11_LIBRARY_PATH",path,1);setenv("NPT_DXGI_LIBRARY_PATH",path,1);setenv("NPT_D3D12_LIBRARY_PATH",path,1);
    r->callbacks=(struct virgl_renderer_callbacks){.version=4,.write_fence=complete_legacy,.write_context_fence=complete};
    int flags=VIRGL_RENDERER_NO_VIRGL|VIRGL_RENDERER_NEPTUNE|VIRGL_RENDERER_RENDER_SERVER|
        VIRGL_RENDERER_THREAD_SYNC|VIRGL_RENDERER_ASYNC_FENCE_CB|VIRGL_RENDERER_USE_EXTERNAL_BLOB;
    int rc=virgl_renderer_init(r,flags,&r->callbacks);
    if(rc) {
        if(error && error_size)snprintf(error,error_size,"Neptune renderer initialization failed (%d).",rc);
        pthread_mutex_destroy(&r->fence_lock);free(r);atomic_flag_clear(&active);return NULL;
    }
    uint32_t version=0,size=0;virgl_renderer_get_cap_set(HV_NEPTUNE_CAPSET,&version,&size);
    if(!size || size>65536) {
        if(error && error_size)snprintf(error,error_size,"Native backend supplied no Neptune capabilities.");
        hv_neptune_destroy(r);return NULL;
    }
    return r;
}
void hv_neptune_destroy(HvNeptune *r) {
    if(!r)return;
    pthread_mutex_lock(&r->fence_lock);r->closing=1;pthread_mutex_unlock(&r->fence_lock);
    virgl_renderer_cleanup(r);
    for(unsigned n=0;n<RESOURCE_BUCKETS;n++) while(r->resources[n]) {
        Blob *b=r->resources[n];r->resources[n]=b->next;
        hv_fb_native_surface_release(b->surface);free(b->iov);free(b);
    }
    pthread_mutex_destroy(&r->fence_lock);free(r);atomic_flag_clear(&active);
}
int hv_neptune_caps(HvNeptune *r,uint32_t *version,void *data,uint32_t *size) {
    if(!r || !version || !size)return -EINVAL;
    uint32_t required=0;virgl_renderer_get_cap_set(HV_NEPTUNE_CAPSET,version,&required);
    if(!data) {*size=required;return 0;}
    if(*size<required) {*size=required;return -ENOSPC;}
    memset(data,0,required);virgl_renderer_fill_caps(HV_NEPTUNE_CAPSET,*version,data);*size=required;return 0;
}
int hv_neptune_context_create(HvNeptune *r,uint32_t id,uint32_t flags,const char *name,uint32_t length) {
    if(!r || !id || (flags&0xff)!=HV_NEPTUNE_CAPSET || length>64)return -EINVAL;
    return virgl_renderer_context_create_with_flags(id,flags,length,name);
}
void hv_neptune_context_destroy(HvNeptune *r,uint32_t id) {if(r && id)virgl_renderer_context_destroy(id);}
int hv_neptune_submit(HvNeptune *r,uint32_t context,const void *commands,uint32_t bytes) {
    if(!r || !context || !commands || !bytes || (bytes&3) || ((uintptr_t)commands&7) || bytes>(16u<<20))return -EINVAL;
    return virgl_renderer_submit_cmd((void *)commands,(int)context,(int)(bytes/4));
}
int hv_neptune_fence(HvNeptune *r,uint32_t context,uint32_t ring,uint64_t value) {
    if(!r || !context || ring>=64)return -EINVAL;
    return virgl_renderer_context_create_fence(context,0,ring,value);
}
int hv_neptune_pop_fence(HvNeptune *r,HvNeptuneFence *f) {
    if(!r || !f)return -EINVAL;
    pthread_mutex_lock(&r->fence_lock);int rc=0;
    if(r->fence_overflow)rc=-EOVERFLOW;
    else if(r->fence_count) {*f=r->fences[r->fence_head];r->fence_head=(r->fence_head+1)%FENCE_COUNT;r->fence_count--;rc=1;}
    pthread_mutex_unlock(&r->fence_lock);return rc;
}
int hv_neptune_blob_create(HvNeptune *r,const HvNeptuneBlob *d) {
    if(!r || !d || !d->resource || !d->size || d->iov_count>16384 || (d->iov_count && !d->iov))return -EINVAL;
    Blob **slot=find_blob(r,d->resource);if(*slot)return -EEXIST;
    Blob *b=calloc(1,sizeof *b);if(!b)return -ENOMEM;
    if(d->iov_count) {
        b->iov=malloc(sizeof(struct iovec)*d->iov_count);
        if(!b->iov) {free(b);return -ENOMEM;}
        memcpy(b->iov,d->iov,sizeof(struct iovec)*d->iov_count);
    }
    struct virgl_renderer_resource_create_blob_args a={.res_handle=d->resource,.ctx_id=d->context,
        .blob_mem=d->memory,.blob_flags=d->flags,.blob_id=d->blob_id,.size=d->size,.iovecs=b->iov,.num_iovs=d->iov_count};
    int rc=virgl_renderer_resource_create_blob(&a);
    if(rc) {free(b->iov);free(b);return rc;}
    b->id=d->resource;b->size=d->size;*slot=b;return 0;
}
void hv_neptune_blob_unref(HvNeptune *r,uint32_t id) {
    if(!r)return;
    Blob **slot=find_blob(r,id),*b=*slot;if(!b)return;
    *slot=b->next;virgl_renderer_resource_unref(id);
    hv_fb_native_surface_release(b->surface);free(b->iov);free(b);
}
void hv_neptune_attach(HvNeptune *r,uint32_t context,uint32_t resource,int attach) {
    if(!r || !*find_blob(r,resource))return;
    if(attach)virgl_renderer_ctx_attach_resource((int)context,(int)resource);
    else virgl_renderer_ctx_detach_resource((int)context,(int)resource);
}
int hv_neptune_map_fixed(HvNeptune *r,uint32_t id,void *address) {
    return r && *find_blob(r,id)?virgl_renderer_resource_map_fixed(id,address):-EINVAL;
}
int hv_neptune_map_view(HvNeptune *r,uint32_t id,void **address,uint64_t *length) {
    if(!r || !address || !length)return -EINVAL;
    Blob *b=*find_blob(r,id);if(!b || !b->size || b->size%4096 || b->size>SIZE_MAX)return -EINVAL;
    uint32_t type=0;int fd=-1,rc=virgl_renderer_resource_export_blob(id,&type,&fd);
    if(rc)return rc;
    if(type!=VIRGL_RENDERER_BLOB_FD_TYPE_SHM) {close(fd);return -ENOTSUP;}
    void *view=mmap(NULL,(size_t)b->size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(view==MAP_FAILED)return -errno;
    *address=view;*length=b->size;return 0;
}
int hv_neptune_map_info(HvNeptune *r,uint32_t id,uint32_t *info) {
    return r && info && *find_blob(r,id)?virgl_renderer_resource_get_map_info(id,info):-EINVAL;
}
int hv_neptune_present(HvNeptune *r,HvFbDisplay *display,uint32_t id,uint32_t w,uint32_t h,
    uint32_t stride,uint64_t offset,int rgba) {
    if(!r || !display)return -EINVAL;
    Blob *b=*find_blob(r,id);if(!b)return -EINVAL;
    if(!b->surface || b->w!=w || b->h!=h || b->stride!=stride || b->offset!=offset || b->rgba!=rgba) {
        uint32_t type=0;int fd=-1;
        int rc=virgl_renderer_resource_export_blob(id,&type,&fd);
        if(rc)return rc;
        HvFbNativeSurface *surface=NULL;
        if(type==VIRGL_RENDERER_BLOB_FD_TYPE_SHM)
            surface=hv_fb_metal_surface_from_shared_fd(fd,b->size,w,h,stride,offset,rgba);
        close(fd);
        if(!surface)return -ENOTSUP;
        hv_fb_native_surface_release(b->surface);b->surface=surface;
        b->w=w;b->h=h;b->stride=stride;b->offset=offset;b->rgba=rgba;
    }
    return hv_fb_display_set_native_surface(display,b->surface);
}
