/* Split-screen players 2-4 save as guests (src/runtime_savedata.c): savedata/guest<N>, emptied the
 * first time a run asks for it; player 1 keeps savedata/1 untouched. */
#include "../src/runtime_savedata.c"

#include <assert.h>

static char user_dir[256];
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
void runtime_file_unmount(const char *guest) { (void)guest; }
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

    remove_tree(user_dir);
    puts("PASS: players 2-4 save as guests, emptied each run; player 1's saves untouched");
    return 0;
}
