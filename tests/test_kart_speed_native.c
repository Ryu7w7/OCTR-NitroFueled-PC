/* Offline native-function fixture, invoked by test_kart_timestep_native.py. */
#define _GNU_SOURCE
#include "../src/runtime_race_options.c"
#include <assert.h>
#include <elf.h>

static void *test_boost;
static void *ABI fixture_boost(void *entity, void *type, int flags) {
    (void)entity; (void)type; (void)flags;
    return test_boost;
}
static void set_float(void *p, size_t at, float value) { memcpy((char *)p+at,&value,4); }
static void set_pointer(void *p, size_t at, void *value) { memcpy((char *)p+at,&value,8); }
static unsigned char test_clock[0x38];
static void *test_clock_pointer=test_clock;
static void set_frame_dt(float dt) {
    set_float(test_clock,0x18,dt);
    timestep_clock=&test_clock_pointer;
}

extern void rotation_probe(void *, float *, float *, float *);
__asm__(".text\n.globl rotation_probe\nrotation_probe:\n"
    "push %rbp\nmov %rsp,%rbp\nsub $0x280,%rsp\n"
    "vmovups .Lrotation_pattern(%rip),%ymm0\n"
    ".irp n,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
    "vmovaps %ymm0,%ymm\\n\n.endr\n"
    "mov %rsi,-0x158(%rbp)\nmovups (%rcx),%xmm6\nmov %rdx,%rcx\n"
    "movabs $0x3f8000003f800000,%rax\nmovq %rax,%xmm7\npunpcklqdq %xmm7,%xmm7\n"
    "movaps %xmm7,-0x190(%rbp)\nmovaps %xmm7,-0x270(%rbp)\n"
    "movabs $0x3f0000003f000000,%rax\nmovq %rax,%xmm7\npunpcklqdq %xmm7,%xmm7\n"
    "movaps %xmm7,-0x210(%rbp)\ncall *%rdi\n"
    "movaps %xmm1,0x10(%rsi)\nleave\nret\n"
    ".section .rodata\n.Lrotation_pattern:\n"
    ".long 0x3f123456,0x3f234567,0x3f345678,0x3f456789,0x3f56789a,0x3f6789ab,0x3f789abc,0x3f89abcd\n.text\n");

static TimestepRegisters register_before;
static int register_visits;
static uintptr_t register_skip;
static void ABI check_registers(TimestepRegisters *r) {
    if (!register_visits++) {
        memcpy(&register_before,r,sizeof *r);
        if (register_skip) r->resume=register_skip;
    } else assert(!memcmp(&register_before,r,offsetof(TimestepRegisters,resume)));
    /* A helper or libm may overwrite all vector registers, including upper YMM lanes. */
    __asm__ volatile("vzeroall\nxor %%eax,%%eax\nxor %%ecx,%%ecx\nxor %%edx,%%edx"
        : : : "rax","rcx","rdx","cc","xmm0","xmm1","xmm2","xmm3",
          "xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11",
          "xmm12","xmm13","xmm14","xmm15");
}

static double rotation_distance(const float *a, const float *b) {
    double aa=0,bb=0,ab=0;
    for (int i=0;i<4;i++) { aa+=(double)a[i]*a[i]; bb+=(double)b[i]*b[i]; ab+=(double)a[i]*b[i]; }
    return 2*acos(fmin(1,fabs(ab)/sqrt(aa*bb)))*180/acos(-1);
}

static void rotation_step(void *entry, float *transform, float dt, float frame_dt, const float *velocity) {
    _Alignas(16) float info[16]={0}, omega[4]={0};
    set_frame_dt(frame_dt);
    for (unsigned i=0;i<4;i++) info[8+i]=dt;
    memcpy(omega,velocity,3*sizeof(float));
    rotation_probe(entry,transform,info,omega);
}

static const float angular_velocities[][3]={
    {0,0,3.14159265f},{1,2,3},{0,0,12.5663706f},{0,0,0},
    {0,-3.14159265f,0},{1e-5f,0,0},{-3,2,-1},{12.5663706f,0,0}
};

static void rotation_simulate(void *entry, float *out, const float *initial, unsigned scenario, unsigned rate, float time_scale) {
    _Alignas(16) float transform[8]={0};
    const float mixed[]={1.f/240,1.f/80,1.f/60};
    memcpy(transform+4,initial,4*sizeof(float));
    for (unsigned frame=0;frame<30;frame++) {
        const unsigned v=scenario<8 ? scenario : scenario==8 ? frame%8 :
                         scenario==9 ? (frame/5)%8 : frame%2 ? 2 : 4;
        for (unsigned sub=0;sub<(rate ? rate/30 : 3);sub++) {
            const float frame_dt=rate ? 1.f/rate : mixed[sub];
            rotation_step(entry,transform,frame_dt*time_scale,frame_dt,angular_velocities[v]);
        }
    }
    memcpy(out,transform+4,4*sizeof(float));
}

static void test_rotation_timestep(unsigned char *image) {
    unsigned char *code=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(code!=MAP_FAILED);
    assert(!memcmp(image+0x8790eb,"\xc5\xf8\x28\x8d\xf0\xfd\xff\xff\xc5\xf0\x59\x49\x20\xc5\xc8\x59\xc9",17));
    /* Original quaternion integration ends before the native linear integration. */
    memcpy(code,image+0x8790eb,0x97); code[0x97]=0xc3;
    assert(register_timestep_bridge(code+512,code,17,code+17,rotation_timestep)<=512);
    const unsigned rates[]={30,60,90,120,240,0};
    const float time_scales[]={.125f,.5f,1,2};
    float initials[][4]={{0,0,0,1},{.2f,.3f,-.1f,.9f},{0,1,0,0},{-.5f,.5f,-.5f,.5f}};
    double worst_native=0,worst_fixed=0;
    for (unsigned q=0;q<4;q++) {
        float norm=0;
        for (unsigned i=0;i<4;i++) norm+=initials[q][i]*initials[q][i];
        for (unsigned i=0;i<4;i++) initials[q][i]/=sqrtf(norm);
        for (unsigned scenario=0;scenario<11;scenario++)
        for (unsigned s=0;s<sizeof time_scales/sizeof *time_scales;s++) {
            float reference[4],raw[4],fixed[4];
            rotation_simulate(code,reference,initials[q],scenario,30,time_scales[s]);
            for (unsigned r=0;r<sizeof rates/sizeof *rates;r++) {
                rotation_simulate(code,raw,initials[q],scenario,rates[r],time_scales[s]);
                rotation_simulate(code+512,fixed,initials[q],scenario,rates[r],time_scales[s]);
                const double error=rotation_distance(reference,fixed);
                worst_native=fmax(worst_native,rotation_distance(reference,raw));
                worst_fixed=fmax(worst_fixed,error);
                assert(isfinite(error) && error<.002);
                if (rates[r]==30) assert(!memcmp(raw,fixed,sizeof raw));
            }
        }
    }
    assert(worst_native>10);
    const unsigned other_rates[]={72,144};
    for (unsigned v=0;v<8;v++) for (unsigned r=0;r<2;r++) {
        _Alignas(16) float fixed[8]={0,0,0,0,0,0,0,1};
        float reference[4];
        rotation_simulate(code,reference,initials[0],v,30,1);
        for (unsigned frame=0;frame<other_rates[r];frame++)
            rotation_step(code+512,fixed,1.f/other_rates[r],1.f/other_rates[r],angular_velocities[v]);
        assert(rotation_distance(reference,fixed+4)<.002);
    }
    const float fallback[]={0,-1.f/60,1.f/20,273.f/8192,274.f/8192,NAN,INFINITY};
    for (unsigned i=0;i<sizeof fallback/sizeof *fallback;i++)
    for (unsigned simulation=0;simulation<2;simulation++) {
        _Alignas(16) float raw[8]={0,0,0,0,0,0,0,1},fixed[8];
        const float dt=simulation ? 1.f/240 : fallback[i];
        memcpy(fixed,raw,sizeof raw);
        rotation_step(code,raw,dt,fallback[i],angular_velocities[1]);
        rotation_step(code+512,fixed,dt,fallback[i],angular_velocities[1]);
        assert(!memcmp(raw,fixed,sizeof raw));
    }
    _Alignas(16) float transform[8]={0},info[16]={0},omega[4]={0};
    for (unsigned absent=0;absent<2;absent++) {
        _Alignas(16) float raw[8]={0,0,0,0,0,0,0,1},fixed[8];
        memcpy(fixed,raw,sizeof raw);
        for (unsigned i=0;i<4;i++) info[8+i]=1.f/240;
        memcpy(omega,angular_velocities[1],sizeof angular_velocities[1]);
        rotation_probe(code,raw,info,omega);
        timestep_clock=absent ? &test_clock_pointer : NULL;
        test_clock_pointer=NULL;
        rotation_probe(code+512,fixed,info,omega);
        assert(!memcmp(raw,fixed,sizeof raw));
        test_clock_pointer=test_clock;
    }
    const float invalid_omega[]={NAN,INFINITY,-INFINITY};
    for (unsigned i=0;i<3;i++) {
        TimestepRegisters registers={0},before;
        info[8]=1.f/240;
        set_frame_dt(1.f/240);
        registers.rcx=(uintptr_t)info;
        registers.ymm[6][0]=invalid_omega[i];
        memcpy(&before,&registers,sizeof before);
        rotation_timestep(&registers);
        assert(!memcmp(&registers,&before,sizeof before));
    }
    for (unsigned skip=0;skip<2;skip++) {
        const unsigned char displaced[]={0x0f,0x0b};
        register_skip=skip ? (uintptr_t)(code+1536) : 0;
        register_visits=0;
        assert(register_timestep_bridge(code+1024,skip ? displaced : (const unsigned char *)"\x90",
                                       skip ? 2 : 1,code+1536,check_registers)<=512);
        assert(register_timestep_bridge(code+1536,(const unsigned char *)"\x90",1,code+2048,check_registers)<=512);
        code[2048]=0xc3;
        rotation_probe(code+1024,transform,info,omega);
        assert(register_visits==2);
    }
    printf("native rotation: 1056 cases, 176 exact 30 FPS including scaled time, fallback and full register checks pass; gap %.9f -> %.9f degrees\n",worst_native,worst_fixed);
    assert(!munmap(code,4096));
}

static void test_solver_timestep(unsigned char *image) {
    typedef void (ABI *SolverStep)(void *, const float *, int, int, float, float);
    SolverStep step=(SolverStep)(image+0x6e7af0);
    const int rates[]={30,60,90,120,240};
    const float gravity[][4]={{0,-980,0,0},{-300,-400,800,0},{0,0,0,0}};
    assert(!memcmp(image+0x6e7af0,"\xc4\xe3\x79\x04\xc0\x00",6));
    for (unsigned g=0;g<sizeof gravity/sizeof *gravity;g++)
        for (int iterations=2;iterations<=8;iterations*=2)
            for (int microsteps=1;microsteps<=4;microsteps*=2)
                for (unsigned r=0;r<=sizeof rates/sizeof *rates;r++) {
                    unsigned char info[0x150]={0};
                    double impulse[3]={0}, substep_impulse[3]={0};
                    const int mixed=r==sizeof rates/sizeof *rates;
                    const int frames=mixed ? 90 : rates[r];
                    const float mixed_dt[]={1.f/240,1.f/80,1.f/60};
                    for (int frame=0;frame<frames;frame++) {
                        const float dt=mixed ? mixed_dt[frame%3] : 1.f/rates[r];
                        step(info,gravity[g],iterations,microsteps,.1f,dt);
                        assert(real(info,0x20)==dt && real(info,0xc8)==dt);
                        assert(fabsf(real(info,0x30)*dt-1)<1e-6f);
                        assert(fabsf(real(info,0x40)*iterations-dt)<1e-7f);
                        assert(integer(info,0xb4)==iterations && integer(info,0xbc)==microsteps);
                        for (int axis=0;axis<3;axis++) {
                            impulse[axis]+=real(info,0xe0+4*axis);
                            substep_impulse[axis]+=real(info,0xd0+4*axis)*iterations;
                        }
                    }
                    /* Inspect native step setup, not the result of a contact simulation. */
                    for (int axis=0;axis<3;axis++) {
                        assert(fabs(impulse[axis]-gravity[g][axis])<.0001);
                        assert(fabs(substep_impulse[axis]-gravity[g][axis])<.0001);
                    }
                }
    puts("native solver setup: gravity timestep invariant at 30-240 FPS and mixed cadence");
}

int main(int argc, char **argv) {
    assert(argc==3);
    FILE *file=fopen(argv[1],"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END));
    const long length=ftell(file); assert(length>0);
    rewind(file);
    unsigned char *elf=malloc((size_t)length); assert(elf);
    assert(fread(elf,1,(size_t)length,file)==(size_t)length); fclose(file);
    const Elf64_Ehdr *header=(const void *)elf;
    const Elf64_Phdr *segments=(const void *)(elf+header->e_phoff);
    size_t size=0;
    for (unsigned i=0;i<header->e_phnum;i++)
        if (segments[i].p_type==PT_LOAD && segments[i].p_vaddr+segments[i].p_memsz>size)
            size=segments[i].p_vaddr+segments[i].p_memsz;
    unsigned char *image=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
    for (unsigned i=0;i<header->e_phnum;i++) if (segments[i].p_type==PT_LOAD)
        memcpy(image+segments[i].p_vaddr,elf+segments[i].p_offset,segments[i].p_filesz);
    test_solver_timestep(image);
    test_rotation_timestep(image);
    assert(!memcmp(image+0xe4a231,"\x48\x8b\x05",3) && image[0xe4a292]==0xe8);
    int32_t displacement;
    memcpy(&displacement,image+0xe4a234,4);
    uint64_t cookie=0x1234;
    set_pointer(image,0xe4a238+(int64_t)displacement,&cookie);
    memcpy(&displacement,image+0xe4a293,4);
    /* Only component lookup is stubbed; all speed and curve branches use original bytes. */
    unsigned char *lookup=image+0xe4a297+(int64_t)displacement;
    memcpy(lookup,"\x48\xb8",2);
    set_pointer(lookup,2,(void *)fixture_boost);
    memcpy(lookup+10,"\xff\xe0",2);
    native_kart_speed=(KartSpeed)(image+0xe4a220);
    component=fixture_boost;
    void *meta=(void *)1; boost_type=&meta;
    file=fopen(argv[2],"rb"); assert(file);
    uint32_t count; assert(fread(&count,4,1,file)==1 && count>0);
    const int rates[]={30,60,90,120,240};
    float largest_native_difference=0;
    for (uint32_t fixture=0;fixture<count;fixture++) {
        unsigned char kart[0x300]={0}, data[48], curves[3][104];
        assert(fread(data,sizeof data,1,file)==1 && fread(curves,sizeof curves,1,file)==1);
        for (int i=0;i<3;i++) set_pointer(data,0x18+8*i,curves[i]);
        set_float(kart,0x190,1); set_float(kart,0x198,1); set_float(kart,0x19c,1);
        set_pointer(kart,0x18,kart);
        const float top=real(data,12);
        /* AnimationSpeed scales motion time; e47361 can also scale curve time independently. */
        const float time_scales[]={.125f,.5f,1,2};
        for (int kind=0;kind<2;kind++)
        for (unsigned simulation=0;simulation<sizeof time_scales/sizeof *time_scales;simulation++)
        for (unsigned curve=0;curve<sizeof time_scales/sizeof *time_scales;curve++) {
            const float reference_dt=time_scales[simulation]/30*time_scales[curve];
            const float reference=native_kart_speed(kart,data,kind,top*3,reference_dt);
            for (unsigned r=0;r<sizeof rates/sizeof *rates;r++) {
                float speed=top*3;
                set_frame_dt(1.f/rates[r]);
                kart_motion_dt=time_scales[simulation]/rates[r];
                for (int i=0;i<rates[r]/30;i++)
                    speed=kart_speed_timestep(kart,data,kind,speed,kart_motion_dt*time_scales[curve]);
                if (rates[r]==30) assert(speed==reference);
                assert(speed>top*1.05f && fabsf(speed-reference)<top*.0001f);
            }
        }
        for (int kind=0;kind<2;kind++) {
            float reference=top*1.5f;
            for (int i=0;i<3;i++) reference=native_kart_speed(kart,data,kind,reference,1.f/30);
            assert(reference>top*1.05f); /* Stay inside the proved multiplicative branch. */
            for (unsigned r=0;r<sizeof rates/sizeof *rates;r++) {
                float corrected=top*1.5f, original=corrected;
                kart_motion_dt=1.f/rates[r];
                set_frame_dt(kart_motion_dt);
                for (int i=0;i<rates[r]/10;i++) {
                    corrected=kart_speed_timestep(kart,data,kind,corrected,1.f/rates[r]);
                    original=native_kart_speed(kart,data,kind,original,1.f/rates[r]);
                }
                assert(fabsf(corrected-reference)<top*.0001f);
                if (rates[r]==30) assert(corrected==original);
                largest_native_difference=fmaxf(largest_native_difference,fabsf(original-reference));
                const float normal=top*.25f;
                assert(kart_speed_timestep(kart,data,kind,normal,1.f/rates[r])==
                       native_kart_speed(kart,data,kind,normal,1.f/rates[r]));
            }
            float mixed=top*1.5f;
            /* Three 30 Hz periods, each split into unequal 240/80/60 Hz updates. */
            for (int i=0;i<3;i++) {
                kart_motion_dt=1.f/240;
                set_frame_dt(kart_motion_dt);
                mixed=kart_speed_timestep(kart,data,kind,mixed,1.f/240);
                kart_motion_dt=1.f/80;
                set_frame_dt(kart_motion_dt);
                mixed=kart_speed_timestep(kart,data,kind,mixed,1.f/80);
                kart_motion_dt=1.f/60;
                set_frame_dt(kart_motion_dt);
                mixed=kart_speed_timestep(kart,data,kind,mixed,1.f/60);
            }
            assert(fabsf(mixed-reference)<top*.0001f);
        }
        unsigned char boost[0x60]={0}, steering[0x90]={0}, handle[0x30]={0};
        set_pointer(handle,0x28,steering);
        set_pointer(kart,0xb8,handle);
        set_float(steering,0x88,.25f);
        /* Exercise native fallbacks, including real boost and steering-loss branches. */
        for (int state=0;state<4;state++) {
            test_boost=state==1 ? boost : NULL;
            set_float(boost,0x38,top*.2f);
            set_float(kart,0x190,state==2 ? .7f : 1);
            set_float(kart,0x1a0,state==3 ? .2f : 0);
            for (int kind=0;kind<3;kind++) for (int fraction=0;fraction<7;fraction++) {
                const float speed=top*fraction*.25f;
                kart_motion_dt=1.f/30;
                set_frame_dt(kart_motion_dt);
                const float baseline=native_kart_speed(kart,data,kind,speed,1.f/30);
                assert(kart_speed_timestep(kart,data,kind,speed,1.f/30)==baseline);
                if (state || kind==2 || speed<=1.05f*top)
                    for (unsigned r=1;r<sizeof rates/sizeof *rates;r++) {
                        kart_motion_dt=1.f/rates[r];
                        set_frame_dt(kart_motion_dt);
                        assert(kart_speed_timestep(kart,data,kind,speed,1.f/rates[r])==
                               native_kart_speed(kart,data,kind,speed,1.f/rates[r]));
                    }
            }
        }
        test_boost=NULL;
        set_float(kart,0x1a0,0);
        kart_motion_dt=1.f/240;
        const float native=native_kart_speed(kart,data,0,top*3,kart_motion_dt);
        const float fallback[]={0,-1.f/60,1.f/30,1.f/20,NAN,INFINITY};
        for (unsigned i=0;i<sizeof fallback/sizeof *fallback;i++) {
            set_frame_dt(fallback[i]);
            assert(kart_speed_timestep(kart,data,0,top*3,kart_motion_dt)==native);
        }
        for (unsigned absent=0;absent<2;absent++) {
            timestep_clock=absent ? &test_clock_pointer : NULL;
            test_clock_pointer=NULL;
            assert(kart_speed_timestep(kart,data,0,top*3,kart_motion_dt)==native);
            test_clock_pointer=test_clock;
        }
    }
    assert(largest_native_difference>50); /* The original native bytes reproduce the defect. */
    kart_motion_dt=0;
    timestep_clock=NULL;
    printf("native overspeed: %u real curve datasets; max original difference %.3f\n",count,largest_native_difference);
    fclose(file); free(elf); assert(!munmap(image,size));
}
