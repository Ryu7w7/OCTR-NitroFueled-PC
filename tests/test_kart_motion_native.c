/* CPU-only complete linear-motion caller; no world, contacts or graphics API. */
#define _GNU_SOURCE
#include "../src/runtime_race_options.c"
#include "../src/runtime_text.c"
#include <assert.h>
#include <elf.h>

static int corrected, fixed_clock, boost_transition;
static float lateral_speed,surface_retention,boost_parameters[3];
static unsigned char native_lateral[2][15],patched_lateral[2][15];
static unsigned char native_deceleration[18], patched_deceleration[18];
static unsigned char native_ground_project[15],patched_ground_project[15];
static size_t motion_clock_address;
static void *ABI no_component(void *entity, void *type, int flags) {
    (void)type; (void)flags; return ptr(entity,0x100);
}
static void ABI entity_changed(void *entity, int flags) { (void)entity; (void)flags; }
static float ABI motion_speed(void *kart, void *data, int kind, float speed, float dt) {
    unsigned char tuned[0x30];
    if(kart_motion_speed_scale!=1) {
        memcpy(tuned,data,sizeof tuned);float top=real(data,12)*kart_motion_speed_scale;
        memcpy(tuned+12,&top,4);data=tuned;
    }
    return corrected ? kart_speed_timestep(kart,data,kind,speed,dt) :
        native_kart_speed(kart,data,kind,speed,dt);
}
static void put_float(void *p, size_t offset, float value) { memcpy((char *)p+offset,&value,4); }
static unsigned char *read_blob(const char *path, size_t *size) {
    FILE *file=fopen(path,"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); long length=ftell(file); assert(length>0); rewind(file);
    unsigned char *data=malloc((size_t)length); assert(data);
    assert(fread(data,1,(size_t)length,file)==(size_t)length); fclose(file);
    *size=(size_t)length; return data;
}
static unsigned char *call_target(unsigned char *image, size_t offset) {
    int32_t displacement;
    assert(image[offset]==0xe8); memcpy(&displacement,image+offset+1,4);
    return image+offset+5+displacement;
}

static uint64_t counter_nanoseconds;
static uint64_t ABI counter(void) { return counter_nanoseconds; }
static void native_clock_reference(unsigned char *image) {
    unsigned char clock[0x38]={0};
    void *object=clock;
    timestep_clock=&object;
    put_float(clock,0x2c,1);
    jump(call_target(image,0xa31a4f),counter);
    const uint64_t frequency=1000000000;
    memcpy(image+0x2af3048,&frequency,sizeof frequency);
    unsigned counts[2]={0};
    for (unsigned frame=1;frame<=30;++frame) {
        counter_nanoseconds=frame*frequency/30;
        ((void (ABI *)(void *))(image+0xa31a10))(clock);
        const float dt=real(clock,0x18);
        assert(dt==273.f/8192 || dt==274.f/8192);
        ++counts[dt==274.f/8192];
        assert(timestep_frames()==1);
    }
    assert(counts[0]==28 && counts[1]==2);
    assert(integer(clock,0x14)==8192 && integer(clock,0x20)==8192);
    timestep_clock=NULL;
    puts("native clock: all 30 quantized reference frames retain native timestep behavior");
}

static unsigned char native_clock_code[15], patched_clock_code[15];
static void clock_series(unsigned char *image,unsigned rate,float scale,int fixed,int patched,uint32_t initial,unsigned char result[0x38]) {
    unsigned char clock[0x38]={0}; void *object=clock; timestep_clock=&object;
    put_int(clock,0x20,(int)initial); put_float(clock,0x2c,scale);
    memcpy(image+0xa31afd,patched ? patched_clock_code : native_clock_code,15);
    for(unsigned frame=0;frame<(rate ? rate*10 : 900);++frame) {
        counter_nanoseconds=rate ? (uint64_t)(frame+1)*1000000000/rate :
            (uint64_t)(frame/3*8+(const unsigned[]){1,4,8}[frame%3])*1000000000/240;
        const float dt=rate ? 1.f/rate : (const float[]){1.f/240,1.f/80,1.f/60}[frame%3];
        put_float(clock,0x34,fixed ? dt : 0);
        const uint32_t previous=(uint32_t)integer(clock,0x20);
        ((void (ABI *)(void *))(image+0xa31a10))(clock);
        assert((uint32_t)((uint32_t)integer(clock,0x20)-previous)<10000);
    }
    memcpy(result,clock,0x38); timestep_clock=NULL;
}
static unsigned char *test_scaled_clock(unsigned char *image,size_t size) {
    assert(scaled_clock_contract(image,size));
    image[0xa31afd]^=1; assert(!scaled_clock_contract(image,size)); image[0xa31afd]^=1;
    unsigned char *bridge=mmap(NULL,576,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(bridge!=MAP_FAILED); memset(bridge,0xcc,576);
    memcpy(native_clock_code,image+0xa31afd,15); install_scaled_clock(image,bridge);
    memcpy(patched_clock_code,image+0xa31afd,15);
    for(unsigned i=512;i<576;++i)assert(bridge[i]==0xcc);
    const unsigned rates[]={30,32,60,64,72,120,128,144,240,256,0};
    const float scales[]={0,.125f,.25f,.5f,.7f,1,1.5f,2,4};
    unsigned cases=0,changed=0;
    for(unsigned fixed=0;fixed<2;++fixed)for(unsigned wrap=0;wrap<2;++wrap)
    for(unsigned scale=0;scale<9;++scale) {
        unsigned char reference[0x38]; const uint32_t initial=wrap ? 0xffffff00u : 0;
        clock_series(image,30,scales[scale],fixed,0,initial,reference);
        for(unsigned rate=0;rate<11;++rate) {
            unsigned char raw[0x38],patched[0x38];
            clock_series(image,rates[rate],scales[scale],fixed,0,initial,raw);
            clock_series(image,rates[rate],scales[scale],fixed,1,initial,patched);
            if(!fixed)assert(integer(patched,0x20)==integer(reference,0x20));
            if(fixed || rates[rate]==30 || scales[scale]==1)assert(!memcmp(raw,patched,sizeof raw));
            changed+=integer(raw,0x20)!=integer(patched,0x20);
            put_int(raw,0x20,integer(patched,0x20)); assert(!memcmp(raw,patched,sizeof raw));
            cases++;
        }
    }
    assert(changed>0);
    unsigned char clocks[2][0x38]={{0}}; void *object=NULL; timestep_clock=&object;
    for(unsigned frame=0;frame<24000;++frame) {
        const float scale=scales[(frame/37)%9]; const int paused=frame%71>=40 && frame%71<60;
        for(unsigned patched=0;patched<2;++patched) {
            object=clocks[patched]; put_float(object,0x2c,scale); ((unsigned char *)object)[0x28]=(unsigned char)paused;
            if(frame%401==0) put_int(object,0x20,(int)(0xffffff00u+frame));
            const uint32_t previous=(uint32_t)integer(object,0x20);
            counter_nanoseconds=(uint64_t)(frame+1)*1000000000/240;
            memcpy(image+0xa31afd,patched ? patched_clock_code : native_clock_code,15);
            ((void (ABI *)(void *))(image+0xa31a10))(object);
            const uint32_t delta=(uint32_t)integer(object,0x20)-previous;
            assert(delta<200); if(paused)assert(delta==0);
        }
        unsigned char raw[0x38];memcpy(raw,clocks[0],sizeof raw);
        put_int(raw,0x20,integer(clocks[1],0x20)); assert(!memcmp(raw,clocks[1],sizeof raw));
    }
    printf("native scaled clock: %u steady cases, 24000 scale/pause/reset frames; native30/fixed/fullspeed exact\n",cases);
    memcpy(image+0xa31afd,native_clock_code,15); timestep_clock=NULL;
    return bridge; /* Complete motion tests below also exercise the patched clock. */
}

static void simulate(unsigned char *image, unsigned char *archetype, unsigned scenario,
                     unsigned action, unsigned rate, float scale, float initial, unsigned duration, float out[5]) {
    memcpy(image+0xe46a55,corrected ? patched_deceleration : native_deceleration,18);
    memcpy(image+0xe46b2a,corrected?patched_lateral[0]:native_lateral[0],15);
    memcpy(image+0xe47237,corrected?patched_lateral[1]:native_lateral[1],15);
    memcpy(image+0xe46acf,corrected ? patched_ground_project : native_ground_project,15);
    memcpy(image+0xa31afd,corrected ? patched_clock_code : native_clock_code,15);
    static const int controls[][4]={{0,1,0,0},{0,0,0,0},{0,0,1,0},{3,1,0,0},{3,1,1,0},{0,1,0,0},{0,0,1,0},
        {0,0,0,0},{0,0,1,0},{0,1,0,0},{0,1,1,0},{0,0,0,1},{0,0,1,1},{0,1,0,1},{0,1,1,1}};
    _Alignas(16) unsigned char kart[0x400]={0},entity[0x400]={0},message[0x40]={0};
    unsigned char handle[0x40]={0},boost_handle[0x40]={0},boost[0x80]={0};
    unsigned char clock[0x40]={0},vtable[0x300]={0},transform[0x80]={0};
    unsigned char surface_handle[0x30]={0},surface[0x30]={0};
    void *frame_clock=clock;
    timestep_clock=&frame_clock;
    put_ptr(image,motion_clock_address,clock);
    put_ptr(vtable,0xb8,entity_changed); put_ptr(entity,0,vtable);
    put_ptr(handle,0x28,archetype); put_ptr(kart,0xb8,handle); put_ptr(kart,0x18,entity);
    put_ptr(boost_handle,0x28,boost); put_ptr(kart,0x90,boost_handle);put_ptr(entity,0x100,boost);
    put_float(boost,0x38,boost_parameters[0]);put_float(boost,0x4c,boost_parameters[1]);put_float(boost,0x50,boost_parameters[2]);
    if(boost_parameters[0]) {put_float(kart,0x174,boost_parameters[0]);put_float(kart,0x170,initial-boost_parameters[0]);}
    put_float(kart,0x190,1); put_float(kart,0x198,1); put_float(kart,0x19c,1);
    put_float(kart,0x210,1); kart[0x214]=kart[0x215]=1;
    put_int(kart,0xe0,controls[action][0]); kart[0x34]=controls[action][1]; kart[0x35]=controls[action][2]; kart[0x36]=controls[action][3];
    kart_motion_speed_scale=scenario==7 ? 1.25f : 1;
    const float yaw=scenario==1 ? .7f : 0;
    put_ptr(entity,0x30,transform); put_float(transform,0x68,yaw);
    put_float(entity,0x60,initial*cosf(yaw)-lateral_speed*sinf(yaw)); put_float(entity,0x64,initial*sinf(yaw)+lateral_speed*cosf(yaw));
    if (scenario==2) { put_float(kart,0x30,.6f); kart[0x36]=1; put_float(entity,0xa0,.3f); }
    if (scenario==5) put_float(kart,0x30,.6f);
    if (scenario==6) kart[0x36]=1;
    if (scenario>=8) {
        put_ptr(kart,0x180,surface_handle); put_ptr(surface_handle,0x28,surface);
        surface[0x28]=scenario==9;put_float(surface,0x1c,surface_retention);
        if(scenario==10) { put_float(surface,0x20,.2f); put_float(surface,0x24,.3f); }
    }
    if (scenario==3) put_float(kart,0x190,.6f);
    if (scenario==4) put_float(kart,0x19c,.7f);
    typedef void (ABI *Motion)(void *,void *,void *);
    Motion motion=(Motion)(image+0xe46250);
    for (unsigned frame=0;frame<(rate ? rate*duration/30 : 3*duration);frame++) {
        const float interval=rate ? 1.f/rate : (const float[]){1.f/240,1.f/80,1.f/60}[frame%3];
        counter_nanoseconds=rate ? (uint64_t)(frame+1)*1000000000/rate :
            (uint64_t)(frame/3*8+(const unsigned[]){1,4,8}[frame%3])*1000000000/240;
        put_float(clock,0x2c,scale); put_float(clock,0x34,fixed_clock ? interval : 0);
        ((void (ABI *)(void *))(image+0xa31a10))(clock);
        const float dt=real(clock,0x24);
        put_float(message,0x18,dt); kart_motion_dt=dt;
        if(boost_transition && frame+1==(rate ? rate*duration/30 : 3*duration))
            put_float(boost,boost_transition==1 ? 0x4c : 0x50,boost_transition==1 ? 1000 : 5000);
        motion(kart,entity,message);
    }
    memcpy(out,entity+0x60,3*sizeof(float)); out[3]=real(kart,0x170); out[4]=real(kart,0xc0);
    timestep_clock=NULL; kart_motion_dt=0; kart_motion_speed_scale=1;
}

static void turn_simulate(unsigned char *image,unsigned char *archetype,unsigned state,
                          float initial,unsigned rate,float scale,float surface,unsigned changing,
                          unsigned span,float out[15][3]) {
    _Alignas(16) unsigned char kart[0x400]={0},entity[0x400]={0},message[0x40]={0};
    unsigned char handle[0x40]={0},surface_handle[0x40]={0},surface_data[0x40]={0};
    unsigned char clock[0x40]={0},vtable[0x300]={0},transform[0x80]={0};
    void *frame_clock=clock;timestep_clock=&frame_clock;
    put_ptr(image,motion_clock_address,clock);
    put_ptr(vtable,0xb8,entity_changed);put_ptr(entity,0,vtable);
    put_ptr(handle,0x28,archetype);put_ptr(kart,0xb8,handle);put_ptr(kart,0x18,entity);
    put_ptr(surface_handle,0x28,surface_data);put_ptr(kart,0x180,surface_handle);put_float(surface_data,0x18,surface);
    put_float(kart,0x210,1);kart[0x214]=kart[0x215]=1;kart[0x34]=1;
    put_int(kart,0xe0,state);put_int(kart,0x1a4,-1);
    const float yaw=changing ? .7f : 0;
    put_ptr(entity,0x30,transform);put_float(transform,0x68,yaw);
    put_float(transform,0x18,sinf(yaw*.5f));put_float(transform,0x1c,cosf(yaw*.5f));
    for(size_t i=0x6c;i<=0x78;i+=4)put_float(transform,i,1);
    put_float(entity,0x60,1600*cosf(yaw));put_float(entity,0x64,1600*sinf(yaw));put_float(entity,0x74,initial);
    double elapsed=0;
    for(unsigned tick=0;tick<15;++tick) {
        put_float(kart,0x30,changing ? (const float[]){.6f,-.6f,0,.3f,-1}[tick/3] : .6f);
        const unsigned steps=rate ? rate*span/30 : 3;
        for(unsigned sub=0;sub<steps;++sub) {
            const float dt=rate ? 1.f/rate : (const float[]){1.f/240,1.f/80,1.f/60}[sub];
            elapsed+=dt;put_int(clock,0x20,100000+(int)(elapsed*scale*8192));put_float(clock,0x18,dt);
            put_float(message,0x18,dt*scale);
            ((void (ABI *)(void *,void *,void *))(image+0xe454d0))(kart,entity,message);
        }
        memcpy(out[tick],entity+0x6c,3*sizeof(float));
    }
    timestep_clock=NULL;
}
static void test_turns(unsigned char *image,unsigned char *asset,const uint32_t *offsets,unsigned count,size_t size) {
    unsigned char native[15],patched[15];memcpy(native,image+0xe45951,15);
    assert(angular_timestep_contract(image,size));
    image[0xe45951]^=1;assert(!angular_timestep_contract(image,size));image[0xe45951]^=1;
    const size_t math_init=!memcmp(image+0x1488a30,"\xc5\xf8\x10\x05",4) ? 0x1488a30 : 0x1488a20;
    assert(!memcmp(image+math_init,"\xc5\xf8\x10\x05",4) && image[math_init+0x3c3]==0xc3);
    ((void (ABI *)(void))(image+math_init))();
    unsigned char *bridge=mmap(NULL,576,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(bridge!=MAP_FAILED);memset(bridge,0xcc,576);
    install_angular_timestep(image,bridge);memcpy(patched,image+0xe45951,15);
    for(unsigned i=512;i<576;++i)assert(bridge[i]==0xcc);
    const unsigned rates[]={30,60,120,240,0,72,144},states[]={0,3,4};
    const float starts[]={-500,-100,0,100,500},scales[]={.125f,.25f,.5f,1,1.5f,2},surfaces[]={0,.25f,.75f};
    unsigned cases=0,exact=0;double worst=0,raw_worst=0;
    for(unsigned a=0;a<count;++a)for(unsigned st=0;st<3;++st)for(unsigned o=0;o<5;++o)
    for(unsigned s=0;s<6;++s)for(unsigned m=0;m<3;++m)for(unsigned changing=0;changing<2;++changing) {
        for(unsigned r=0;r<sizeof rates/sizeof *rates;++r) {
            const unsigned span=rates[r]%30 ? 5 : 1;
            float reference[15][3],raw[15][3],fixed[15][3];memcpy(image+0xe45951,native,15);
            turn_simulate(image,asset+offsets[a],states[st],starts[o],30,scales[s],surfaces[m],changing,span,reference);
            turn_simulate(image,asset+offsets[a],states[st],starts[o],rates[r],scales[s],surfaces[m],changing,span,raw);
            memcpy(image+0xe45951,patched,15);
            turn_simulate(image,asset+offsets[a],states[st],starts[o],rates[r],scales[s],surfaces[m],changing,span,fixed);
            if(!r){assert(!memcmp(reference,fixed,sizeof reference));exact++;}
            for(unsigned t=0;t<15;++t)for(unsigned axis=0;axis<3;++axis) {
                double e=fabs(fixed[t][axis]-reference[t][axis]);assert(isfinite(e));
                raw_worst=fmax(raw_worst,fabs(raw[t][axis]-reference[t][axis]));worst=fmax(worst,e);
                /* Longer 72/144 Hz runs retain native float accumulation in linear-only paths. */
                assert(e<.003 || (e<.005 && fixed[t][axis]==raw[t][axis]));
            }
            cases++;
        }
    }
    assert(raw_worst>100);
    printf("native angular controller: %u cases, %u exact30; native gap %.6f corrected %.6f\n",cases,exact,raw_worst,worst);
    memcpy(image+0xe45951,native,15);assert(!munmap(bridge,576));
}

int main(int argc, char **argv) {
    assert(argc==3);
    size_t length;
    unsigned char *elf=read_blob(argv[1],&length);
    assert(length>=sizeof(Elf64_Ehdr));
    const Elf64_Ehdr *header=(const void *)elf;
    const Elf64_Phdr *segments=(const void *)(elf+header->e_phoff);
    size_t size=0;
    for (unsigned i=0;i<header->e_phnum;i++) if (segments[i].p_type==PT_LOAD &&
        segments[i].p_vaddr+segments[i].p_memsz>size) size=segments[i].p_vaddr+segments[i].p_memsz;
    unsigned char *image=mmap(NULL,size+4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(image!=MAP_FAILED);
    for (unsigned i=0;i<header->e_phnum;i++) if (segments[i].p_type==PT_LOAD)
        memcpy(image+segments[i].p_vaddr,elf+segments[i].p_offset,segments[i].p_filesz);
    uint64_t cookie=0x1234;
    int32_t displacement; memcpy(&displacement,image+0xe4a234,4);
    put_ptr(image,0xe4a238+(int64_t)displacement,&cookie);
    native_clock_reference(image);
    unsigned char *clock_bridge=test_scaled_clock(image,size);
    assert(!memcmp(image+0xe468af,"\x48\x8b\x05",3));
    memcpy(&displacement,image+0xe468b2,4);
    motion_clock_address=(size_t)(0xe468b6+(int64_t)displacement);
    jump(call_target(image,0xe4a292),no_component);
    /* Python verifies these two imported names before executing this fixture. */
    jump(call_target(image,0xe462fe),sinf); jump(call_target(image,0xe46313),cosf);
    native_kart_speed=(KartSpeed)(image+0xe4a220);
    component=no_component; void *type=(void *)1; boost_type=&type;
    unsigned char *deceleration_code=mmap(NULL,576,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(deceleration_code!=MAP_FAILED); memset(deceleration_code,0xcc,576);
    assert(deceleration_timestep_contract(image,size));
    image[0xe46a55]^=1; assert(!deceleration_timestep_contract(image,size)); image[0xe46a55]^=1;
    memcpy(native_deceleration,image+0xe46a55,18);
    install_deceleration_timestep(image,deceleration_code);
    memcpy(patched_deceleration,image+0xe46a55,18);
    for(unsigned i=512;i<576;++i) assert(deceleration_code[i]==0xcc);
    unsigned char *project_code=mmap(NULL,576,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(project_code!=MAP_FAILED);memset(project_code,0xcc,576);
    assert(ground_speed_project_contract(image,size));
    for(unsigned i=0;i<sizeof ground_speed_project_code;++i) {
        image[0xe46acf+i]^=1;assert(!ground_speed_project_contract(image,size));image[0xe46acf+i]^=1;
    }
    for(unsigned i=0;i<sizeof ground_boost_layout/sizeof *ground_boost_layout;++i)
        for(unsigned j=0;j<ground_boost_layout[i].length;++j) {
            const size_t address=ground_boost_layout[i].address+j;
            image[address]^=1;assert(!ground_speed_project_contract(image,size));image[address]^=1;
        }
    memcpy(native_ground_project,image+0xe46acf,15);
    install_ground_speed_project(image,project_code);memcpy(patched_ground_project,image+0xe46acf,15);
    for(unsigned i=512;i<576;++i)assert(project_code[i]==0xcc);
    unsigned char *lateral_code=mmap(NULL,1152,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(lateral_code!=MAP_FAILED);memset(lateral_code,0xcc,1152);
    memcpy(native_lateral[0],image+0xe46b2a,15);memcpy(native_lateral[1],image+0xe47237,15);
    timestep_bridge(lateral_code,image+0xe46b39,native_lateral[0],11,-0x34,lateral_retention);
    timestep_bridge(lateral_code+512,image+0xe47246,native_lateral[1],3,-0x34,lateral_retention);
    jump(image+0xe46b2a,lateral_code);image[0xe46b38]=0x90;
    jump(image+0xe47237,lateral_code+512);image[0xe47245]=0x90;
    memcpy(patched_lateral[0],image+0xe46b2a,15);memcpy(patched_lateral[1],image+0xe47237,15);
    /* Capture dt in the same unused Euler padding as the production motion entry. */
    memcpy(lateral_code+1024,image+0xe46250,20);memcpy(lateral_code+1044,"\x8b\x42\x18\x89\x45\xcc",6);
    jump(lateral_code+1050,image+0xe46264);jump(image+0xe46250,lateral_code+1024);
    for(unsigned i=1064;i<1152;++i)assert(lateral_code[i]==0xcc);
    jump(image+size,motion_speed);
    const size_t calls[]={0xe4683a,0xe47361,0xe47504,0xe4761a,0xe476e7,
                          0xe47873,0xe47da1,0xe47db9,0xe47e16,0xe47e48};
    for (unsigned i=0;i<sizeof calls/sizeof *calls;i++) {
        assert(call_target(image,calls[i])==image+0xe4a220);
        const int64_t relative=(int64_t)size-(int64_t)calls[i]-5;
        assert(relative>=INT32_MIN && relative<=INT32_MAX);
        displacement=(int32_t)relative; memcpy(image+calls[i]+1,&displacement,4);
    }
    unsigned char *fixture=read_blob(argv[2],&length);
    const uint32_t asset_size=(uint32_t)integer(fixture,0);
    assert(asset_size>0 && (size_t)asset_size+12<=length);
    unsigned char *asset=fixture+4;
    size_t at=4+asset_size;
    const uint32_t relocations=(uint32_t)integer(fixture,at); at+=4;
    assert(at+(uint64_t)relocations*4+4<=length);
    for (unsigned i=0;i<relocations;i++,at+=4) {
        const uint32_t offset=(uint32_t)integer(fixture,at);
        assert((uint64_t)offset+8<=asset_size);
        uintptr_t target; memcpy(&target,asset+offset,8); assert(target<asset_size);
        if (target) put_ptr(asset,offset,asset+target);
    }
    const uint32_t count=(uint32_t)integer(fixture,at); at+=4;
    assert(count==8 && at+(uint64_t)count*4==length);
    const unsigned rates[]={30,32,60,64,120,128,144,240,256,0};
    const float scales[]={.25f,.5f,1,2};
    const float starts[]={0,1600,1600,1600,1600,4000,-500};
    unsigned cases=0,exact=0;
    double native_gap=0,remaining_gap=0,overspeed_gap=0,acceleration_gap=0,deceleration_gap=0;
    for (fixed_clock=0;fixed_clock<2;++fixed_clock) for (unsigned a=0;a<count;a++) {
        const uint32_t offset=(uint32_t)integer(fixture,at+4*a);
        assert((uint64_t)offset+0xb0<=asset_size);
        for (unsigned scenario=0;scenario<11;scenario++) for (unsigned action=0;action<7;action++)
        for (unsigned s=0;s<4;s++) {
            float reference[5]; corrected=0;
            simulate(image,asset+offset,scenario,action,30,scales[s],starts[action],30,reference);
            for (unsigned r=0;r<sizeof rates/sizeof *rates;r++) {
                float raw[5],fixed[5]; corrected=0;
                simulate(image,asset+offset,scenario,action,rates[r],scales[s],starts[action],30,raw);
                corrected=1; simulate(image,asset+offset,scenario,action,rates[r],scales[s],starts[action],30,fixed);
                for (unsigned axis=0;axis<5;axis++) {
                    assert(isfinite(raw[axis]) && isfinite(fixed[axis]));
                    native_gap=fmax(native_gap,fabs(raw[axis]-reference[axis]));
                    remaining_gap=fmax(remaining_gap,fabs(fixed[axis]-reference[axis]));
                    if ((action==1 || action==2 || action==6) && scenario!=2 && scenario!=6 && scenario!=10) {
                        const double error=fabs(fixed[axis]-reference[axis]);
                        deceleration_gap=fmax(deceleration_gap,error); assert(error<.003);
                    }
                    if (action==0 && scenario!=10) {
                        const double error=fabs(fixed[axis]-reference[axis]);
                        acceleration_gap=fmax(acceleration_gap,error); assert(error<.1);
                    }
                    if (action==5 && scenario!=3 && scenario!=10) {
                        const double error=fabs(fixed[axis]-reference[axis]);
                        overspeed_gap=fmax(overspeed_gap,error); assert(error<.1);
                    }
                }
                if (rates[r]==30) { assert(!memcmp(reference,fixed,sizeof reference)); ++exact; }
                ++cases;
            }
        }
    }
    assert(native_gap>100); assert(remaining_gap<.01);
    printf("native linear motion: %u cases, %u bit-identical 30 FPS; overspeed gap %.6f; "
           "grounded speed gap %.6f (native %.6f)\n",cases,exact,overspeed_gap,remaining_gap,native_gap);
    printf("ordinary acceleration maximum gap %.6f; deceleration %.6f\n",acceleration_gap,deceleration_gap);
    unsigned boundaries=0; double boundary_gap=0;
    const float boundary_starts[]={-500,-51,-50,-49,0,49,50,51,500,1600};
    const unsigned durations[]={1,2,3,4,10,20,30,31,32,60},boundary_rates[]={30,60,120,240,0};
    for(fixed_clock=0;fixed_clock<2;++fixed_clock)for(unsigned a=0;a<count;++a)
    for(unsigned st=0;st<10;++st)for(unsigned act=1;act<=2;++act)for(unsigned d=0;d<10;++d) {
        float ref[5],value[5]; corrected=0;
        unsigned offset=(unsigned)integer(fixture,at+4*a);
        simulate(image,asset+offset,1,act,30,1,boundary_starts[st],durations[d],ref);
        for(unsigned rate=0;rate<5;++rate) {
            corrected=1; simulate(image,asset+offset,1,act,boundary_rates[rate],1,boundary_starts[st],durations[d],value);
            for(unsigned axis=0;axis<3;++axis) {
                double error=fabs(value[axis]-ref[axis]);boundary_gap=fmax(boundary_gap,error);
                assert(error<.003);
            }
            boundaries++;
        }
    }
    printf("boundary cases %u maxgap %.9g\n",boundaries,boundary_gap);
    const float control_starts[]={-1600,-51,-50,-49,0,49,50,51,500,1600,4000};
    const unsigned control_surfaces[]={1,9,10};
    unsigned control_cases=0,control_exact=0; double control_gap=0;
    for(fixed_clock=0;fixed_clock<2;++fixed_clock)for(unsigned a=0;a<count;++a)
    for(unsigned action=7;action<15;++action)for(unsigned start=0;start<11;++start)
    for(unsigned surface=0;surface<3;++surface) {
        const unsigned offset=(unsigned)integer(fixture,at+4*a);
        float ref[5];corrected=0;
        simulate(image,asset+offset,control_surfaces[surface],action,30,1,control_starts[start],30,ref);
        for(unsigned rate=0;rate<5;++rate) {
            float value[5];corrected=1;
            simulate(image,asset+offset,control_surfaces[surface],action,boundary_rates[rate],1,control_starts[start],30,value);
            for(unsigned axis=0;axis<5;++axis) {
                const double error=fabs(value[axis]-ref[axis]);assert(error<.01);control_gap=fmax(control_gap,error);
            }
            if(!rate){assert(!memcmp(ref,value,sizeof ref));control_exact++;}control_cases++;
        }
    }
    printf("combined controls and surfaces: %u cases, %u exact30, velocity/base/normalized gap %.9g\n",
           control_cases,control_exact,control_gap);
    lateral_speed=500;surface_retention=.6f;
    const float sideways_starts[]={0,1600,4000};const unsigned sideways_durations[]={1,30};
    unsigned sideways_cases=0,sideways_exact=0;double sideways_gap=0;
    for(fixed_clock=0;fixed_clock<2;++fixed_clock)for(unsigned a=0;a<count;++a)
    for(unsigned action=7;action<15;++action)for(unsigned start=0;start<3;++start)
    for(unsigned surface=0;surface<3;++surface)for(unsigned duration=0;duration<2;++duration) {
        const unsigned offset=(unsigned)integer(fixture,at+4*a);
        float ref[5];corrected=0;
        simulate(image,asset+offset,control_surfaces[surface],action,30,1,sideways_starts[start],sideways_durations[duration],ref);
        for(unsigned rate=0;rate<5;++rate) {
            float value[5];corrected=1;
            simulate(image,asset+offset,control_surfaces[surface],action,boundary_rates[rate],1,
                     sideways_starts[start],sideways_durations[duration],value);
            for(unsigned axis=0;axis<5;++axis) {
                const double error=fabs(value[axis]-ref[axis]);assert(error<.01);sideways_gap=fmax(sideways_gap,error);
            }
            if(!rate){assert(!memcmp(ref,value,sizeof ref));sideways_exact++;}sideways_cases++;
        }
    }
    const float boosts[][3]={{250,600,400},{1000,1000,1000},{1000,3000,1200},
                              {2000,6000,2500},{1000,500,1500}};
    unsigned boost_cases=0,boost_exact=0;double boost_gap=0,boost_speed_gap=0,boost_angle_gap=0;
    for(fixed_clock=0;fixed_clock<2;++fixed_clock)for(unsigned a=0;a<count;++a)
    for(unsigned action=7;action<15;++action)for(unsigned config=0;config<5;++config)
    for(unsigned surface=0;surface<3;++surface)for(unsigned duration=0;duration<2;++duration) {
        const unsigned offset=(unsigned)integer(fixture,at+4*a);
        memcpy(boost_parameters,boosts[config],sizeof boost_parameters);
        float ref[5];corrected=0;
        simulate(image,asset+offset,control_surfaces[surface],action,30,1,4000,sideways_durations[duration],ref);
        for(unsigned rate=0;rate<5;++rate) {
            float value[5];corrected=1;
            simulate(image,asset+offset,control_surfaces[surface],action,boundary_rates[rate],1,
                     4000,sideways_durations[duration],value);
            double reference_length=0,actual_length=0,dot=0,cross_length=0;
            for(unsigned axis=0;axis<3;++axis) {
                reference_length+=(double)ref[axis]*ref[axis];actual_length+=(double)value[axis]*value[axis];
                dot+=(double)ref[axis]*value[axis];
                const unsigned j=(axis+1)%3,k=(axis+2)%3;
                const double cross=(double)ref[j]*value[k]-(double)ref[k]*value[j];cross_length+=cross*cross;
            }
            const double speed_error=fabs(sqrt(actual_length)-sqrt(reference_length));
            const double angle_error=atan2(sqrt(cross_length),dot);
            assert(speed_error<.003 && angle_error<1e-5);
            boost_speed_gap=fmax(boost_speed_gap,speed_error);boost_angle_gap=fmax(boost_angle_gap,angle_error);
            /* Repeated native direction normalization has a separately measured float residual. */
            for(unsigned axis=0;axis<5;++axis) {
                const double error=fabs(value[axis]-ref[axis]);
                if(axis==3)assert(error<.003);
                if(axis==4)assert(error<2e-6);
                boost_gap=fmax(boost_gap,error);
            }
            if(!rate){assert(!memcmp(ref,value,sizeof ref));boost_exact++;}boost_cases++;
        }
    }
    printf("steady boost: %u cases, %u exact30; speed gap %.9g, direction %.9g rad, component %.9g\n",
           boost_cases,boost_exact,boost_speed_gap,boost_angle_gap,boost_gap);
    lateral_speed=0;boost_parameters[0]=1000;boost_parameters[1]=3000;boost_parameters[2]=0;
    for(boost_transition=1;boost_transition<=2;++boost_transition)
    for(fixed_clock=0;fixed_clock<2;++fixed_clock)for(unsigned a=0;a<count;++a)
    for(unsigned rate=0;rate<5;++rate) {
        const unsigned offset=(unsigned)integer(fixture,at+4*a);
        float raw[5],value[5];corrected=0;
        simulate(image,asset+offset,0,9,boundary_rates[rate],1,3000,2,raw);
        corrected=1;simulate(image,asset+offset,0,9,boundary_rates[rate],1,3000,2,value);
        assert(raw[0]==(boost_transition==1 ? 1000 : 5000) && value[0]==raw[0]);
        if(!rate)assert(!memcmp(raw,value,sizeof raw));
    }
    boost_transition=0;memset(boost_parameters,0,sizeof boost_parameters);
    puts("boost cap/floor transitions: 160 native immediate clamps preserved");
    lateral_speed=surface_retention=0;
    printf("sideways motion: %u cases, %u exact30; maximum gap %.9g\n",sideways_cases,sideways_exact,sideways_gap);
    test_turns(image,asset,(const uint32_t *)(fixture+at),count,size);
    assert(!munmap(lateral_code,1152)); assert(!munmap(project_code,576)); assert(!munmap(deceleration_code,576)); assert(!munmap(clock_bridge,576));
    free(fixture); free(elf); assert(!munmap(image,size+4096));
    return 0;
}
