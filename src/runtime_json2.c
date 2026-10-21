/* SacredFire: libSceJson2 entry points the game imports. The game parses JSON with its own code
 * and only touches the library's set-up classes: it derives its allocator from
 * sce::Json::MemAllocator (an abstract base: virtual allocate/deallocate/notifyError) and runs
 * sce::Json::Initializer::initialize/terminate with it. The base constructor installs a vtable
 * whose entries must never run (the derived class overrides them); notifyError is the base
 * class's default, which only reports. Itanium C++ ABI: `this` is the first argument. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ABI void pure_virtual(void) {
    fputs("STOP: sce::Json::MemAllocator base virtual called (the game's override is missing)\n",stderr);
    exit(21);
}
static ABI void base_dtor(void *self) { (void)self; }
/* Default notifyError: the library reports allocation failures through the allocator; nothing to do. */
static unsigned notified; /* __atomic builtins (clang rejects them on _Atomic objects) */
static ABI void notify_error(void *self,int32_t code,uint64_t size,void *user) {
    (void)self; (void)user;
    if (__atomic_fetch_add(&notified,1,__ATOMIC_RELAXED)<4) printf("Json2: MemAllocator::notifyError(%d, %llu bytes)\n",code,(unsigned long long)size);
}
/* Vtable: offset-to-top, typeinfo, then the virtual functions in declaration order:
 * ~MemAllocator (complete and deleting), allocate, deallocate, notifyError. */
static void *const memallocator_vtable[]={
    0,0,(void *)base_dtor,(void *)base_dtor,(void *)pure_virtual,(void *)pure_virtual,(void *)notify_error,
};
static ABI void memallocator_ctor(void **self) { if (self) *self=(void *)&memallocator_vtable[2]; }
static ABI void memallocator_dtor(void *self) { (void)self; }

/* Initializer: records the allocator the game hands in; the object itself holds nothing we use. */
typedef struct { void *allocator, *user; uint64_t buffer_size; } JsonInitParameter;
static const JsonInitParameter *json_parameter;
static ABI void initializer_ctor(void *self) { (void)self; }
static ABI void initializer_dtor(void *self) { (void)self; }
static ABI int32_t initializer_initialize(void *self,const JsonInitParameter *p) {
    (void)self;
    if (!p || !p->allocator) return (int32_t)0x80920000|1; /* invalid argument */
    json_parameter=p;
    printf("Json2: initialized with the game's allocator (buffer %llu bytes)\n",(unsigned long long)p->buffer_size);
    return 0;
}
static ABI int32_t initializer_terminate(void *self) { (void)self; json_parameter=NULL; return 0; }

static const RuntimeExport exports[]={
    {"_ZN3sce4Json12MemAllocatorC2Ev",memallocator_ctor}, {"_ZN3sce4Json12MemAllocatorD2Ev",memallocator_dtor},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv",notify_error},
    {"_ZN3sce4Json11InitializerC1Ev",initializer_ctor}, {"_ZN3sce4Json11InitializerD1Ev",initializer_dtor},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE",initializer_initialize},
    {"_ZN3sce4Json11Initializer9terminateEv",initializer_terminate},
};
uintptr_t runtime_json2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
