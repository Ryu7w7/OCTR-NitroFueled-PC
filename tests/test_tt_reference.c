#include "../src/runtime_tt_reference.c"
#include <assert.h>
#include <unistd.h>

int main(int argc, char **argv) {
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
    assert(!(intent & TT_REFERENCE_ALIGNED));
    position[0]=930; position[2]=0;
    line[12].p[2]=-180;
    assert(!flat_slide_hop(&tracks[0],10)); /* Drop also remains mandatory. */
    line[12].p[2]=0;
    assert(flat_slide_hop(&tracks[0],10));
    assert(targets[0][0] - position[0] <= 451); /* Bounded near target. */
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
