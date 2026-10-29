/* Synthetic guest-filesystem regression harness; no game content. */
#include "../src/runtime_file.c"
#include <assert.h>
void runtime_cinematic_video(int delta) { (void)delta; }
int runtime_private_room_active(void) { return 0; }
int32_t *runtime_errno(void) { static int32_t value;return &value; }
int runtime_guest_errno(int value) { return value; }
void runtime_memory_note_write(uintptr_t address,uint64_t size) { (void)address;(void)size; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) { (void)table;(void)count;(void)nid;return 0; }
#ifdef _WIN32
int compat_errno_from_win32(unsigned long value) { (void)value;return EIO; }
ssize_t compat_pread(int fd,void *data,size_t n,int64_t offset) {
    int64_t old=_lseeki64(fd,0,SEEK_CUR);_lseeki64(fd,offset,SEEK_SET);
    ssize_t result=_read(fd,data,(unsigned)n);_lseeki64(fd,old,SEEK_SET);return result;
}
ssize_t compat_pwrite(int fd,const void *data,size_t n,int64_t offset) {
    int64_t old=_lseeki64(fd,0,SEEK_CUR);_lseeki64(fd,offset,SEEK_SET);
    ssize_t result=_write(fd,data,(unsigned)n);_lseeki64(fd,old,SEEK_SET);return result;
}
#endif
static void content(const char *path,const char *expected) {
    char bytes[64]={0};int fd=(int)do_open(path,0,0);assert(fd>=3);
    assert(do_read(fd,bytes,sizeof(bytes)-1)==(int64_t)strlen(expected));
    assert(!strcmp(bytes,expected));assert(!do_close(fd));
}
int main(int argc,char **argv) {
    assert(argc==4);runtime_file_configure(argv[1],argv[2]);
    if (!strncmp(argv[3],"menus-",6)) {
        int skip=!strcmp(argv[3],"menus-on");
        for (size_t i=0;i<2;++i) {
            const char *path=i ? "/app0/openinglogo.bk2" : "/app0/octane_intro_logo_atvi.bk2";
            if (skip) assert(do_open(path,0,0)==-ENOENT);
            else content(path,"INTRO");
        }
        content("/app0/race.bk2","RACE");
        assert(do_open("/app0/archives/update3.pak",0,0)==-ENOENT);
        runtime_graphics_tab_ready=1;
        content("/app0/archives/update3.pak","MENUS");
        char own[1024];snprintf(own,sizeof(own),"%s/archives/update3.pak",argv[1]);
        FILE *f=fopen(own,"w");assert(f);assert(fputs("OWN",f)>=0);assert(!fclose(f));
        content("/app0/archives/update3.pak","OWN");
        return 0;
    }
    if (!strcmp(argv[3],"broken")) { assert(do_open("/app0/data.txt",0,0)==-EIO);return 0; }
    int enabled=!strcmp(argv[3],"enabled");
    content("/app0/data.txt",enabled ? "MOD" : "SOURCE");
    content("/hostapp/data.txt",enabled ? "MOD" : "SOURCE");
    content("data.txt",enabled ? "MOD" : "SOURCE");
    content("/app0/unchanged.txt","UNCHANGED");
    assert(do_open("/app0/data.txt",1,0)==-EROFS);
    assert(do_open("data.txt",0x200,0)==-EROFS);
    assert(do_truncate("/app0/data.txt",0)==-EROFS);
    assert(do_rename("/app0/data.txt","/app0/x")==-EROFS);
    assert(path_op("/app0/data.txt",2,0)==-EROFS);
    assert(do_open("/app0/../data.txt",0,0)==-EACCES);
    if (enabled) {
        content("/app0/DATA.TXT","MOD");
        content("/app0/new/nested.txt","ADDED");
        GuestStat st;assert(!do_stat("/app0/data.txt",&st));assert(st.size==3);
        assert(!do_access("/app0/new/nested.txt",0));
        char dirpath[1024];assert(!translate("/app0",dirpath,sizeof(dirpath)));
        Listing *listing=list_directory(dirpath,"/app0");assert(listing);
        int data=0,original=0,added=0;
        for (size_t i=0;i<listing->count;++i) {
            const char *name=listing->names+listing->offsets[i];
            data+=!strcasecmp(name,"data.txt");original+=!strcmp(name,"unchanged.txt");added+=!strcmp(name,"new");
        }
        assert(data==1 && original==1 && added==1);free_listing(listing);
        assert(!translate("/app0/new",dirpath,sizeof(dirpath)));
        listing=list_directory(dirpath,"/app0/new");assert(listing);
        int nested=0;for(size_t i=0;i<listing->count;++i) nested+=!strcmp(listing->names+listing->offsets[i],"nested.txt");
        assert(nested==1);free_listing(listing);
    } else assert(do_open("/app0/new/nested.txt",0,0)==-ENOENT);
    return 0;
}
