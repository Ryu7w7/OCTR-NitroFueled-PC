/* Real UDP mesh: multiple guests, disabled ping UI, four CUP maps, and no-AI results. */
#include "../src/runtime_net.c"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <sys/wait.h>

static int endpoint(struct sockaddr_in *addr) {
    int fd=socket(AF_INET,SOCK_DGRAM,0); assert(fd>=0);
    *addr=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(fd,(struct sockaddr *)addr,sizeof *addr));
    socklen_t size=sizeof *addr; assert(!getsockname(fd,(struct sockaddr *)addr,&size));
    assert(!fcntl(fd,F_SETFL,O_NONBLOCK)); return fd;
}
static void exchange(int fd,const struct sockaddr_in peers[8],int index,int count,int full_humans) {
    char name[24]; snprintf(name,sizeof name,index ? "guest%d" : "host",index);
    setenv("BB_ONLINE_NAME",name,1);
    VSocket socket={.s=fd,.tunnel=1};
    for (int race=0;race<4;++race) {
        /* Repeat a track too: old nonces must not supply the new race's metadata. */
        const char *map=race%2 ? "cup-b" : "cup-a";
        runtime_ai_catchup_reset(map,"host",index==0);
        if (!full_humans) runtime_ai_catchup_require_peers(index==0 ? count-1 : 0);
        const uint64_t start=now_ms(), end=start+1200;
        const unsigned finished=(0xffu<<race)&0xff;
        int synchronized=0;
        while (now_ms()<end) {
            if (!index) {
                if (!full_humans) {
                    for (unsigned id=0;id<8;++id) runtime_ai_catchup_publish(id,.11f+.03f*id+.05f*race);
                    runtime_ai_catchup_level_publish(5);
                    runtime_ai_catchup_recovery_publish(3,1);
                }
                runtime_ai_catchup_results_publish(finished);
            }
            /* A delayed final guest must not be mistaken for a ready full roster. */
            if (index!=count-1 || now_ms()-start>=200) {
                for (int i=0;i<count;++i) if (i!=index) {
                    assert(sendto(fd,"G",1,0,(const struct sockaddr *)&peers[i],sizeof peers[i])==1);
                    ping_after_send(&socket,&peers[i]);
                }
                uint8_t data[256]; struct sockaddr_in from; socklen_t size=sizeof from;
                int n;
                while ((n=recvfrom(fd,data,sizeof data,0,(struct sockaddr *)&from,&size))>0)
                    if (!ping_take(&socket,data,n,&from)) assert(n==1 && data[0]=='G');
            }
            int matches=runtime_ai_catchup_results()==(int)finished;
            if (!full_humans) {
                matches=matches && runtime_ai_catchup_recovery(3);
                if (index) matches=matches && runtime_ai_catchup_level()==5;
                for (unsigned id=0;id<8;++id)
                    matches=matches && fabsf(runtime_ai_catchup_bonus(id)-(.11f+.03f*id+.05f*race))<.001f;
            }
            if (matches) synchronized=1;
            struct timespec delay={0,10000000}; nanosleep(&delay,NULL);
        }
        assert(synchronized);
    }
}
static void group(int count,int ping,int full_humans) {
    setenv("BB_ONLINE_PING",ping ? "1" : "0",1);
    struct sockaddr_in addresses[8]; int sockets[8]; pid_t children[8];
    for (int i=0;i<count;++i) sockets[i]=endpoint(&addresses[i]);
    for (int i=0;i<count;++i) {
        children[i]=fork(); assert(children[i]>=0);
        if (!children[i]) {
            for (int j=0;j<count;++j) if (j!=i) close(sockets[j]);
            exchange(sockets[i],addresses,i,count,full_humans);
            close(sockets[i]); _exit(0);
        }
    }
    for (int i=0;i<count;++i) close(sockets[i]);
    for (int i=0;i<count;++i) {
        int status;
        assert(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    printf("LAN sync: %d clients, ping=%d, full_humans=%d, four CUP maps passed\n",count,ping,full_humans);
    fflush(stdout);
}
int main(void) {
    setenv("BB_ONLINE","1",1); setenv("BB_ONLINE_SERVER","127.0.0.2",1);
    setenv("BB_ONLINE_TUNNEL","1",1);
    assert(!runtime_ai_catchup_discovery());
    group(2,1,0); group(3,1,0); group(4,0,0); group(8,0,1);
}
