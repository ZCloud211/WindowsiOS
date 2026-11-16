/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_aperture.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#define PAGE 4096u
#define DISABLED UINT64_C(0xfffff00000000000)
typedef struct Mapping {
    uint32_t resource;
    uint64_t offset,length,tickets[HV_MAX_CPUS];
    void *host;
    struct Mapping *next;
} Mapping;
struct HvGpuAperture {
    HvMachine *machine;
    HvRam *ram;
    uint32_t *owners;
    Mapping *mapped,*retired;
    uint64_t tickets[HV_MAX_CPUS];
};
HvGpuAperture *hv_gpu_aperture_create(HvMachine *m,uint64_t size) {
    if(!m || !size || size%PAGE || size>(UINT64_C(8)<<30))return NULL;
    HvGpuAperture *a=calloc(1,sizeof *a);if(!a)return NULL;
    a->owners=calloc(size/PAGE,sizeof *a->owners);
    uint8_t **pages=calloc(size/PAGE,sizeof *pages);
    if(!a->owners || !pages) {free(a->owners);free(pages);free(a);return NULL;}
    int index=m->nram;HvRam *main_ram=m->main_ram;
    if(hv_machine_add_ram(m,DISABLED,size,NULL,0)) {
        if(m->nram>index) {
            HvRam *r=&m->ram[index];free(r->code_pages);free(r->page_gen);free(r->whint);
            *r=(HvRam){0};m->nram=index;
        }
        m->main_ram=main_ram;free(a->owners);free(pages);free(a);return NULL;
    }
    m->main_ram=main_ram;a->machine=m;a->ram=&m->ram[index];a->ram->page_host=pages;return a;
}
void hv_gpu_aperture_relocate(HvGpuAperture *a,uint64_t pa) {
    if(!a)return;
    if(!pa)pa=DISABLED;
    if(pa%PAGE || pa==hv_ram_base(a->ram))return;
    __atomic_store_n(&a->ram->pa,pa,__ATOMIC_RELEASE);
    hv_machine_memory_changed(a->machine,a->tickets);
}
int hv_gpu_aperture_map(HvGpuAperture *a,uint32_t id,uint64_t off,void *host,uint64_t len) {
    if(!a || !id || !host || host==MAP_FAILED || !len || off%PAGE || len%PAGE ||
       off>=a->ram->size || len>a->ram->size-off || (uintptr_t)host%PAGE)return -EINVAL;
    for(Mapping *m=a->mapped;m;m=m->next)if(m->resource==id)return -EEXIST;
    for(uint64_t p=off/PAGE;p<(off+len)/PAGE;p++)if(a->owners[p])return -EBUSY;
    Mapping *m=calloc(1,sizeof *m);if(!m)return -ENOMEM;
    m->resource=id;m->offset=off;m->length=len;m->host=host;m->next=a->mapped;a->mapped=m;
    for(uint64_t p=off/PAGE;p<(off+len)/PAGE;p++) {
        a->owners[p]=id;
        __atomic_store_n(&a->ram->page_host[p],(uint8_t *)host+p*PAGE-off,__ATOMIC_RELEASE);
    }
    hv_machine_memory_changed(a->machine,a->tickets);return 0;
}
int hv_gpu_aperture_unmap(HvGpuAperture *a,uint32_t id) {
    if(!a)return -EINVAL;
    Mapping **slot=&a->mapped;while(*slot && (*slot)->resource!=id)slot=&(*slot)->next;
    Mapping *m=*slot;if(!m)return -ENOENT;
    *slot=m->next;
    for(uint64_t p=m->offset/PAGE;p<(m->offset+m->length)/PAGE;p++) {
        a->owners[p]=0;__atomic_store_n(&a->ram->page_host[p],NULL,__ATOMIC_RELEASE);
    }
    hv_machine_memory_changed(a->machine,m->tickets);
    for(unsigned i=0;i<HV_MAX_CPUS;i++)a->tickets[i]=m->tickets[i];
    m->next=a->retired;a->retired=m;return 0;
}
void hv_gpu_aperture_poll(HvGpuAperture *a) {
    if(!a)return;
    Mapping **slot=&a->retired;
    while(*slot) {
        Mapping *m=*slot;
        if(!hv_machine_memory_ready(a->machine,m->tickets)) {slot=&m->next;continue;}
        *slot=m->next;munmap(m->host,(size_t)m->length);free(m);
    }
}
int hv_gpu_aperture_ready(HvGpuAperture *a) {return !a || hv_machine_memory_ready(a->machine,a->tickets);}
void hv_gpu_aperture_destroy(HvGpuAperture *a) {
    if(!a)return;
    for(unsigned i=0;i<2;i++) {
        Mapping *m=i?a->retired:a->mapped;
        while(m) {Mapping *next=m->next;munmap(m->host,(size_t)m->length);free(m);m=next;}
    }
    free(a->owners);free(a);
}
