/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_aperture.h"
#include "mmu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define BASE UINT64_C(0x8000000000)
static void *page(uint8_t value) {
    void *p=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    assert(p!=MAP_FAILED);memset(p,value,16384);return p;
}
int main(void) {
    HvMachine *m=calloc(1,sizeof *m);HvCpu *c=calloc(1,sizeof *c);
    assert(!hv_machine_init(m,1<<20));hv_cpu_reset(c,m);
    HvGpuAperture *a=hv_gpu_aperture_create(m,1<<20);assert(a);
    hv_gpu_aperture_relocate(a,BASE);
    void *first=page(0x11),*second=page(0x22);
    assert(!hv_gpu_aperture_map(a,1,4096,first,4096));
    assert(!hv_gpu_aperture_map(a,2,8192,second,4096));
    assert(hv_phys_ptr(m,BASE+4096,4096)==first);
    assert(!hv_phys_ptr(m,BASE+4096,8192));
    uint8_t bytes[8];assert(!hv_phys_read(m,BASE+8190,bytes,8));
    assert(bytes[0]==0x11 && bytes[1]==0x11 && bytes[2]==0x22 && bytes[7]==0x22);
    memset(bytes,0x5a,8);assert(!hv_phys_write(m,BASE+8190,bytes,8));
    assert(((uint8_t *)first)[4094]==0x5a && ((uint8_t *)second)[0]==0x5a);
    assert(!hv_tlb_refill(c,BASE+4100,0));
    HvTlbEntry *e=hv_tlb_ent(c,((BASE+4096)>>HV_TLB_PAGE_BITS)&(HV_TLB_SIZE-1),0);
    assert(e->read_tag!=HV_TLB_INVALID && e->write_tag!=HV_TLB_INVALID && e->exec_tag==HV_TLB_INVALID);
    assert((uint8_t *)(uintptr_t)(BASE+4100+e->addend)==(uint8_t *)first+4);
    assert(!hv_gpu_aperture_unmap(a,1));assert(!hv_gpu_aperture_ready(a));
    assert(((uint8_t *)first)[0]==0x11); /* live TLB still owns retired memory */
    hv_cpu_process_requests(c);assert(hv_gpu_aperture_ready(a));hv_gpu_aperture_poll(a);
    assert(e->read_tag==HV_TLB_INVALID && !hv_phys_ptr(m,BASE+4096,1));
    void *replacement=page(0x33);assert(!hv_gpu_aperture_map(a,3,4096,replacement,4096));
    hv_cpu_process_requests(c);c->pc_state->last_refill_key=0;assert(!hv_tlb_refill(c,BASE+4100,0));
    assert((uint8_t *)(uintptr_t)(BASE+4100+e->addend)==(uint8_t *)replacement+4);
    hv_gpu_aperture_relocate(a,BASE+(1<<20));hv_cpu_process_requests(c);
    assert(!hv_phys_ptr(m,BASE+4096,1) && hv_phys_ptr(m,BASE+(1<<20)+4096,4096)==replacement);
    hv_gpu_aperture_destroy(a);hv_machine_free(m);free(c);free(m);
    puts("PASS: adjacent guest 4KB resources independently alias host 16KB pages; scatter access, direct TLB addends, non-executable mappings, deferred unmap, replacement and PCI relocation");
    return 0;
}
