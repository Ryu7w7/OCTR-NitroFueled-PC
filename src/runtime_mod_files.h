/* Explicit read-only mod file redirections. No game tree, links or copies.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_RUNTIME_MOD_FILES_H
#define BB_RUNTIME_MOD_FILES_H
typedef struct { char *guest, *host; } ModFile;
static ModFile *mod_files;
static uint32_t mod_file_count;
static int mod_files_error;
static pthread_once_t mod_files_once=PTHREAD_ONCE_INIT;
static int compare_mod_files(const void *a,const void *b) {
    return strcasecmp(((const ModFile*)a)->guest,((const ModFile*)b)->guest);
}

static uint32_t mod_u32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static const char *mod_relative(const char *guest) {
    if (!guest || !*guest) return NULL;
    if (*guest!='/') return guest;
    if (!strncmp(guest,"/app0",5) && (!guest[5] || guest[5]=='/')) return guest+5+(guest[5]=='/');
    if (!strncmp(guest,"/hostapp",8) && (!guest[8] || guest[8]=='/')) return guest+8+(guest[8]=='/');
    return NULL;
}
static int mod_safe_relative(const char *p) {
    if (!*p || *p=='/' || strchr(p,'\\') || strchr(p,':')) return 0;
    for (const char *s=p;;) {
        const char *e=strchr(s,'/'); size_t n=e ? (size_t)(e-s) : strlen(s);
        if (!n || (n==1 && *s=='.') || (n==2 && s[0]=='.' && s[1]=='.')) return 0;
        if (!e) break;
        s=e+1;
    }
    return 1;
}
static void load_mod_files(void) {
    const char *name=getenv("BB_MOD_FILE_MAP");
    if (!name || !*name) return;
    mod_files_error=EIO;
    FILE *fp=fopen(name,"rb");
    if (!fp) return;
    unsigned char header[12];
    if (fread(header,1,12,fp)!=12 || memcmp(header,"BBFILE01",8)) { fclose(fp); return; }
    uint32_t count=mod_u32(header+8);
    if (count>65536) { fclose(fp); return; }
    ModFile *items=calloc(count ? count : 1,sizeof(*items));
    if (!items) { fclose(fp); return; }
    uint32_t i=0;
    for (;i<count;++i) {
        unsigned char sizes[8];
        if (fread(sizes,1,8,fp)!=8) break;
        uint32_t a=mod_u32(sizes),b=mod_u32(sizes+4);
        if (!a || a>=1024 || !b || b>=4096) break;
        items[i].guest=calloc(a+1,1);items[i].host=calloc(b+1,1);
        if (!items[i].guest || !items[i].host) break;
        if (fread(items[i].guest,1,a,fp)!=a || fread(items[i].host,1,b,fp)!=b ||
            strlen(items[i].guest)!=a || strlen(items[i].host)!=b || !mod_safe_relative(items[i].guest)) break;
    }
    int valid=i==count && fgetc(fp)==EOF;
    fclose(fp);
    if (valid) {
        qsort(items,count,sizeof(*items),compare_mod_files);
        for(uint32_t j=1;j<count;++j) if(!strcasecmp(items[j-1].guest,items[j].guest)) { valid=0;break; }
    }
    if (!valid) {
        for (uint32_t j=0;j<count;++j) { free(items[j].guest);free(items[j].host); }
        free(items);return;
    }
    mod_files=items;mod_file_count=count;mod_files_error=0;
}
/* Exact files override source reads. Directory fallback is an empty directory
 * hierarchy created from mod target names, never populated with game files. */
static int mod_file_translate(const char *guest,char *out,size_t size) {
    const char *relative=mod_relative(guest);
    if (!relative) return 0;
    pthread_once(&mod_files_once,load_mod_files);
    if (mod_files_error) return -mod_files_error;
    ModFile key={.guest=(char*)relative};
    const ModFile *file=bsearch(&key,mod_files,mod_file_count,sizeof(*mod_files),compare_mod_files);
    if (file) {
        if ((size_t)snprintf(out,size,"%s",file->host)>=size) return -ENAMETOOLONG;
        return 1;
    }
    HostStat st;
    const char *root=getenv("BB_MOD_FILE_DIRS");
    if (root && host_stat(out,&st)) {
        size_t n=strlen(relative);
        for (uint32_t i=0;i<mod_file_count;++i) {
            if (!n || (!strncasecmp(relative,mod_files[i].guest,n) && mod_files[i].guest[n]=='/')) {
                if ((size_t)snprintf(out,size,"%s/%s",root,relative)>=size) return -ENAMETOOLONG;
                return 1;
            }
        }
    }
    return 0;
}
#endif
