/* The port's new tracks whatever mods are on (26/11): their files before a mod's of the same name, the
 * port's archive at the first update number the game does not have, and "modified" online without them.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_cinematic_video(int delta) { (void)delta; }
void runtime_load_guard_note(const void *buffer, size_t size) { (void)buffer; (void)size; }
int runtime_online_mode_in_private(void) { return 0; }
int runtime_online_mode_private_cup_race(void) { return 0; }
int runtime_public_itemless_active(void) { return 0; }
int runtime_private_match_active(void) { return 0; }
int runtime_private_room_active(void) { return 0; }
int runtime_replay_no_crates(void) { return 0; }
void runtime_memory_note_write(uintptr_t address, uint64_t size) { (void)address; (void)size; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

static void put(const char *path,const char *text) {
    FILE *f=fopen(path,"wb"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void map_entry(FILE *f,const char *guest,const char *host) {
    uint32_t sizes[2]={(uint32_t)strlen(guest),(uint32_t)strlen(host)};
    assert(fwrite(sizes,4,2,f)==2 && fputs(guest,f)>=0 && fputs(host,f)>=0);
}
static void read_as(const char *guest,const char *want) {
    int fd=(int)do_open(guest,0,0); assert(fd>=3);
    char got[32]={0}; assert(do_read(fd,got,sizeof(got)-1)==(int64_t)strlen(want) && !strcmp(got,want));
    assert(!do_close(fd));
}

int main(void) {
    char root[]="/tmp/bbport-track-files-XXXXXX";
    assert(mkdtemp(root));
    char game[256],archives[512],user[512],p[1024],map[600],level[600],port[600],mod_level[600],mod_update[600];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(archives,sizeof(archives),"%s/archives",game);
    snprintf(user,sizeof(user),"%s/user",root);
    assert(!mkdir(game,0755) && !mkdir(archives,0755));
    snprintf(p,sizeof(p),"%s/update2.pak",archives); put(p,"game u2");
    snprintf(level,sizeof(level),"%s/t929.pak",root); put(level,"port level");
    snprintf(port,sizeof(port),"%s/port-update.pak",root); put(port,"port lists");
    snprintf(mod_level,sizeof(mod_level),"%s/mod-t929.pak",root); put(mod_level,"mod level");
    snprintf(mod_update,sizeof(mod_update),"%s/mod-update3.pak",root); put(mod_update,"mod u3");
    /* a mod session (scripts/mod_content.py): its own update3.pak and a file named like the track's */
    snprintf(map,sizeof(map),"%s/file-map.bin",root);
    FILE *f=fopen(map,"wb"); assert(f);
    assert(fwrite("BBFILE01\2\0\0\0",1,12,f)==12);
    map_entry(f,"archives/t929.pak",mod_level);
    map_entry(f,"archives/update3.pak",mod_update);
    assert(!fclose(f));
    setenv("BB_MOD_FILE_MAP",map,1);
    snprintf(p,sizeof(p),"archives/t929.pak=%s",level);
    setenv("BB_CONTENT_FILES",p,1);
    setenv("BB_CONTENT_REQUIRED","1",1);
    setenv("BB_GRAPHICS_TAB_ARCHIVE",port,1);
    setenv("BB_GRAPHICS_TAB_SLOT","update3.pak",1);  /* the script's count missed the mod's update3.pak */
    runtime_file_configure(game,user);
    runtime_graphics_tab_ready=1;

    assert(!runtime_port_content_ok());             /* the archive with the lists not served yet */
    read_as("/app0/archives/t929.pak","port level");  /* the track's own file, not the mod's */
    read_as("/app0/archives/update2.pak","game u2");
    read_as("/app0/archives/update3.pak","mod u3");   /* the mod keeps its update3.pak */
    read_as("/app0/archives/update4.pak","port lists"); /* the port's goes after it */
    GuestStat st;
    assert(do_stat("/app0/archives/update5.pak",&st)==-ENOENT);
    assert(runtime_port_content_ok());

    puts("Track files: before the mods' files, the port's archive after the mods' updates, content check PASS");
}
