#define _GNU_SOURCE
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/runtime_race_options.c"
#include "../src/runtime_text.c"

void *camera_position_entry, *camera_pitch_entry;
uint64_t camera_register_failures;
const uint64_t camera_ymm_canary[4]={0x123456789abcdef0,0x02468ace13579bdf,0xfedcba9876543210,0x1122334455667788};
uint64_t camera_ymm_result[4];
float camera_pitch_velocity;
static unsigned char camera_clock[0x40];
static unsigned char *camera_patched_image;
static uint32_t camera_scalar_frame;
static unsigned char camera_guard_cookie;
extern uint32_t run_camera_position(void *,void *,float,float,float,float,unsigned);
extern float run_camera_pitch(void *,float,float,float);
__asm__(".text\n"
"run_camera_position:\n push %rbp\n push %r13\n push %r15\n sub $0xb0,%rsp\n lea 0xb0(%rsp),%rbp\n"
"mov %rdi,%r15\n mov %rsi,%r13\n lea (%r13,%rdx),%rax\n movss %xmm3,-0xac(%rbp)\n movaps %xmm0,%xmm7\n movaps %xmm2,%xmm4\n"
"mov $0x12345678,%r8d\n mov $0x23456789,%r9d\n mov $0x3456789a,%r10d\n mov $0x456789ab,%r11d\n vmovups camera_ymm_canary(%rip),%ymm15\n"
"call *camera_position_entry(%rip)\n vmovups %ymm15,camera_ymm_result(%rip)\n"
"cmp $0x12345678,%r8d\n jne 1f\n cmp $0x23456789,%r9d\n jne 1f\n cmp $0x3456789a,%r10d\n jne 1f\n cmp $0x456789ab,%r11d\n je 2f\n1: incq camera_register_failures(%rip)\n2:\n"
"add $0xb0,%rsp\n pop %r15\n pop %r13\n pop %rbp\n ret\n"
"run_camera_pitch:\n push %rbp\n push %r12\n push %r15\n sub $0xb0,%rsp\n lea 0xb0(%rsp),%rbp\n"
"mov %rdi,%r15\n movd %xmm0,%r12d\n movss %xmm2,-0xac(%rbp)\n movaps %xmm1,%xmm2\n movss 0x20c(%r15),%xmm5\n"
"mov $0x12345678,%r8d\n mov $0x23456789,%r9d\n mov $0x3456789a,%r10d\n mov $0x456789ab,%r11d\n vmovups camera_ymm_canary(%rip),%ymm15\n"
"call *camera_pitch_entry(%rip)\n vmovups %ymm15,camera_ymm_result(%rip)\n movss %xmm0,camera_pitch_velocity(%rip)\n movaps %xmm1,%xmm0\n"
"cmp $0x12345678,%r8d\n jne 1f\n cmp $0x23456789,%r9d\n jne 1f\n cmp $0x3456789a,%r10d\n jne 1f\n cmp $0x456789ab,%r11d\n je 2f\n1: incq camera_register_failures(%rip)\n2:\n"
"add $0xb0,%rsp\n pop %r15\n pop %r12\n pop %rbp\n ret\n");

static unsigned char *load_camera_image(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long length=ftell(f); assert(length>0); rewind(f);
    unsigned char *elf=malloc((size_t)length); assert(elf);
    assert(fread(elf,(size_t)length,1,f)==1); fclose(f);
    const Elf64_Ehdr *header=(const void *)elf;
    const Elf64_Phdr *segments=(const void *)(elf+header->e_phoff);
    *size=0;
    for (unsigned i=0;i<header->e_phnum;++i)
        if (segments[i].p_type==PT_LOAD && segments[i].p_vaddr+segments[i].p_memsz>*size)
            *size=segments[i].p_vaddr+segments[i].p_memsz;
    unsigned char *image=mmap(NULL,*size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
    for (unsigned i=0;i<header->e_phnum;++i)
        if (segments[i].p_type==PT_LOAD)
            memcpy(image+segments[i].p_vaddr,elf+segments[i].p_offset,segments[i].p_filesz);
    free(elf); return image;
}

static void initialize_camera(float *camera) {
    memset(camera,0,0x300);
    camera[0x1f4/4]=30; camera[0x1f8/4]=-20;
    camera[0x204/4]=20; camera[0x208/4]=-30;
    camera[0x200/4]=30; camera[0x1fc/4]=20;
    camera[0x20c/4]=30; camera[0x210/4]=20;
}

static void camera_tick(unsigned char *image,float *camera,float damping,
                        float tx,float ty,float height,float dt,unsigned alternate,float frame_dt) {
    memcpy(camera_clock+0x18,&frame_dt,4);
    if (image==camera_patched_image) {
        ++camera_scalar_frame;
        memcpy(camera_clock+0x10,&camera_scalar_frame,4);
    }
    float settings[16]={0}; settings[0x14/4]=settings[0x18/4]=settings[0x34/4]=damping;
    camera_position_entry=image+0xc89b54;
    const uint32_t result=run_camera_position(camera,settings,tx,ty,height,dt,alternate?0x34:0x18);
    uint32_t expected; memcpy(&expected,camera+0x1f8/4,4); assert(result==expected);
    assert(!memcmp(camera_ymm_canary,camera_ymm_result,sizeof camera_ymm_canary));
    camera_pitch_entry=image+0xc89c33;
    const float pitch=run_camera_pitch(camera,height,damping,dt);
    assert(!memcmp(&pitch,camera+0x20c/4,4));
    assert(!memcmp(&camera_pitch_velocity,camera+0x210/4,4));
    assert(!memcmp(camera_ymm_canary,camera_ymm_result,sizeof camera_ymm_canary));
}

static void setup_camera_direction_image(unsigned char *image) {
    const uintptr_t cookie=(uintptr_t)&camera_guard_cookie;
    memcpy(image+0x2807098,&cookie,sizeof cookie);
    int32_t displacement; memcpy(&displacement,image+0xa98395,4);
    unsigned char *thunk=image+0xa98399+displacement;
    memcpy(thunk,"\xff\x25\0\0\0\0",6);
    const uintptr_t mod=(uintptr_t)fmodf; memcpy(thunk+6,&mod,sizeof mod);
}

static void camera_direction_tick(unsigned char *image,float position[3],float velocity[3],
                                  const float target[3],float damping,float dt,float frame_dt,uint32_t frame) {
    memcpy(camera_clock+0x10,&frame,sizeof frame);
    memcpy(camera_clock+0x18,&frame_dt,sizeof frame_dt);
    ((CameraDirectionSpring)(image+0xa997c0))(position,velocity,target[0],target[1],target[2],damping,dt);
}

static void test_camera_direction(unsigned char *native,unsigned char *patched) {
    const unsigned rates[]={20,24,30,60,90,120,144,240};
    double worst_position=0,worst_velocity=0; unsigned boundaries=0;
    for (unsigned moving=0;moving<2;++moving) for (unsigned ri=0;ri<sizeof rates/sizeof *rates;++ri) {
        memset(camera_direction_states,0,sizeof camera_direction_states); camera_direction_next=0;
        float reference_position[]={1,0,0},reference_velocity[]={0,0,0};
        float fixed_position[]={1,0,0},fixed_velocity[]={0,0,0};
        double reference_time=0; const unsigned rate=rates[ri];
        for (unsigned frame=1;frame<=rate*2;++frame) {
            const double time=(double)frame/rate;
            const double angle=2*3.141592653589793*fmax(0,time-.5);
            const float target[]={moving?(float)cos(angle):1,moving?(float)sin(angle):0,0};
            camera_direction_tick(patched,fixed_position,fixed_velocity,target,.26f,1.f/rate,1.f/rate,frame);
            while (reference_time+1./30<=time+1e-10) {
                reference_time+=1./30;
                const double reference_angle=2*3.141592653589793*fmax(0,reference_time-.5);
                const float reference_target[]={moving?(float)cos(reference_angle):1,
                                                moving?(float)sin(reference_angle):0,0};
                camera_direction_tick(native,reference_position,reference_velocity,reference_target,.26f,
                                      1.f/30,1.f/30,(uint32_t)(reference_time*30+0.5));
                if (fabs(reference_time-time)<1e-9) {
                    for (unsigned axis=0;axis<3;++axis) {
                        const double position_gap=fabs(fixed_position[axis]-reference_position[axis]);
                        const double velocity_gap=fabs(fixed_velocity[axis]-reference_velocity[axis]);
                        if (position_gap>worst_position) worst_position=position_gap;
                        if (velocity_gap>worst_velocity) worst_velocity=velocity_gap;
            if (!(position_gap<.002 && velocity_gap<.02))
                fprintf(stderr,"camera direction mismatch: rate %u moving %u t %.6f axis %u position %.9g velocity %.9g\n",
                        rate,moving,time,axis,position_gap,velocity_gap);
            assert(position_gap<.002 && velocity_gap<.02);
                    }
                    ++boundaries;
                }
            }
        }
    }
    assert(boundaries>0);
    printf("camera direction: %u reference boundaries; moving/static targets at 20-240 FPS; max position %.9g, velocity %.9g\n",
           boundaries,worst_position,worst_velocity);
}

static void test_camera_direction_mixed(unsigned char *native,unsigned char *patched) {
    const float intervals[]={1.f/120,1.f/45,1.f/240,1.f/60,1.f/24};
    memset(camera_direction_states,0,sizeof camera_direction_states); camera_direction_next=0;
    float reference_position[]={1,0,0},reference_velocity[]={0,0,0};
    float fixed_position[]={1,0,0},fixed_velocity[]={0,0,0};
    double time=0,reference_time=0,worst_position=0,worst_velocity=0;
    unsigned boundaries=0;
    for (unsigned frame=1;frame<=600;++frame) {
        const float dt=intervals[(frame-1)%(sizeof intervals/sizeof *intervals)];
        time+=dt;
        const double angle=2*3.141592653589793*fmax(0,time-.5);
        const float target[]={(float)cos(angle),(float)sin(angle),0};
        camera_direction_tick(patched,fixed_position,fixed_velocity,target,.26f,dt,dt,frame);
        while (reference_time+1./30<=time+1e-10) {
            reference_time+=1./30;
            const double reference_angle=2*3.141592653589793*fmax(0,reference_time-.5);
            const float reference_target[]={(float)cos(reference_angle),(float)sin(reference_angle),0};
            camera_direction_tick(native,reference_position,reference_velocity,reference_target,.26f,
                                  1.f/30,1.f/30,(uint32_t)(reference_time*30+0.5));
            if (fabs(reference_time-time)<1e-5) {
                for (unsigned axis=0;axis<3;++axis) {
                    const double position_gap=fabs(fixed_position[axis]-reference_position[axis]);
                    const double velocity_gap=fabs(fixed_velocity[axis]-reference_velocity[axis]);
                    if (position_gap>worst_position) worst_position=position_gap;
                    if (velocity_gap>worst_velocity) worst_velocity=velocity_gap;
                    assert(position_gap<.002 && velocity_gap<.02);
                }
                ++boundaries;
            }
        }
    }
    assert(boundaries>0);
    printf("camera direction: mixed 24-240 FPS cadence, %u boundaries; max position %.9g, velocity %.9g\n",
           boundaries,worst_position,worst_velocity);
}

static void test_camera_slow_frames(unsigned char *native,unsigned char *patched) {
    const unsigned rates[]={20,24};
    const float scales[]={.5f,1,2};
    double worst_gap=0; unsigned checks=0;
    for (unsigned si=0;si<sizeof scales/sizeof *scales;++si)
        for (unsigned ri=0;ri<sizeof rates/sizeof *rates;++ri) {
            float reference[0x300/4],fixed[0x300/4];
            initialize_camera(reference); initialize_camera(fixed);
            const unsigned rate=rates[ri];
            for (unsigned frame=0;frame<rate*2;++frame) {
                camera_tick(native,reference,.26f,45,-60,45,scales[si]/rate,0,1.f/rate);
                camera_tick(patched,fixed,.26f,45,-60,45,scales[si]/rate,0,1.f/rate);
                for (unsigned i=0;i<sizeof reference/sizeof *reference;++i) {
                    const double gap=fabs((double)fixed[i]-reference[i]);
                    if (gap>worst_gap) worst_gap=gap;
                    assert(gap==0);
                    ++checks;
                }
            }
        }
    assert(checks>0);
    printf("camera scalar springs: native fallback parity at 20/24 FPS, scales .5/1/2; %u comparisons; max gap %.9g\n",
           checks,worst_gap);
}

static void test_camera_dynamic_targets(unsigned char *native,unsigned char *patched) {
    const unsigned rates[]={60,120,240};
    const float dampings[]={0,.088f,.15f,.2f,.26f,.5f,.65f,1};
    const float scales[]={.5f,1,2};
    const unsigned positions[]={0x1f4,0x1f8,0x200,0x20c};
    const unsigned velocities[]={0x204,0x208,0x1fc,0x210};
    double worst_position=0,worst_velocity=0,worst_boundary=0,worst_inner=0; unsigned checks=0;
    for (unsigned si=0;si<sizeof scales/sizeof *scales;++si)
    for (unsigned di=0;di<sizeof dampings/sizeof *dampings;++di)
    for (unsigned ri=0;ri<sizeof rates/sizeof *rates;++ri) {
        float reference[0x300/4],fixed[0x300/4];
        initialize_camera(reference); initialize_camera(fixed);
        const unsigned rate=rates[ri]; const float scale=scales[si],damping=dampings[di];
        const double frequency=di%2 ? 2.1 : .7;
        double previous_delta[4]={0};
        for (unsigned frame=1;frame<=rate*4;++frame) {
            float before[4];
            for (unsigned i=0;i<4;++i) before[i]=fixed[positions[i]/4];
            const double time=(double)frame/rate*scale,angle=2*3.141592653589793*frequency*time;
            const float tx=(float)(40+18*sin(angle)),ty=(float)(-50+25*cos(angle));
            const float height=(float)(20+12*sin(angle+.4));
            camera_tick(patched,fixed,damping,tx,ty,height,scale/rate,0,1.f/rate);
            for (unsigned i=0;i<4;++i) {
                const double delta=fixed[positions[i]/4]-before[i];
                const double change=fabs(delta-previous_delta[i]);
                if (si==1 && frame>rate) {
                    if (frame%(rate/30)==0 && change>worst_boundary) worst_boundary=change;
                    if (frame%(rate/30)!=0 && change>worst_inner) worst_inner=change;
                }
                previous_delta[i]=delta;
            }
            if (frame%(rate/30)==0) {
                const double reference_time=(double)frame/rate*scale;
                const double reference_angle=2*3.141592653589793*frequency*reference_time;
                const float reference_tx=(float)(40+18*sin(reference_angle));
                const float reference_ty=(float)(-50+25*cos(reference_angle));
                const float reference_height=(float)(20+12*sin(reference_angle+.4));
                camera_tick(native,reference,damping,reference_tx,reference_ty,reference_height,scale/30,0,1.f/30);
                for (unsigned i=0;i<4;++i) {
                    const double pg=fabs(fixed[positions[i]/4]-reference[positions[i]/4]);
                    const double vg=fabs(fixed[velocities[i]/4]-reference[velocities[i]/4]);
                    if (pg>worst_position) worst_position=pg;
                    if (vg>worst_velocity) worst_velocity=vg;
                    assert(pg<.002 && vg<.02); ++checks;
                }
            }
        }
    }
    assert(worst_boundary>0 && worst_inner>0 && worst_boundary<1.5*worst_inner);
    printf("dynamic camera targets: 60/120/240 FPS, damping 0–1, scales .5/1/2, 0.7/2.1 Hz; %u boundaries; max position %.9g velocity %.9g; frame step change %.9g/%.9g\n",
           checks,worst_position,worst_velocity,worst_boundary,worst_inner);
}

static void test_camera_dynamic_quantized(unsigned char *native,unsigned char *patched) {
    const unsigned rates[]={60,120,240};
    const unsigned positions[]={0x1f4,0x1f8,0x200,0x20c};
    const unsigned velocities[]={0x204,0x208,0x1fc,0x210};
    double worst_position=0,worst_velocity=0; unsigned checks=0;
    unsigned long long ticks=0;
    for (unsigned ri=0;ri<sizeof rates/sizeof *rates;++ri) {
        const unsigned rate=rates[ri], per_reference=rate/30, base=8192/rate;
        float reference[0x300/4],fixed[0x300/4];
        initialize_camera(reference); initialize_camera(fixed);
        for (unsigned boundary=0;boundary<60;++boundary) {
            const unsigned reference_ticks=273+(boundary%30>=28);
            const unsigned extra=reference_ticks-base*per_reference;
            for (unsigned i=0;i<per_reference;++i) {
                const unsigned frame_ticks=base+(i>=per_reference-extra);
                ticks+=frame_ticks;
                const double time=(double)ticks/8192;
                const double angle=2*3.141592653589793*.7*time;
                const float tx=(float)(40+18*sin(angle)),ty=(float)(-50+25*cos(angle));
                const float height=(float)(20+12*sin(angle+.4));
                const float dt=(float)frame_ticks/8192;
                camera_tick(patched,fixed,.26f,tx,ty,height,dt,0,dt);
            }
            const double time=(double)ticks/8192;
            const double angle=2*3.141592653589793*.7*time;
            const float tx=(float)(40+18*sin(angle)),ty=(float)(-50+25*cos(angle));
            const float height=(float)(20+12*sin(angle+.4));
            const float dt=(float)reference_ticks/8192;
            camera_tick(native,reference,.26f,tx,ty,height,dt,0,dt);
            for (unsigned i=0;i<4;++i) {
                const double pg=fabs(fixed[positions[i]/4]-reference[positions[i]/4]);
                const double vg=fabs(fixed[velocities[i]/4]-reference[velocities[i]/4]);
                if(pg>worst_position)worst_position=pg;
                if(vg>worst_velocity)worst_velocity=vg;
                if (!(pg<.002 && vg<.02)) fprintf(stderr,"quantized camera mismatch rate %u boundary %u channel %u pos %.9g vel %.9g\n",rate,boundary,i,pg,vg);
                assert(pg<.002 && vg<.02); ++checks;
            }
        }
    }
    printf("dynamic camera targets: quantized 8192 Hz clock at 60/120/240 FPS; %u boundaries; max position %.9g velocity %.9g\n",
           checks,worst_position,worst_velocity);
}

static void test_camera_direction_fallback(unsigned char *native,unsigned char *patched) {
    memset(camera_direction_states,0,sizeof camera_direction_states); camera_direction_next=0;
    float patched_position[]={1,0,0},patched_velocity[]={0,0,0};
    const float first_target[]={1,0,0},jump_target[]={0,1,0};
    camera_direction_tick(patched,patched_position,patched_velocity,first_target,.26f,1.f/120,1.f/120,1);
    float expected_position[3],expected_velocity[3];
    memcpy(expected_position,patched_position,sizeof expected_position);
    memcpy(expected_velocity,patched_velocity,sizeof expected_velocity);
    camera_direction_tick(native,expected_position,expected_velocity,jump_target,.26f,1.f/120,1.f/120,2);
    camera_direction_tick(patched,patched_position,patched_velocity,jump_target,.26f,1.f/120,1.f/120,2);
    assert(!memcmp(expected_position,patched_position,sizeof expected_position));
    assert(!memcmp(expected_velocity,patched_velocity,sizeof expected_velocity));

    memset(camera_direction_states,0,sizeof camera_direction_states); camera_direction_next=0;
    float wide_position[]={3,0,0},wide_velocity[]={0,0,0};
    float reference_position[]={3,0,0},reference_velocity[]={0,0,0};
    const float opposite[]={-1,0,0};
    camera_direction_tick(native,reference_position,reference_velocity,opposite,.26f,1.f/120,1.f/120,1);
    camera_direction_tick(patched,wide_position,wide_velocity,opposite,.26f,1.f/120,1.f/120,1);
    assert(!memcmp(reference_position,wide_position,sizeof reference_position));
    assert(!memcmp(reference_velocity,wide_velocity,sizeof reference_velocity));
}

int main(int argc,char **argv) {
    assert(argc==2); size_t size,patched_size;
    void *clock=camera_clock; timestep_clock=&clock;
    unsigned char *native=load_camera_image(argv[1],&size), *patched=load_camera_image(argv[1],&patched_size);
    setup_camera_direction_image(native); setup_camera_direction_image(patched);
    camera_patched_image=patched;
    assert(size==patched_size && camera_timestep_contract(native,size) && camera_timestep_contract(patched,size));
    patched[0xc89bdc]^=1; assert(!camera_timestep_contract(patched,size)); patched[0xc89bdc]^=1;
    patched[0xc89be4]^=1; assert(!camera_timestep_contract(patched,size)); patched[0xc89be4]^=1;
    const size_t changed[]={0xc89bae,0xc89c7d,0xa996d6};
    for (unsigned i=0;i<3;++i) {
        patched[changed[i]]^=1; assert(!camera_timestep_contract(patched,size)); patched[changed[i]]^=1;
    }
    patched[0xa997c0]^=1; assert(!camera_timestep_contract(patched,size)); patched[0xa997c0]^=1;
    int32_t relative; memcpy(&relative,patched+0xc89b8e,4);
    const size_t constant=0xc89b92+relative; patched[constant]^=1;
    assert(!camera_timestep_contract(patched,size)); patched[constant]^=1;
    assert(camera_timestep_contract(patched,size));
    unsigned char *bridge=mmap(NULL,1664,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(bridge!=MAP_FAILED); memset(bridge,0xcc,1664); install_camera_timestep(patched,bridge);
    for (unsigned i=1600;i<1664;++i) assert(bridge[i]==0xcc);
    assert(!memcmp(native+0xa99670,patched+0xa99670,137));
    assert(!memcmp(native+0xc8950a,patched+0xc8950a,5));
    assert(!memcmp(native+0xa997d4,patched+0xa997d4,310-20));
    test_camera_direction(native,patched);
    test_camera_direction_mixed(native,patched);
    test_camera_direction_fallback(native,patched);
    /* Stop each native block at the production continuation, without running the game. */
    native[0xc89be4]=patched[0xc89be4]=0xc3;
    native[0xc89c97]=patched[0xc89c97]=0xc3;
    test_camera_slow_frames(native,patched);
    test_camera_dynamic_targets(native,patched);
    test_camera_dynamic_quantized(native,patched);
    /* Eight values from 352 camera settings in 47 root maps, plus negative input checks. */
    const float dampings[]={0,.088f,.15f,.2f,.26f,.5f,.65f,1,-.26f,-1};
    const unsigned count=sizeof dampings/sizeof *dampings;
    const int rates[]={30,60,90,120,240,0};
    const float scales[]={.125f,.5f,1,2};
    const unsigned positions[]={0x1f4,0x1f8,0x200,0x20c},velocities[]={0x204,0x208,0x1fc,0x210};
    double native_gap=0,position_gap=0,velocity_gap=0; unsigned checks=0;
    for (unsigned scale=0;scale<4;++scale) for (unsigned d=0;d<count;++d) for (unsigned changing=0;changing<2;++changing) for (unsigned rate=0;rate<7;++rate) {
        float reference[0x300/4],old[0x300/4],fixed[0x300/4];
        initialize_camera(reference); initialize_camera(old); initialize_camera(fixed);
        for (unsigned tick=0;tick<30;++tick) {
            const unsigned r=rate<6?rate:tick%6, steps=rates[r]?(unsigned)rates[r]/30:3;
            const float damping=dampings[(d+(changing?tick/3:0))%count];
            const float tx=changing && (tick/3)%2 ? 45:-15,ty=changing && (tick/3)%2 ? -60:40;
            camera_tick(native,reference,damping,tx,ty,tx,scales[scale]/30,tick%2,1.f/30);
            for (unsigned j=0;j<steps;++j) {
                const float mixed[]={1.f/240,1.f/80,1.f/60}; const float dt=rates[r]?1.f/rates[r]:mixed[j];
                camera_tick(native,old,damping,tx,ty,tx,dt*scales[scale],tick%2,dt);
                camera_tick(patched,fixed,damping,tx,ty,tx,dt*scales[scale],tick%2,dt);
            }
            if (rate==0) assert(!memcmp(reference,fixed,sizeof fixed));
            for (unsigned a=0;a<4;++a) {
                const unsigned x=positions[a]/4,v=velocities[a]/4;
                native_gap=fmax(native_gap,fabs(old[x]-reference[x]));
                position_gap=fmax(position_gap,fabs(fixed[x]-reference[x]));
                velocity_gap=fmax(velocity_gap,fabs(fixed[v]-reference[v]));
                assert(fabs(fixed[x]-reference[x])<.002 && fabs(fixed[v]-reference[v])<.02);
            }
            ++checks;
        }
    }
    for (unsigned scale=0;scale<4;++scale) for (unsigned d=0;d<count;++d) for (unsigned rate=72;rate<=144;rate+=72) {
        float reference[0x300/4],fixed[0x300/4]; initialize_camera(reference); initialize_camera(fixed);
        for (unsigned block=0;block<6;++block) {
            for (unsigned j=0;j<5;++j) camera_tick(native,reference,dampings[d],-15,40,-15,scales[scale]/30,block%2,1.f/30);
            for (unsigned j=0;j<rate/6;++j) camera_tick(patched,fixed,dampings[d],-15,40,-15,scales[scale]/rate,block%2,1.f/rate);
            for (unsigned a=0;a<4;++a) {
                const unsigned x=positions[a]/4,v=velocities[a]/4;
                position_gap=fmax(position_gap,fabs(fixed[x]-reference[x]));
                velocity_gap=fmax(velocity_gap,fabs(fixed[v]-reference[v]));
                assert(fabs(fixed[x]-reference[x])<.002 && fabs(fixed[v]-reference[v])<.02);
            }
            ++checks;
        }
    }
    const float invalid[]={0,-.01f,INFINITY,NAN};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;++i) {
        float a[0x300/4],b[0x300/4]; initialize_camera(a); initialize_camera(b);
        camera_tick(native,a,.26f,4,7,-3,invalid[i],i%2,1.f/240); camera_tick(patched,b,.26f,4,7,-3,invalid[i],i%2,1.f/240);
        assert(!memcmp(a,b,sizeof a));
    }
    const float native_cadence[]={0,-.01f,1.f/30,273.f/8192,274.f/8192,1.f/20,INFINITY,NAN};
    for (unsigned i=0;i<sizeof native_cadence/sizeof *native_cadence;++i) {
        float a[0x300/4],b[0x300/4]; initialize_camera(a); initialize_camera(b);
        camera_tick(native,a,.26f,4,7,-3,1.f/240,i%2,native_cadence[i]);
        camera_tick(patched,b,.26f,4,7,-3,1.f/240,i%2,native_cadence[i]);
        assert(!memcmp(a,b,sizeof a));
    }
    const float nonfinite[]={INFINITY,-INFINITY,NAN};
    for (unsigned i=0;i<3;++i) {
        float a[0x300/4],b[0x300/4]; initialize_camera(a); initialize_camera(b);
        camera_tick(native,a,nonfinite[i],4,7,-3,1.f/240,0,1.f/240);
        camera_tick(patched,b,nonfinite[i],4,7,-3,1.f/240,0,1.f/240);
        assert(!memcmp(a,b,sizeof a));
    }
    for (unsigned which=0;which<4;++which) for (unsigned velocity=0;velocity<2;++velocity) for (unsigned i=0;i<3;++i) {
        float a[0x300/4],b[0x300/4]; initialize_camera(a); initialize_camera(b);
        const unsigned offset=velocity?velocities[which]:positions[which];
        a[offset/4]=b[offset/4]=nonfinite[i];
        camera_tick(native,a,.26f,4,7,-3,1.f/240,0,1.f/240); camera_tick(patched,b,.26f,4,7,-3,1.f/240,0,1.f/240);
        /* Independent springs still advance; the invalid spring itself stays native. */
        assert(!memcmp(a+positions[which]/4,b+positions[which]/4,4));
        assert(!memcmp(a+velocities[which]/4,b+velocities[which]/4,4));
    }
    assert(!camera_register_failures && native_gap>1);
    printf("camera native bridges: %u boundaries, %u damping values, AnimationSpeed .125/.5/1/2; native gap %.9g, patched position %.9g velocity %.9g; fallback/registers/contracts pass\n",
           checks,count,native_gap,position_gap,velocity_gap);
    munmap(bridge,1664); munmap(native,size); munmap(patched,size); return 0;
}
