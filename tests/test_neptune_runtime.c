/* SPDX-License-Identifier: GPL-2.0-or-later
 * Real Neptune protocol -> native DXMT -> local Apple GPU. No guest launch.
 * Wire fields follow the pinned upstream npt_protocol_transport/toplevel. */
#include "neptune_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
typedef struct NptHeader {uint32_t type,flags;uint64_t object;uint32_t size,pad;} NptHeader;
static void put32(uint8_t **p,uint32_t v) {memcpy(*p,&v,4);*p+=4;}
static void put64(uint8_t **p,uint64_t v) {memcpy(*p,&v,8);*p+=8;}
int main(int argc,char **argv) {
    assert(argc==2);char error[256]={0};HvNeptune *n=hv_neptune_create(argv[1],error,sizeof error);
    if(!n) {fprintf(stderr,"Neptune init: %s\n",error);return 1;}
    uint32_t version=0,size=0;assert(!hv_neptune_caps(n,&version,NULL,&size) && size);
    assert(!hv_neptune_context_create(n,1,HV_NEPTUNE_CAPSET,"native-test",11));
    HvNeptuneBlob b={.resource=1,.context=1,.memory=2,.flags=1,.size=4096};assert(!hv_neptune_blob_create(n,&b));
    void *view=NULL;uint64_t length=0;assert(!hv_neptune_map_view(n,1,&view,&length) && length==4096);
    memset(view,0xff,(size_t)length);
    _Alignas(8) uint8_t commands[120]={0};
    NptHeader reply={.size=40};memcpy(commands,&reply,sizeof reply);uint8_t *p=commands+24;
    put32(&p,1);put32(&p,0);put32(&p,4096);put32(&p,0);
    NptHeader create={.type=2u<<24,.flags=1,.size=80};memcpy(p,&create,sizeof create);p+=24;
    put64(&p,0);put32(&p,1);put64(&p,0);put32(&p,0x20);put64(&p,0);put32(&p,0);put32(&p,7);
    put64(&p,0x100);put64(&p,0x200);assert(p==commands+sizeof commands);
    assert(!hv_neptune_submit(n,1,commands,sizeof commands));assert(!hv_neptune_fence(n,1,0,123));
    int completed=0;HvNeptuneFence f;
    for(unsigned i=0;i<10000;i++) {
        int rc=hv_neptune_pop_fence(n,&f);assert(rc>=0);
        if(rc) {assert(f.context==1 && f.ring==0 && f.value==123);completed=1;break;}
        usleep(1000);
    }
    assert(completed);
    const uint32_t *result=view;
    fprintf(stderr,"Native D3D11 reply: command=0x%x HRESULT=0x%x level=0x%x\n",result[0],result[1],result[4]);
    assert(result[0]==(2u<<24) && result[1]==0 && result[2] && !result[3] && result[4]>=0xb000);
    hv_neptune_blob_unref(n,1);munmap(view,(size_t)length);hv_neptune_context_destroy(n,1);hv_neptune_destroy(n);
    puts("PASS: real Neptune context, shared blob, FD alias, reply stream, native D3D11 hardware device creation and asynchronous fence completion on the Mac GPU");
    return 0;
}
