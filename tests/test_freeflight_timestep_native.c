#define _GNU_SOURCE
#include "../src/runtime_race_options.c"

static unsigned char freeflight_clock[0x38];
static void *freeflight_clock_pointer=freeflight_clock;
#define TEST_API __attribute__((visibility("default")))
TEST_API void freeflight_test_setup(void *image) { freeflight_image=image; timestep_clock=&freeflight_clock_pointer; memset(freeflight_states,0,sizeof freeflight_states); atomic_store(&freeflight_disabled,0); }
TEST_API void freeflight_test_dt(float dt) { if (real(freeflight_clock,0x18)!=dt) memcpy(freeflight_clock+0x18,&dt,4); }
TEST_API void *freeflight_test_helper(unsigned i) {
    static void (*const f[])(TimestepRegisters *)={freeflight_capture,freeflight_force,freeflight_scalar_contact,freeflight_contact,freeflight_constraint,freeflight_finish};
    return i<6 ? (void *)f[i] : NULL;
}
TEST_API unsigned freeflight_test_active(void) {
    unsigned count=0;freeflight_enter();
    for(unsigned i=0;i<FREEFLIGHT_CAPACITY;++i) count+=freeflight_states[i].active;
    freeflight_leave();return count;
}
TEST_API int freeflight_test_contract(const unsigned char *p,size_t n) {return freeflight_timestep_contract(p,n);}
TEST_API void freeflight_test_install(unsigned char *p,unsigned char *code) {install_freeflight_timestep(p,code);}
TEST_API unsigned freeflight_test_layout(unsigned i, size_t *address,size_t *length) {
    if (i>=sizeof freeflight_layouts/sizeof *freeflight_layouts) return 0;
    *address=freeflight_layouts[i].address;*length=freeflight_layouts[i].length;return 1;
}
