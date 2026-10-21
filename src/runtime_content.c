/* Offline AppContent provider for the configured base-game profile.
 * Add-ons (DLC) without extra data are reported installed when the content profile lists them
 * (the player's own DLC packages, scripts/content_profile.py); no mounting or downloads. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
/* SceAppContentAddcontInfo: SceNpUnifiedEntitlementLabel (17 chars + 3 padding) and a status. */
typedef struct { char label[17]; char padding[3]; uint32_t status; } AddcontInfo;
_Static_assert(sizeof(AddcontInfo)==24,"SceAppContentAddcontInfo layout");
#define ADDCONT_INSTALLED 4u
static char addons[64][17];
static unsigned addon_count;
void runtime_content_add_addon(const char *label) {
    if (addon_count>=64) return;
    memcpy(addons[addon_count],label,16);
    addons[addon_count][16]=0;
    printf("Runtime: add-on %s (installed)\n",addons[addon_count]);
    ++addon_count;
}
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
/* SacredFire: sceSysmoduleLoadModuleInternalWithArg: internal module ids are 0x8000xxxx; the
 * module's start arguments are ignored and the module's start result is 0. */
static ABI int32_t module_load_internal(uint32_t id,uint64_t argc,const void *argv,uint32_t flags,const void *option,int32_t *result) {
    (void)argc; (void)argv; (void)flags; (void)option;
    if ((id&0xffff0000)!=0x80000000) return INVALID_ID;
    int32_t r=module_load((uint16_t)id);
    if (!r && result) *result=0;
    return r;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    printf("Runtime: AppContent initialized; offline base-game profile, %u add-on(s)\n",addon_count);
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    ++lists;
    /* Without a list: the number of add-ons. With one: up to capacity entries, never touching
     * the unused slots. BB_ADDCONT_STATUS overrides the status (tests). */
    if (!capacity || !list) { *hits=addon_count; return 0; }
    static int status_read; static uint32_t status=ADDCONT_INSTALLED;
    if (!status_read) { const char *v=getenv("BB_ADDCONT_STATUS"); if (v && *v) status=(uint32_t)strtoul(v,NULL,0); status_read=1; }
    AddcontInfo *out=list;
    uint32_t n=addon_count<capacity ? addon_count : capacity;
    for (uint32_t i=0;i<n;++i) {
        memset(&out[i],0,sizeof(out[i]));
        memcpy(out[i].label,addons[i],17);
        out[i].status=status;
    }
    if (hits) *hits=n;
    if (lists<=3) printf("Runtime: AppContent add-on list: %u of %u\n",n,addon_count);
    return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (runtime_nid_equals(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (runtime_nid_equals(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (runtime_nid_equals(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (runtime_nid_equals(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal;
    if (runtime_nid_equals(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (runtime_nid_equals(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (runtime_nid_equals(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
