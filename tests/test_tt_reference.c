#include "../src/runtime_tt_reference.c"
#include <assert.h>
#include <unistd.h>

/* Closed-loop motion under gravity through a 180-degree airborne turn.
 * Test the landing corridor, not just whether a hop flag was emitted. */
static void test_airborne_turn(void) {
    static struct Sample arc[61];
    const float dt=1.f/30, duration=1.2f, omega=3.14159265f/duration, radius=500;
    const float speed=radius*omega;
    for (int direction=-1;direction<=1;direction+=2) for (int bonus=0;bonus<2;++bonus) {
        memset(cursors,0,sizeof cursors);
        for (int i=0;i<61;++i) {
            float t=i*dt, a=fminf(t,duration)*omega;
            arc[i]=(struct Sample){.time=t,.p={radius*sinf(a),direction*radius*(1-cosf(a)),
                t<duration ? 100+1200*t-1000*t*t : 100},.flags=i==0 ? 2 : 0};
            if (t>duration) arc[i].p[0]=-speed*(t-duration);
        }
        tracks[0]=(struct Track){.count=61,.samples=arc};
        strcpy(tracks[0].name,"air_turn__developer"); track_count=1;
        float p[3]={0,-direction*30.f,100},v[3]={speed*(bonus ? 1.3f : 1),0,1100};
        float targets[2][3],next[3]; unsigned intent;
        int corrections=0;
        for (int tick=0;tick<36;++tick) {
            assert(runtime_tt_reference_guide("air_turn",77,0,0,5,p,v,targets,&intent));
            if (tick==5) {
                float boosted[3]={v[0]*3,v[1]*3,v[2]},braked[3];
                assert(runtime_tt_reference_turn_allowance(0,77)<1);
                assert(runtime_tt_reference_motion(0,77,p,boosted,0,dt,braked));
                assert(hypotf(braked[0],braked[1])<hypotf(boosted[0],boosted[1]));
                assert(braked[2]==boosted[2]); /* Ground corner braking never launches a kart. */
            }
            v[2]-=2000*dt; /* Native gravity remains in the loop. */
            if (runtime_tt_reference_motion(0,77,p,v,1,dt,next)) {
                memcpy(v,next,sizeof v); ++corrections;
            }
            for (int k=0;k<3;++k) p[k]+=v[k]*dt;
            assert(isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]));
        }
        assert(corrections>25);
        assert(hypotf(p[0],p[1]-direction*1000.f)<150);
        assert(fabsf(p[2]-100)<120);
        assert(v[0]<0); /* Completes the U-turn, not continued straight flight. */
        float saved[3]; memcpy(saved,v,sizeof saved);
        assert(!runtime_tt_reference_motion(0,78,p,v,1,dt,next)); /* Wrong entity. */
        p[2]-=1000;
        assert(!runtime_tt_reference_motion(0,77,p,v,1,dt,next)); /* No rescue below the route. */
        assert(!memcmp(v,saved,sizeof v));
    }
    memset(tracks,0,sizeof tracks); memset(cursors,0,sizeof cursors); track_count=0;
}

int main(int argc, char **argv) {
    test_airborne_turn();
    static struct Sample line[100];
    for (int i = 0; i < 100; ++i) {
        line[i].p[0] = (float)i * 100;
        line[i].time = (float)i / 10;
    }
    strcpy(tracks[0].name, "test_track__developer"); tracks[0].samples = line;
    tracks[0].count = 100; track_count = 1;
    float position[] = {1000, 0, 0}, velocity[] = {1000, 0, 0};
    float targets[2][3] = {{1500, 0, 0}, {1800, 0, 0}};
    unsigned intent = 0;
    assert(runtime_tt_reference_guide("Octane/test_track/test_track", 1, 0, 0, 5, position, velocity, targets, &intent));
    assert(fabsf(targets[0][0] - 1250) < .1f && targets[0][1] == 0);
    float other[2][3] = {{1500, 0, 0}, {1800, 0, 0}};
    assert(runtime_tt_reference_guide("TEST_TRACK", 2, 1, 0, 5, position, velocity, other, &intent));
    assert(other[0][1] == 0); /* Both follow the recorded lane, not occupancy offsets. */
    assert(runtime_tt_reference_guide("test_track",1,0,0,5,position,velocity,targets,&intent));
    float slower[3]={700,0,0},paced[3];
    assert(runtime_tt_reference_motion(0,1,position,slower,0,1.f/30,paced));
    assert(paced[0]>700 && paced[0]<=1000 && paced[1]==0 && paced[2]==0);
    slower[0]=-700;
    assert(!runtime_tt_reference_motion(0,1,position,slower,0,1.f/30,paced));
    /* Terrain takeoff speed is prepared in advance, never for flat drift hops. */
    line[15].flags=2; line[16].p[2]=150;
    assert(runtime_tt_reference_guide("test_track",1,0,0,5,position,velocity,targets,&intent));
    assert(fabsf(runtime_tt_reference_jump_speed(0,1)-1000)<.01f);
    assert(!runtime_tt_reference_jump_speed(0,2));
    assert(fabsf(runtime_tt_reference_route_speed(0,1)-1000)<.01f);
    assert(!runtime_tt_reference_route_speed(0,2));
    /* A small local drift outside the precision-input corridor keeps speed
     * support, but cannot fire a hop. Large errors and reverse motion do not. */
    position[1]=214;
    assert(runtime_tt_reference_guide("test_track",1,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & TT_REFERENCE_ALIGNED) && !(intent & 2));
    assert(runtime_tt_reference_route_speed(0,1)>0 && runtime_tt_reference_jump_speed(0,1)>0);
    position[1]=500;
    assert(runtime_tt_reference_guide("test_track",1,0,0,5,position,velocity,targets,&intent));
    assert(!runtime_tt_reference_route_speed(0,1) && !runtime_tt_reference_jump_speed(0,1));
    position[1]=0;
    velocity[0]=-1000;
    assert(runtime_tt_reference_guide("test_track",1,0,0,5,position,velocity,targets,&intent));
    assert(!runtime_tt_reference_jump_speed(0,1));
    assert(!runtime_tt_reference_route_speed(0,1));
    velocity[0]=1000; line[16].p[2]=0;
    assert(runtime_tt_reference_guide("test_track",1,0,0,5,position,velocity,targets,&intent));
    assert(!runtime_tt_reference_jump_speed(0,1));
    line[16].p[2]=150;
    for (int i=16;i<100;++i) line[i].time=1.5f+(i-15)*.05f;
    position[0]=1300;
    assert(runtime_tt_reference_guide("test_track",11,0,0,5,position,velocity,targets,&intent));
    assert(runtime_tt_reference_route_speed(0,11)<1000.1f);
    assert(runtime_tt_reference_jump_speed(0,11)>1500 && runtime_tt_reference_jump_speed(0,11)<2000);
    for (int i=0;i<100;++i) line[i].time=i/10.f;
    line[16].p[2]=0; position[0]=1000;
    line[15].flags=0;
    /* Recovery is spatial, independent of elapsed race/ghost time. */
    position[0] = 8000;
    assert(runtime_tt_reference_guide("test_track", 1, 0, 0, 5, position, velocity, targets, &intent));
    assert(targets[0][0] > 8000);
    position[0] = 1000; position[2] = 2000;
    float saved[2][3]; memcpy(saved, targets, sizeof saved);
    assert(runtime_tt_reference_guide("test_track", 1, 0, 0, 5, position, velocity, targets, &intent));
    assert(targets[0][2] == 0 && !(intent & TT_REFERENCE_ALIGNED));
    position[2] = 0; velocity[0] = -1000;
    assert(runtime_tt_reference_guide("test_track", 1, 0, 0, 5, position, velocity, targets, &intent));
    assert(!(intent & TT_REFERENCE_ALIGNED));
    velocity[0] = 1000; line[10].flags = 3;
    assert(runtime_tt_reference_guide("test_track", 3, 0, 0, 5, position, velocity, targets, &intent));
    assert((intent & 3) == 3 && !(intent & 4));
    assert((intent & (TT_REFERENCE_ALIGNED | TT_REFERENCE_DRIFT_HOP)) ==
                     (TT_REFERENCE_ALIGNED | TT_REFERENCE_DRIFT_HOP));
    assert(targets[0][1] == 0); /* Flat drift entry still centers steering. */
    position[1]=250;
    memcpy(saved, targets, sizeof saved);
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    assert(!intent && targets[0][1] == 0 && targets[0][0] > position[0]);
    /* Even outside technique alignment, every frame still steers onto TT. */
    for (int i = 0; i < 4; ++i) {
        position[1] = i % 2 ? 205 : 190;
        assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
        assert(!intent && targets[0][1] == 0 && targets[0][0] > position[0]);
    }
    position[1] = 119;
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2)); /* Recovery must not replay the consumed hop. */
    position[1] = 190;
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    position[1]=0;
    velocity[0]=400; velocity[1]=900;
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & TT_REFERENCE_ALIGNED));
    velocity[0]=1000; velocity[1]=0;
    /* Same marker, and a backwards/forwards projection wobble, cannot retrigger. */
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2));
    position[0]=900;
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 512));
    position[0]=1000;
    assert(runtime_tt_reference_guide("test_track",3,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2));
    assert(runtime_tt_reference_guide("test_track",3,0,1,5,position,velocity,targets,&intent));
    assert(intent & 2); /* New lap legitimately rearms the marker. */
    velocity[0]=0;
    assert(runtime_tt_reference_guide("test_track",5,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & (2|512|16))); /* Stationary grid must not jump or U-turn. */
    velocity[0]=1000; position[1]=1000;
    assert(runtime_tt_reference_guide("test_track",6,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & (2|512|16))); /* A distant route projection is not a takeoff. */
    position[1]=0;
    assert(runtime_tt_reference_guide("test_track",6,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2)); /* No late replay after reacquisition. */
    /* A future edge prepares release without triggering the jump .22s early. */
    memset(cursors, 0, sizeof cursors); position[0] = 900;
    assert(runtime_tt_reference_guide("test_track", 4, 0, 0, 5, position, velocity, targets, &intent));
    assert(!(intent & (2|4))); /* Flat slide hop must not veto pickup approaches. */
    assert(targets[0][1] == 0 && targets[0][0] <= position[0] + 250.1f);
    position[0] = 980;
    assert(runtime_tt_reference_guide("test_track", 4, 0, 0, 5, position, velocity, targets, &intent));
    assert(!(intent & 2) && (intent & 512) && (intent & TT_REFERENCE_DRIFT_HOP));
    position[0] = 999;
    assert(runtime_tt_reference_guide("test_track",4,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2)); /* Nearest future sample must not fire before the takeoff position. */
    position[0] = 1000;
    assert(runtime_tt_reference_guide("test_track",4,0,0,5,position,velocity,targets,&intent));
    assert(intent & 2);
    position[0] = 1110; /* Crossing again must not repeat the same hop. */
    assert(runtime_tt_reference_guide("test_track", 4, 0, 0, 5, position, velocity, targets, &intent));
    assert(!(intent & 2));
    line[10].flags=2;
    assert(flat_slide_hop(&tracks[0],10)); /* No drift-hold bit does not make a flat hop a shortcut. */
    line[10].flags=3;
    line[12].p[2]=180;
    assert(!flat_slide_hop(&tracks[0],10)); /* Ramp/shortcut jump remains mandatory. */
    position[0]=930;
    assert(runtime_tt_reference_guide("test_track",7,0,0,5,position,velocity,targets,&intent));
    assert(intent & 4); /* Actual ramp still centers the route and blocks pickups. */
    /* Keep Developer steering when jump height differs, but never when the
     * off-path recovery keeps ghost steering without replaying its technique. */
    position[0]=1100; position[2]=300;
    assert(runtime_tt_reference_guide("test_track",7,0,0,5,position,velocity,targets,&intent));
    assert(intent & 4);
    assert(!(intent & TT_REFERENCE_BELOW_ROUTE)); /* Above the recording is not a missed landing. */
    assert(!(intent & TT_REFERENCE_REJOIN_READY)); /* Airborne horizontal proximity is not a safe recovery handoff. */
    position[2]=-300;
    assert(runtime_tt_reference_guide("test_track",7,0,0,5,position,velocity,targets,&intent));
    assert((intent & (4|TT_REFERENCE_BELOW_ROUTE))==(4|TT_REFERENCE_BELOW_ROUTE));
    position[2]=300;
    position[1]=250;
    assert(runtime_tt_reference_guide("test_track",7,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & TT_REFERENCE_ALIGNED));
    position[1]=0; position[2]=700;
    assert(runtime_tt_reference_guide("test_track",7,0,0,5,position,velocity,targets,&intent));
    assert(intent & TT_REFERENCE_ALIGNED); /* Height cannot move steering to a distant ghost point. */
    assert(!(intent & 2)); /* Never replay takeoff while following the airborne corridor. */
    position[0]=930; position[2]=0;
    line[12].p[2]=-180;
    assert(!flat_slide_hop(&tracks[0],10)); /* Drop also remains mandatory. */
    line[12].p[2]=0;
    assert(flat_slide_hop(&tracks[0],10));
    assert(targets[0][0] - position[0] <= 451); /* Bounded near target. */
    /* Fast motion can cross more than four densely sampled points. The hop
     * between the old and new projection must still fire exactly once. */
    memset(cursors,0,sizeof cursors);
    line[10].flags=2; line[12].p[2]=180;
    position[0]=700; position[1]=position[2]=0;
    assert(runtime_tt_reference_guide("test_track",8,0,0,5,position,velocity,targets,&intent));
    position[0]=1400;
    assert(runtime_tt_reference_guide("test_track",8,0,0,5,position,velocity,targets,&intent));
    assert(intent & 2);
    assert(runtime_tt_reference_guide("test_track",8,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2));
    /* At speed, request the terrain hop while still grounded before the
     * next simulation step crosses the cliff. Flat drift hops stay native. */
    memset(cursors,0,sizeof cursors);
    position[0]=950; velocity[0]=3000;
    assert(runtime_tt_reference_guide("test_track",10,0,0,5,position,velocity,targets,&intent));
    assert(intent & 2);
    position[0]=1100;
    assert(runtime_tt_reference_guide("test_track",10,0,0,5,position,velocity,targets,&intent));
    assert(!(intent & 2));
    velocity[0]=1000;
    line[12].p[2]=0;
    for (int i=12;i<100;++i) { line[i].p[0]=1200; line[i].p[1]=(i-12)*100.f; }
    position[0]=1000; position[1]=position[2]=0; velocity[0]=3000;
    assert(runtime_tt_reference_guide("test_track",12,0,0,5,position,velocity,targets,&intent));
    assert(fabsf(targets[0][0]-1200)<.1f && fabsf(targets[0][1]-175)<.1f);
    for (int i=12;i<100;++i) { line[i].p[0]=i*100.f; line[i].p[1]=0; }
    position[0] = NAN;
    assert(!runtime_tt_reference_guide("test_track", 1, 0, 0, 5, position, velocity, targets, &intent));
    assert(!runtime_tt_reference_guide("unknown", 1, 0, 0, 5, position, velocity, targets, &intent));
    /* At racing speed, neither recovery hysteresis nor hop flags may move
     * the steering target closer and increase steering gain abruptly. */
    position[0]=1000; position[1]=position[2]=0;
    velocity[0]=3000; velocity[1]=velocity[2]=0; line[10].flags=0;
    assert(runtime_tt_reference_guide("test_track",9,0,0,5,position,velocity,targets,&intent));
    assert(fabsf(targets[0][0]-1750)<.1f);
    for (int i=0;i<4;++i) {
        position[1]=i%2 ? 119 : 205;
        line[12].flags=i%2 ? 2 : 0;
        assert(runtime_tt_reference_guide("test_track",9,0,0,5,position,velocity,targets,&intent));
        assert(fabsf(targets[0][0]-1750)<.1f && targets[0][1]==0);
    }
    position[1]=0; position[0]=1050;
    assert(runtime_tt_reference_guide("test_track",9,0,0,5,position,velocity,targets,&intent));
    float forward_target=targets[0][0];
    position[0]=1040;
    assert(runtime_tt_reference_guide("test_track",9,0,0,5,position,velocity,targets,&intent));
    assert(targets[0][0]==forward_target); /* Forward projection cannot crawl backwards. */
    velocity[0]=-3000;
    assert(runtime_tt_reference_guide("test_track",9,0,0,5,position,velocity,targets,&intent));
    assert(targets[0][0]<forward_target); /* Actual native reversing can rewind. */
    /* Learn the recorded cycle, not a universal meter-quality target. */
    for (unsigned i=10;i<=30;++i) line[i].flags=1;
    line[12].flags|=8192; line[16].flags|=8; line[23].flags|=8; line[29].flags|=8;
    position[0]=1500; velocity[0]=1000;
    assert(runtime_tt_reference_guide("test_track",10,0,0,5,position,velocity,targets,&intent));
    assert(fabsf(runtime_tt_reference_boost_interval(0,0)-.4f)<.001f);
    assert(fabsf(runtime_tt_reference_boost_interval(0,1)-.7f)<.001f);
    assert(fabsf(runtime_tt_reference_boost_interval(0,2)-.6f)<.001f);
    assert(runtime_tt_reference_boost_interval(0,3)==0);
    /* Lookahead crosses the recording seam without running out at lap 7. */
    memset(cursors, 0, sizeof cursors);
    for (int i = 0; i < 100; ++i) {
        float angle = i * 6.2831853f / 100;
        line[i].p[0] = cosf(angle) * 1000; line[i].p[1] = sinf(angle) * 1000;
        line[i].flags = 0;
    }
    memcpy(position, line[98].p, sizeof position);
    for (int k = 0; k < 3; ++k) velocity[k] = line[99].p[k] - position[k];
    memcpy(targets[0], line[4].p, sizeof position); memcpy(targets[1], line[8].p, sizeof position);
    assert(runtime_tt_reference_guide("test_track", 1, 0, 6, 5, position, velocity, targets, &intent));
    assert(targets[0][1] > 0 && targets[1][1] > targets[0][1]);
    /* Cross the seam spatially, rather than only checking a wrapped target. */
    memcpy(position,line[2].p,sizeof position);
    for (int k=0;k<3;++k) velocity[k]=(line[3].p[k]-position[k])*10;
    assert(runtime_tt_reference_guide("test_track",1,0,6,5,position,velocity,targets,&intent));
    assert(cursors[0].sample<5);
    memcpy(position,line[4].p,sizeof position);
    assert(runtime_tt_reference_guide("test_track",1,0,6,5,position,velocity,targets,&intent));
    assert(cursors[0].sample>=3 && cursors[0].sample<6);
    track_count = 0; /* Fixture is static, unlike file-loaded samples. */
    if (argc > 1) {
        assert(runtime_tt_reference_load(argv[1]) == 160);
        const struct Track *cove = find_track("Octane/T111_Crash_Cove/T111_Crash_Cove__developer");
        assert(cove && cove->count > 2000);
        unsigned ramp_markers=0;
        for (unsigned i=0;i<cove->count;++i)
            if (cove->samples[i].time>13 && cove->samples[i].time<14 && (cove->samples[i].flags&2)) {
                assert(!flat_slide_hop(cove,i)); ++ramp_markers;
            }
        assert(ramp_markers); /* Crash Cove's first-lap climb is never deferred. */
        /* Drive-Thru Danger's failed shortcut: the kart can be below and
         * beside the recording while still on the same local approach. */
        const struct Track *toxic=find_track("t508_toxic__developer");
        assert(toxic && toxic->count>356 && (toxic->samples[356].flags&2));
        const float *before=toxic->samples[355].p, *edge=toxic->samples[356].p;
        const float dx=edge[0]-before[0],dy=edge[1]-before[1],length=hypotf(dx,dy);
        for (int height=0;height<2;++height) {
            memset(cursors,0,sizeof cursors);
            for (int k=0;k<3;++k) position[k]=toxic->samples[354].p[k];
            position[2]+=height ? -180 : 180;
            velocity[0]=dx/length*3500; velocity[1]=dy/length*3500; velocity[2]=0;
            assert(runtime_tt_reference_guide("t508_toxic",99,0,0,5,position,velocity,targets,&intent));
            for (int k=0;k<3;++k) position[k]=before[k]+.7f*(edge[k]-before[k]);
            position[0]-=dy/length*80; position[1]+=dx/length*80;
            position[2]+=height ? -180 : 180;
            assert(runtime_tt_reference_guide("t508_toxic",99,0,0,5,position,velocity,targets,&intent));
            assert((intent&(TT_REFERENCE_ALIGNED|2))==(TT_REFERENCE_ALIGNED|2));
            assert(runtime_tt_reference_guide("t508_toxic",99,0,0,5,position,velocity,targets,&intent));
            assert(!(intent&2)); /* Same marker cannot fire twice. */
        }
        /* Integrate the actual failed shortcut under gravity, including a
         * weak native takeoff. This checks the landing region, not a button. */
        memset(cursors,0,sizeof cursors);
        memcpy(position,toxic->samples[355].p,sizeof position);
        for (int k=0;k<3;++k) velocity[k]=(toxic->samples[357].p[k]-toxic->samples[356].p[k])/
            (toxic->samples[357].time-toxic->samples[356].time);
        assert(runtime_tt_reference_guide("t508_toxic",100,0,0,5,position,velocity,targets,&intent));
        memcpy(position,toxic->samples[356].p,sizeof position); position[0]+=30; position[2]-=40;
        velocity[2]*=.6f;
        const float flight_dt=1.f/30;
        int guided_frames=0;
        for (int tick=0;tick<24;++tick) {
            assert(runtime_tt_reference_guide("t508_toxic",100,0,0,5,position,velocity,targets,&intent));
            velocity[2]-=6000*flight_dt;
            float corrected[3];
            if (runtime_tt_reference_motion(0,100,position,velocity,1,flight_dt,corrected)) {
                memcpy(velocity,corrected,sizeof velocity); ++guided_frames;
            }
            for (int k=0;k<3;++k) position[k]+=velocity[k]*flight_dt;
        }
        printf("Toxic landing simulation: frames=%d position=%.1f,%.1f,%.1f\n",guided_frames,position[0],position[1],position[2]);
        assert(guided_frames>=20);
        assert(hypotf(position[0]-toxic->samples[375].p[0],position[1]-toxic->samples[375].p[1])<300);
        assert(position[2]>toxic->samples[375].p[2]-120);
        unsigned checks = 0;
        for (unsigned i = 1; i + 20 < cove->count; i += 17) {
            memcpy(position, cove->samples[i].p, sizeof position);
            for (int k = 0; k < 3; ++k) velocity[k] = cove->samples[i + 1].p[k] - position[k];
            memcpy(targets[0], cove->samples[i + 3].p, sizeof position);
            memcpy(targets[1], cove->samples[i + 6].p, sizeof position);
            checks += runtime_tt_reference_guide("T111_Crash_Cove", 1, 0, 0, 5, position, velocity, targets, &intent);
        }
        assert(checks > 100);
        /* Exercise every installed track and all six difficulties, including
         * additional race laps beyond the three laps stored in TT assets. */
        unsigned routes_checked=0, poses_checked=0;
        for (unsigned t=0;t<track_count;++t) {
            const struct Track *reference=tracks+t;
            char map[64]; strcpy(map,reference->name);
            char *suffix=strstr(map,"__"); assert(suffix);
            int first_level=!strcmp(suffix,"__tropy") ? 0 :
                            !strcmp(suffix,"__oxide") ? 3 :
                            !strcmp(suffix,"__velo") ? 4 : 5;
            int last_level=first_level==0 ? 2 : first_level;
            *suffix=0;
            unsigned continuation=reference->loop_start+2;
            if (continuation+1<reference->count) {
                const struct Sample *end=reference->samples+reference->count-1;
                const struct Sample *resume=reference->samples+continuation;
                memcpy(position,end->p,sizeof position);
                for (int k=0;k<3;++k) velocity[k]=(end->p[k]-end[-1].p[k])*30;
                assert(runtime_tt_reference_guide(map,888,7,4,first_level,position,velocity,targets,&intent));
                cursors[7].sample=reference->count-2; cursors[7].fraction=1;
                memcpy(position,resume->p,sizeof position);
                for (int k=0;k<3;++k) velocity[k]=(resume[1].p[k]-resume->p[k])*30;
                float forward=velocity[0]*(end->p[0]-end[-1].p[0])+velocity[1]*(end->p[1]-end[-1].p[1]);
                assert(runtime_tt_reference_guide(map,888,7,4,first_level,position,velocity,targets,&intent));
                if (forward>0) assert(cursors[7].sample>=reference->loop_start && cursors[7].sample<reference->loop_start+5);
            }
            for (int level=first_level;level<=last_level;++level) {
                ++routes_checked;
                for (int lap=0;lap<7;++lap) {
                    unsigned guided=0, safe_rejoins=0;
                    for (unsigned i=4;i+20<reference->count;i+=17) {
                        memcpy(position,reference->samples[i].p,sizeof position);
                        float dt=reference->samples[i+1].time-reference->samples[i].time;
                        for (int k=0;k<3;++k)
                            velocity[k]=dt>0 ? (reference->samples[i+1].p[k]-position[k])/dt : 0;
                        assert(runtime_tt_reference_guide(map,1,0,lap,level,position,velocity,targets,&intent));
                        ++guided; ++poses_checked;
                        assert(!(intent&TT_REFERENCE_BELOW_ROUTE)); /* Never reject the recorded trajectory itself. */
                        for (int a=0;a<2;++a) for (int k=0;k<3;++k) assert(isfinite(targets[a][k]));
                        if ((intent&TT_REFERENCE_REJOIN_READY) && hypotf(velocity[0],velocity[1])>300)
                            ++safe_rejoins;
                    }
                    assert(guided>0 && safe_rejoins>0);
                }
            }
        }
        assert(routes_checked==240);
        printf("All-track reference check: %u track/difficulty combinations, %u poses, seven laps each\n",
               routes_checked,poses_checked);
        clear_tracks();
    }
    char bad[96]; snprintf(bad, sizeof bad, "/tmp/ai-reference-%ld.bin", (long)getpid());
    FILE *file = fopen(bad, "wb"); assert(file);
    uint32_t count = 1;
    assert(fwrite("BBGHOST1", 1, 8, file) == 8 && fwrite(&count, 4, 1, file) == 1);
    fclose(file);
    assert(runtime_tt_reference_load(bad) == 0 && track_count == 0); /* Truncated record. */
    unlink(bad);
    assert(!runtime_tt_reference_load(NULL));
    puts("TT reference: spatial recovery, lane variation, jump/drift, gates and local assets OK");
    return 0;
}
