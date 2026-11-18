/* Split-screen players 2-4 save as guests (src/runtime_savedata.c): savedata/guest<N>, emptied the
 * first time a run asks for it; player 1 keeps savedata/1 untouched. */
#include "../src/runtime_savedata.c"

#include <assert.h>

static char user_dir[256];
const char *runtime_file_progress_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
void runtime_file_unmount(const char *guest) { (void)guest; }
void runtime_reserves_bar_save_closed(void) {}
static int replay_character_on;
int runtime_replay_keeps_save_aside(void) { return replay_character_on; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void touch(const char *path) {
    make_dirs(path);
    char file[900];
    snprintf(file,sizeof file,"%s/data",path);
    FILE *f=fopen(file,"w");
    assert(f);
    fputs("x",f);
    fclose(f);
}
static int exists(const char *path) { struct stat st; return !stat(path,&st); }

int main(void) {
    snprintf(user_dir,sizeof user_dir,"/tmp/bbport-savedata-guest-%d",(int)getpid());
    char p1[700], p2[700], again[720], file[900];
    /* Saves from earlier runs: player 1's, the old profile folder of player 2, a guest's. */
    snprintf(p1,sizeof p1,"%s/savedata/1/CUSA13795/SAVE",user_dir); touch(p1);
    snprintf(p2,sizeof p2,"%s/savedata/2/CUSA13795/SAVE",user_dir); touch(p2);
    snprintf(p2,sizeof p2,"%s/savedata/guest2/CUSA13795/SAVE",user_dir); touch(p2);

    root(1,"CUSA13795",p1,sizeof p1);
    assert(!strcmp(p1+strlen(user_dir),"/savedata/1/CUSA13795"));
    root(2,"CUSA13795",p2,sizeof p2);
    assert(!strcmp(p2+strlen(user_dir),"/savedata/guest2/CUSA13795"));
    snprintf(file,sizeof file,"%s/SAVE/data",p2);
    assert(!exists(file));                     /* the last run's guest save is gone */
    snprintf(file,sizeof file,"%s/SAVE/data",p1);
    assert(exists(file));                      /* player 1's is kept */
    snprintf(file,sizeof file,"%s/savedata/2/CUSA13795/SAVE/data",user_dir);
    assert(exists(file));                      /* an older profile folder is left alone */
    /* Within a run the guest's save stays. */
    snprintf(again,sizeof again,"%s/SAVE",p2); touch(again);
    root(2,"CUSA13795",again,sizeof again);
    snprintf(file,sizeof file,"%s/SAVE/data",again);
    assert(exists(file));
    root(4,NULL,again,sizeof again);
    assert(strstr(again,"/savedata/guest4/"));

    /* The game's saves are the folders with their .sce_sys: the replays' folder beside them is not one. */
    initialized = 1;
    runtime_savedata_configure("CUSA13795");  /* a search names no title: the game's own */
    char replays[720], sys[720];
    snprintf(replays,sizeof replays,"%s/RaceReplay",p1); touch(replays);
    snprintf(sys,sizeof sys,"%s/SAVE.sce_sys",p1); make_dirs(sys);
    { Param empty={0}; write_param(sys,&empty); }
    DirName names[4]; memset(names,0,sizeof names);
    SearchCond cond; memset(&cond,0,sizeof cond); cond.user = 1;
    SearchResult found; memset(&found,0,sizeof found); found.names = names; found.names_capacity = 4;
    assert(save_search(&cond,&found) == 0 && found.hits == 1 && !strcmp(names[0].data,"SAVE"));

    /* With a replay's character on the player, a writable mount is a copy: the own save stays. */
    DirName save; memset(&save,0,sizeof save); snprintf(save.data,sizeof save.data,"SAVE");
    MountResult mounted;
    replay_character_on = 1;
    assert(mount(1,"CUSA13795",&save,0,&mounted) == 0);
    const int slot = slot_of(&mounted.point);
    assert(slot >= 0 && strstr(slots[slot].host,"/replay-aside/SAVE"));
    snprintf(file,sizeof file,"%s/data",slots[slot].host);
    FILE *w = fopen(file,"w"); assert(w); fputs("someone else's",w); fclose(w);
    assert(save_umount(&mounted.point) == 0);
    snprintf(file,sizeof file,"%s/SAVE/data",p1);
    char kept[32] = ""; FILE *r = fopen(file,"r"); assert(r); assert(fgets(kept,sizeof kept,r)); fclose(r);
    assert(!strcmp(kept,"x"));
    /* given back: the own save again */
    replay_character_on = 0;
    assert(mount(1,"CUSA13795",&save,0,&mounted) == 0);
    assert(!strstr(slots[slot_of(&mounted.point)].host,"replay-aside"));
    assert(save_umount(&mounted.point) == 0);

    remove_tree(user_dir);
    puts("PASS: players 2-4 save as guests, emptied each run; player 1's saves untouched");
    puts("PASS: replays are not listed as saves; a replay's character never reaches the own save");
    return 0;
}
