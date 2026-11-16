#define _GNU_SOURCE
#include <assert.h>
#include <elf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../src/runtime_race_options.c"
static const char *fixture_path;
static unsigned char clock_data[0x40];
static void *clock_pointer=clock_data;
typedef void (*Op)(void *,void *);
static void *image;
static void setf(void *p,int off,float f){memcpy((char*)p+off,&f,4);}
static void setp(void *p,int off,void *f){memcpy((char*)p+off,&f,8);}

static void *getp(const void *p,int off){void *v;memcpy(&v,(const char*)p+off,8);return v;}
static float getf(const void *p,int off){float v;memcpy(&v,(const char*)p+off,4);return v;}
static uint32_t getu(const void *p,int off){uint32_t v;memcpy(&v,(const char*)p+off,4);return v;}
static void setu(void *p,int off,uint32_t v){memcpy((char*)p+off,&v,4);}
static const size_t kinds[15]={0x24700e0,0x24707c0,0x2470500,0x24604e8,0x246e560,0x245d1f8,0x246e560,0x24612c0,0x246ad00,0x246bd80,0x246bd80,0x24621e0,0x2462340,0x2462340,0x24661c8};
static const size_t offsets[15]={0x2c8,0x338,0x458,0x490,0x4a8,0x4c0,0x4d0,0x4f0,0x520,0x550,0x5c8,0x640,0x7d0,0x910,0xa50};
struct Fixture {
    unsigned char asset[0x3000],ctx[0x400],primitive[0xa0],instance[0x30],bolt[0x100],compiled[0x78],list[0x28],manager[0x1000],stream[0x200],worker_context[0x20];
    void *ops[15];
};
static void fixture(struct Fixture *f){
    memset(f,0,sizeof *f);FILE *in=fopen(fixture_path,"rb");assert(in);size_t n=fread(f->asset,1,sizeof f->asset,in);assert(n>0x1500&&n<sizeof f->asset);fclose(in);
    for(int i=0;i<15;i++){f->ops[i]=f->asset+offsets[i];setp(f->ops[i],0,(char*)image+kinds[i]);}
    /* Original operators, minimal compiled motion metadata; no native render work. */
    setp(f->list,0x20,f->ops);setu(f->list,0xc,15);setp(f->compiled,0x70,f->list);memcpy(f->compiled,f->asset+0xc0,sizeof f->compiled);setp(f->compiled,0x70,f->list);
    setp(f->primitive,0x10,f->compiled);setp(f->primitive,0x68,f->bolt);setp(f->ctx,0,f->ctx+0x10);setf(f->ctx,0x1c,1);
    setp(f->ctx,0x370,f->manager);setp(f->ctx,0x380,f->compiled);setp(f->ctx,0x388,f->primitive);setp(f->ctx,0x390,f->instance);setp(f->ctx,0x398,f->stream);setp(f->ctx,0x3c0,f->worker_context);
    float identity[8]={0,0,0,1,0,0,0,0};memcpy(f->bolt+0x40,identity,32);setf(f->manager,0x3e8,-1);setp(f->ops[14],0x20,f->asset+0xa90);setf(f->ctx,0x3dc,1.f/240);setf(f->manager,0xcc,1.f/240);
}

static float classified_dt(void *op,void *context,void *normal_manager,float raw_dt){
 unsigned char frame[0xd0]={0};setp(frame,0,op);TimestepRegisters r={0};r.rbp=(uintptr_t)(frame+0xc8);r.r15=(uintptr_t)context;r.resume=1000;
 timestep_clock=&clock_pointer;setf(clock_data,0x18,raw_dt);setf(clock_data,0x24,getf(context,0x3dc));vfx_worker_manager=normal_manager;
 vfx_drag_timestep(&r);return r.resume==1009?r.ymm[2][0]:getf(context,0x3dc);
}
static void drag_call(void *op,void *ctx,void *normal_manager,float raw_dt,int corrected){
 uint32_t dt_bits=getu(ctx,0x3dc);timestep_clock=&clock_pointer;setf(clock_data,0x18,raw_dt);setf(clock_data,0x24,getf(ctx,0x3dc));vfx_worker_manager=corrected?normal_manager:NULL;
 ((Op)((char*)image+0x2c5380))(op,ctx);assert(getu(ctx,0x3dc)==dt_bits);
}
static void tail(void **ops,void *ctx){
 const size_t entries[]={0x2c7490,0x2e80a0,0x2e9110,0x2e9110,0x2c9bb0,0x2c9e60,0x2c9e60,0x2ce090};
 unsigned char saved[40];void *stream=getp(ctx,0x398);memcpy(saved,stream,40);
 /* Original tail math and sprite preparation only; never execute sprite draw submission. */
 for(unsigned i=0;i<8;i++){((Op)((char*)image+entries[i]))(ops[7+i],ctx);assert(!memcmp(saved,stream,40));}
}
static void simulation(float *out,int rate,int corrected,float speed,float scale,int varying){
    struct Fixture f;fixture(&f);const float mix[]={1.f/240,1.f/80,1.f/60};
    for(int tick=0;tick<30;tick++){
        float local_scale=scale*(varying?(1+(tick%4)*.125f):1);
        setf(f.bolt,0x90,100+(varying?tick*3:0));setf(f.bolt,0x94,200-(varying?tick*2:0));setf(f.bolt,0x98,-50+(varying?tick:0));
        setf(f.ctx,0x2d0,varying?tick/30.f:0);setf(f.ctx,0x2d4,varying?tick/30.f:0);
        for(int sub=0;sub<(rate?rate/30:3);sub++){
            uint64_t seed=1234+(varying?tick*167:0);memcpy(f.ctx+0x3e0,&seed,8);setp(f.ctx,0,f.ctx+0x10);setu(f.ctx,8,0);
            float raw_dt=rate?1.f/rate:mix[sub],dt=raw_dt*speed*local_scale;setf(f.ctx,0x3dc,dt);setf(f.manager,0xcc,raw_dt*speed);
            ((Op)((char*)image+0x2ee680))(f.ops[0],f.ctx);((Op)((char*)image+0x2ee980))(f.ops[1],f.ctx);((Op)((char*)image+0x2ee830))(f.ops[2],f.ctx);
            uint32_t before=getu(f.ctx,0x3dc);drag_call(f.ops[3],f.ctx,f.manager,raw_dt,corrected);assert(before==getu(f.ctx,0x3dc));
            ((Op)((char*)image+0x2ec940))(f.ops[4],f.ctx);assert(getu(f.ctx,8)==2);
            ((Op)((char*)image+0x2dffb0))(f.ops[5],f.ctx);
            ((Op)((char*)image+0x2ec940))(f.ops[6],f.ctx);assert(getu(f.ctx,8)==0);
            tail(f.ops,f.ctx);
        }
    }
    memcpy(out,f.stream+16,12);
}

void *vfx_block_entry;
uint64_t vfx_register_failures;
const uint64_t vfx_ymm_canary[4]={0x123456789abcdef0,0x02468ace13579bdf,0xfedcba9876543210,0x1122334455667788};
uint64_t vfx_ymm_result[4];
extern void vfx_block_probe(void *,void *,const float *,float *);
extern void vfx_worker_body_stub(void);
__asm__(".text\n"
"vfx_block_probe:\n push %rbp\n push %r15\n sub $0xd8,%rsp\n lea 0xd0(%rsp),%rbp\n"
"mov %rdi,-0xc8(%rbp)\n mov %rsi,%r15\n mov %rcx,-8(%rbp)\n vmovups (%rdx),%xmm0\n"
"mov $0x12345678,%r8d\n mov $0x23456789,%r9d\n mov $0x3456789a,%r10d\n mov $0x456789ab,%r11d\n vmovups vfx_ymm_canary(%rip),%ymm15\n"
"call *vfx_block_entry(%rip)\n vmovups %ymm15,vfx_ymm_result(%rip)\n"
"cmp $0x12345678,%r8d\n jne 1f\n cmp $0x23456789,%r9d\n jne 1f\n cmp $0x3456789a,%r10d\n jne 1f\n cmp $0x456789ab,%r11d\n je 2f\n1: incq vfx_register_failures(%rip)\n2:\n"
"mov -8(%rbp),%rdx\n vmovups -0x70(%rbp),%xmm0\n vmovups %xmm0,(%rdx)\n add $0xd8,%rsp\n pop %r15\n pop %rbp\n ret\n"
"vfx_worker_body_stub:\n call vfx_worker_body_test\n add $0x168,%rsp\n pop %rbx\n pop %r12\n pop %r13\n pop %r14\n pop %r15\n pop %rbp\n ret\n");
static int worker_depth,worker_visits,worker_job;
void vfx_worker_body_test(void *owner,void *job){
    assert(vfx_worker_manager==getp(owner,0)&&job==&worker_job);void *outer=vfx_worker_manager;worker_visits++;
    if(!worker_depth++) {void *inner=&worker_depth;((VfxWorker)((char*)image+0x2b3670))(&inner,job);assert(vfx_worker_manager==outer);}
    worker_depth--;
}
static void emission_tests(unsigned char *code){
    struct Fixture f;fixture(&f);float input[4]={-100,-200,50,0},expected[4],got[4];
    memcpy(code+576,vfx_drag_code,sizeof vfx_drag_code);code[576+sizeof vfx_drag_code]=0xc3;
    unsigned char call=((unsigned char*)image)[0x2c4faa];((unsigned char*)image)[0x2c4faa]=0xc3;
    const float raw[]={1.f/240,1.f/72,1.f/144,1.f/30,273.f/8192,274.f/8192,0,NAN};
    for(unsigned i=0;i<sizeof raw/sizeof *raw;i++)for(unsigned allowed=0;allowed<2;allowed++){
        setf(f.ctx,0x3dc,1.f/240);setf(clock_data,0x18,raw[i]);setf(clock_data,0x24,.0001f);vfx_worker_manager=allowed?f.manager:NULL;
        float reference=allowed&&i<3?(1.f/240)/(raw[i]*30):1.f/240;
        setf(f.ctx,0x3dc,reference);vfx_block_entry=code+576;vfx_block_probe(f.ops[3],f.ctx,input,expected);assert(!memcmp(vfx_ymm_canary,vfx_ymm_result,sizeof vfx_ymm_result));
        setf(f.ctx,0x3dc,1.f/240);vfx_block_entry=(char*)image+0x2c4f98;vfx_block_probe(f.ops[3],f.ctx,input,got);
        assert(!memcmp(expected,got,sizeof got));assert(!memcmp(vfx_ymm_canary,vfx_ymm_result,sizeof vfx_ymm_result));assert(getf(f.ctx,0x3dc)==1.f/240);
    }
    ((unsigned char*)image)[0x2c4faa]=call;assert(!vfx_register_failures);
    /* Execute the real patched entry and relocated native prologue, with a bounded job body. */
    jump((unsigned char*)image+0x2b3684,vfx_worker_body_stub);
    void *previous=&worker_visits,*owner=f.manager;vfx_worker_manager=previous;
    ((VfxWorker)((char*)image+0x2b3670))(&owner,&worker_job);
    assert(worker_visits==2&&worker_depth==0&&vfx_worker_manager==previous);
    puts("native emitted hooks: native/fixed arithmetic, live GPR/YMM, unchanged dt, worker arguments and nested TLS restore pass");
}
static void rejected_tests(void){
    for(int test=0;test<41;test++){
        struct Fixture f;fixture(&f);void *manager=f.manager,*op=f.ops[3];float raw_dt=1.f/240;
        switch(test){
        case 0:manager=NULL;break;case 1:manager=f.primitive;break;case 2:setp(f.ctx,0x3c0,NULL);break; /* native prewarm */
        case 3:setu(f.instance,0x10,8);break;case 4:setu(f.instance,0x10,2);break;case 5:setp(f.ctx,0x390,NULL);break;
        case 6:setu(f.list,0xc,14);break;case 7:setu(f.list,0xc,16);break;
        case 8:{void *tmp=f.ops[0];f.ops[0]=f.ops[1];f.ops[1]=tmp;break;}
        case 9:setu(f.ops[0],0xc,0);break;case 10:setu(f.ops[0],0x10,0);break;
        case 11:setu(f.ops[1],0x14,0);break;case 12:setu(f.ops[1],0x10,0);break;
        case 13:setu(f.ops[2],0x14,0);break;case 14:setu(f.ops[2],0x10,5);break;
        case 15:setu(op,0x10,1);break;case 16:setp(op,0,(char*)image+0x24604e8+8);break;
        case 17:setu(f.ctx,8,1);break;case 18:setp(f.ctx,0,f.ctx+0xc0);break;case 19:f.compiled[0x60]=0;break;
        case 20:setu(f.ops[4],0x10,7);break;case 21:setu(f.ops[6],0x10,7);break;
        case 22:raw_dt=0;break;case 23:raw_dt=-1;break;case 24:raw_dt=NAN;break;case 25:raw_dt=INFINITY;break;case 26:raw_dt=1.f/30;break;case 27:raw_dt=1.f/20;break;
        case 28:setf(f.ctx,0x3dc,0);break;case 29:setf(f.ctx,0x3dc,-1);break;case 30:setf(f.ctx,0x3dc,NAN);break;case 31:setf(f.ctx,0x3dc,INFINITY);break;
        case 32:setp(f.primitive,0x10,NULL);break;case 33:setu(f.ops[14],0xc,0);break;case 34:setp(f.ops[14],0,(char*)image+1);break;
        case 35:setu(f.compiled,0x5c,0xf);break;case 36:setu(f.compiled,0x64,1);break;
        case 37:setu(f.ops[14],0x30,0x10);break;case 38:setp(f.ops[14],0x28,f.asset);break;
        case 39:setu(f.ops[0],0x10,6);break;case 40:setf(op,0x14,-2);break;
        }
        uint32_t bits=getu(f.ctx,0x3dc);float chosen=classified_dt(op,f.ctx,manager,raw_dt);uint32_t chosen_bits;memcpy(&chosen_bits,&chosen,4);assert(chosen_bits==bits);
        /* Execute native Drag for rejected cases whose synthetic context still represents a valid native math path. */
        if(test!=15&&test!=18&&test!=30&&test!=31){
            float initial[3]={100,200,-50},want[3],got[3];memcpy(f.ctx+0x30,initial,12);
            vfx_worker_manager=NULL;((Op)((char*)image+0x2c5380))(op,f.ctx);memcpy(want,f.ctx+0x30,12);memcpy(f.ctx+0x30,initial,12);
            drag_call(op,f.ctx,manager,raw_dt,1);memcpy(got,f.ctx+0x30,12);assert(!memcmp(want,got,12));assert(getu(f.ctx,0x3dc)==bits);
        }
    }
    puts("classifier: 41 rejected contracts; 37 retain native Drag output bitwise");
}
int main(int argc,char **argv){
    assert(argc==3);fixture_path=argv[2];FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);
    unsigned char *elf=malloc(len);assert(fread(elf,1,len,f)==(size_t)len);fclose(f);
    const Elf64_Ehdr *eh=(void*)elf;const Elf64_Phdr *ph=(void*)(elf+eh->e_phoff);size_t size=0;
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD&&ph[i].p_vaddr+ph[i].p_memsz>size)size=ph[i].p_vaddr+ph[i].p_memsz;
    image=mmap(0,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(image!=MAP_FAILED);
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD||ph[i].p_type==0x61000010)memcpy((char*)image+ph[i].p_vaddr,elf+ph[i].p_offset,ph[i].p_filesz);
    float zero[4]={0},identity[8]={0,0,0,1,0,0,0,0};
    memcpy((char*)image+0x2af14f0,zero,sizeof zero);memcpy((char*)image+0x2af1ad0,identity,sizeof identity);
    uint64_t cookie=1234;int32_t disp;memcpy(&disp,(char*)image+0x2c4d47,4);setp(image,0x2c4d4b+disp,&cookie);
    assert(!memcmp((char*)image+0x2c5380,"\xc5\xfa\x10\x47\x14\xe9",6));

    assert(vfx_timestep_contract(image,size));
    const size_t mutation_sites[]={0x2b3670,0x2c4f98,0x2b3ac0,0x2b2673,0x2b2687};
    for(unsigned i=0;i<5;i++){((unsigned char*)image)[mutation_sites[i]]^=1;assert(!vfx_timestep_contract(image,size));((unsigned char*)image)[mutation_sites[i]]^=1;}
    unsigned char *bridges=mmap(NULL,640,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bridges!=MAP_FAILED);memset(bridges,0xcc,640);
    install_vfx_timestep(image,bridges);for(unsigned i=546;i<640;i++)assert(bridges[i]==0xcc);
    setp(image,0x2808198,cosf);setp(image,0x28081a0,sinf);setp(image,0x2808390,fmodf);
    const float scales[]={.125,.5,1,1.4},speeds[]={.125,.5,1,2};const int rates[]={30,60,90,120,240,0};
    unsigned cases=0,exact30=0;double worst=0,fixed=0;
    for(unsigned sc=0;sc<4;sc++)for(unsigned sp=0;sp<4;sp++)for(int changing=0;changing<2;changing++){
        float ref[3];simulation(ref,30,0,speeds[sp],scales[sc],changing);
        for(unsigned r=0;r<6;r++){
            float raw[3],got[3];simulation(raw,rates[r],0,speeds[sp],scales[sc],changing);simulation(got,rates[r],1,speeds[sp],scales[sc],changing);
            for(unsigned axis=0;axis<3;axis++){worst=fmax(worst,fabs(raw[axis]-ref[axis]));fixed=fmax(fixed,fabs(got[axis]-ref[axis]));assert(fabs(got[axis]-ref[axis])<.003);}
            if(rates[r]==30){assert(!memcmp(raw,got,sizeof raw));exact30++;}cases++;
        }
    }
    printf("classified native smoke: %u cases, %u exact30; worst native position gap=%g corrected=%g\n",cases,exact30,worst,fixed);
    rejected_tests();
    emission_tests(bridges);
    munmap(bridges,640);munmap(image,size);free(elf);
}
