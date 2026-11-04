#include "../src/runtime_ai_catchup.c"
#include <assert.h>
#include <stdio.h>

static void full_roster(void) {
    for (int count=2;count<=8;++count) {
        const uint64_t now=runtime_ai_catchup_now();
        CatchupSync guests[7]; uint8_t hello[AI_CATCHUP_PACKET], packet[AI_CATCHUP_PACKET];
        runtime_ai_catchup_reset("full-roster","host",1);
        runtime_ai_catchup_require_peers(count-1);
        runtime_ai_catchup_publish(3,.42f);
        CatchupSync host=sync;
        for (int i=0;i<count-1;++i) {
            char name[24]; snprintf(name,sizeof name,"guest%d",i);
            runtime_ai_catchup_reset("full-roster","host",0);
            assert(runtime_ai_catchup_send("host",hello,now)==sizeof hello);
            guests[i]=sync;
            sync=host;
            runtime_ai_catchup_receive(name,hello,sizeof hello,now);
            assert(runtime_ai_catchup_send(name,packet,now)==sizeof packet);
            assert(runtime_ai_catchup_ready()==(i==count-2));
            assert(packet[36]==(i==count-2 ? 42 : 0));
            host=sync;
        }
        for (int i=0;i<count-1;++i) {
            char name[24]; snprintf(name,sizeof name,"guest%d",i);
            sync=host;
            assert(runtime_ai_catchup_send(name,packet,now+51)==sizeof packet);
            sync=guests[i];
            runtime_ai_catchup_receive("host",packet,sizeof packet,now);
            assert(fabsf(runtime_ai_catchup_bonus(3)-.42f)<.001f);
        }
        sync=host;
        sync.peers[count-2].last_seen=now-1501;
        assert(!runtime_ai_catchup_ready()); /* Losing one guest cannot silently enable a partial room. */
        assert(runtime_ai_catchup_bonus(3)==0);
        runtime_ai_catchup_require_peers(count-2); /* Native roster confirms the departure. */
        assert(runtime_ai_catchup_ready());
    }
}

int main(void) {
    full_roster();
    for (int level=0;level<6;++level) {
        float close=runtime_ai_catchup_target(level,10,100,100,0);
        float far=runtime_ai_catchup_target(level,100,100,100,0);
        assert(close>0 && far>close);
        assert(runtime_ai_catchup_target(level,100,110,100,0)>far);
        if (level) assert(far>runtime_ai_catchup_target(level-1,100,100,100,0));
        assert(runtime_ai_catchup_target(level,INFINITY,100,100,0)==0);
        assert(runtime_ai_catchup_target(level,100,100,0,0)==0);
        assert(runtime_ai_catchup_target(level,100,0,100,0)==0);
    }
    assert(runtime_ai_catchup_target(5,100,100,100,0)<runtime_ai_catchup_target(5,100,100,100,.2f));
    /* A fast player is matched before the difficulty-limited closing margin. */
    assert(runtime_ai_catchup_target(2,100,300,100,0)>2);
    assert(runtime_ai_catchup_target(5,100000,1000,100,0)==2.5f);
    uint64_t now=runtime_ai_catchup_now();
    uint8_t hello[AI_CATCHUP_PACKET],packet[AI_CATCHUP_PACKET],old[AI_CATCHUP_PACKET];
    runtime_ai_catchup_reset("track1","host",1);
    assert(runtime_ai_catchup_discovery());
    runtime_ai_catchup_publish(3,.237f);
    runtime_ai_catchup_level_publish(5);
    assert(fabsf(runtime_ai_catchup_bonus(3)-.24f)<.0001f);
    runtime_ai_catchup_require_peers(1);
    assert(runtime_ai_catchup_discovery());
    assert(!runtime_ai_catchup_ready() && runtime_ai_catchup_bonus(3)==0);
    assert(runtime_ai_catchup_send("guest",packet,now)==0); /* Needs this race's challenge. */
    runtime_ai_catchup_recovery_publish(3,1);
    assert(runtime_ai_catchup_results()==-1);
    runtime_ai_catchup_results_publish(0x0a);
    runtime_ai_catchup_results_publish(0xff); /* Final mask is immutable. */
    assert(runtime_ai_catchup_results()==0x0a);
    CatchupSync host=sync;
    runtime_ai_catchup_reset("track1","host",0);
    assert(runtime_ai_catchup_discovery());
    assert(runtime_ai_catchup_send("stranger",hello,now)==0);
    assert(runtime_ai_catchup_send("host",hello,now)==sizeof hello);
    CatchupSync guest=sync;
    sync=host;
    assert(runtime_ai_catchup_receive("guest",hello,sizeof hello,now)==1);
    assert(runtime_ai_catchup_send("guest",packet,now)==sizeof packet);
    assert(runtime_ai_catchup_send("guest",old,now)==0); /* Bounded traffic. */
    assert(runtime_ai_catchup_ready());
    host=sync;
    sync=guest;
    runtime_ai_catchup_results_publish(0xff);
    assert(runtime_ai_catchup_results()==-1);
    assert(runtime_ai_catchup_receive("stranger",packet,sizeof packet,now)==1);
    assert(runtime_ai_catchup_results()==-1);
    assert(runtime_ai_catchup_bonus(3)==0);
    assert(runtime_ai_catchup_receive("host",packet,sizeof packet-1,now)==1);
    assert(runtime_ai_catchup_bonus(3)==0);
    assert(runtime_ai_catchup_receive("host",packet,sizeof packet,now)==1);
    assert(fabsf(runtime_ai_catchup_bonus(3)-.24f)<.0001f);
    assert(runtime_ai_catchup_bonus(2)==0);
    assert(runtime_ai_catchup_level()==5);
    assert(runtime_ai_catchup_recovery(3) && !runtime_ai_catchup_recovery(2));
    assert(runtime_ai_catchup_results()==0x0a);
    runtime_ai_catchup_results_publish(0xff); /* Guest publication is rejected. */
    assert(runtime_ai_catchup_results()==0x0a);
    runtime_ai_catchup_recovery_publish(3,0); /* Guest cannot overwrite host state. */
    assert(runtime_ai_catchup_recovery(3));
    memcpy(old,packet,sizeof old); guest=sync;
    sync=host;
    runtime_ai_catchup_publish(3,.1f);
    runtime_ai_catchup_recovery_publish(3,0);
    assert(runtime_ai_catchup_send("guest",packet,now+51)==sizeof packet);
    sync=guest;
    runtime_ai_catchup_receive("host",packet,sizeof packet,now);
    runtime_ai_catchup_receive("host",old,sizeof old,now); /* Out of order/relay duplicate. */
    assert(fabsf(runtime_ai_catchup_bonus(3)-.1f)<.0001f);
    assert(!runtime_ai_catchup_recovery(3));
    packet[44]=0xff; write64(packet+24,sync.received+1);
    runtime_ai_catchup_receive("host",packet,sizeof packet,now);
    assert(runtime_ai_catchup_results()==0x0a); /* Newer packets cannot rewrite final results. */
    sync.last_receive=now-1501;
    assert(runtime_ai_catchup_bonus(3)==0);
    assert(runtime_ai_catchup_results()==0x0a); /* Final metadata does not expire with driving bonuses. */
    sync=host;
    uint8_t unused[AI_CATCHUP_PACKET];
    runtime_ai_catchup_send("old-client",unused,now);
    assert(!runtime_ai_catchup_ready() && runtime_ai_catchup_bonus(3)==0);
    assert(runtime_ai_catchup_send("guest",packet,now+51)==sizeof packet);
    assert(packet[43]==1 && packet[44]==0x0a); /* Results still transmit without driving readiness. */
    assert(packet[36]==0); /* One incompatible peer disables bonuses for all peers. */
    runtime_ai_catchup_reset("track1","host",0); /* Even the same track has a fresh nonce. */
    assert(runtime_ai_catchup_results()==-1);
    runtime_ai_catchup_receive("host",old,sizeof old,now);
    assert(runtime_ai_catchup_bonus(3)==0);
    runtime_ai_catchup_reset("track2","host",0);
    runtime_ai_catchup_receive("host",packet,sizeof packet,now);
    assert(runtime_ai_catchup_bonus(3)==0);
    assert(runtime_ai_catchup_results()==-1);
    /* Zero finishers is a valid final result, distinct from pending metadata. */
    runtime_ai_catchup_reset("track3","host",1);
    runtime_ai_catchup_results_publish(0); host=sync;
    runtime_ai_catchup_reset("track3","host",0);
    assert(runtime_ai_catchup_send("host",hello,now)==sizeof hello); guest=sync;
    sync=host; runtime_ai_catchup_receive("guest",hello,sizeof hello,now);
    assert(runtime_ai_catchup_send("guest",packet,now)==sizeof packet);
    sync=guest;
    memcpy(old,packet,sizeof old); old[43]=2;
    runtime_ai_catchup_receive("host",old,sizeof old,now);
    assert(runtime_ai_catchup_results()==-1); /* Invalid valid flag is rejected. */
    runtime_ai_catchup_receive("host",packet,sizeof packet,now);
    assert(runtime_ai_catchup_results()==0);
    runtime_ai_catchup_reset(NULL,NULL,0);
    assert(runtime_ai_catchup_send("host",hello,now)==0);
    assert(!runtime_ai_catchup_receive("host",(const uint8_t *)"native game data",16,now));
    puts("AI catch-up: distance, difficulty, authority, race nonce, ordering and expiry passed");
}
