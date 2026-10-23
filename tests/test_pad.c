#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
int bbgpu_pad_rescan_take(void) { return 0; }
static char status_name[4][64];
static int status_pressing[4];
void bbgpu_pad_status(int port, const char *name, int pressing) {
    if (port<0 || port>=4) return;
    snprintf(status_name[port],sizeof status_name[port],"%s",name);
    status_pressing[port]=pressing;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* Controllers of different kinds for the port tests: the product id sets each one's GUID, which
 * bbport.ini's port.N= names (the in-game menu writes GUIDs; the name SDL reports for a virtual
 * pad depends on SDL's version). kind_guid learns a kind's GUID with nothing else open. */
static void kind_guid(SDL_VirtualJoystickDesc *desc, Uint16 product, char guid[33]) {
    desc->product_id=product;
    const SDL_JoystickID id=SDL_AttachVirtualJoystick(desc);
    assert(id);
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),guid,33);
    assert(SDL_DetachVirtualJoystick(id));
}
static SDL_JoystickID attach_kind(SDL_VirtualJoystickDesc *desc, Uint16 product) {
    desc->product_id=product;
    const SDL_JoystickID id=SDL_AttachVirtualJoystick(desc);
    assert(id);
    return id;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[]="/tmp/bbport-pad-config-XXXXXX";
    int config_fd=mkstemp(config);
    assert(config_fd>=0);
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\nkey.r1=Mouse Right, wheeldown\n";
    assert(write(config_fd,controls,sizeof(controls)-1)==(ssize_t)(sizeof(controls)-1));
    close(config_fd);
    setenv("BB_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    pad_thread_state=-1; /* no bb:pad thread: each read samples first, so the asserts below see it */
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* The mouse among the keys (key.r1=Mouse Right, wheeldown): only while the window has the focus. */
    assert(bindings[IN_R1].key_count==2 && bindings[IN_R1].keys[0]==MOUSE_KEY_BASE+SDL_BUTTON_RIGHT &&
           bindings[IN_R1].keys[1]==MOUSE_KEY_BASE+MOUSE_WHEEL_DOWN);
    {
        static const bool no_keys[SDL_SCANCODE_COUNT];
        mouse_focus=1;
        mouse_buttons=SDL_BUTTON_RMASK;
        assert(key_down(no_keys,IN_R1));
        mouse_buttons=SDL_BUTTON_LMASK;
        assert(!key_down(no_keys,IN_R1));
        wheel_until[1]=SDL_GetTicks()+1000;
        assert(key_down(no_keys,IN_R1));
        mouse_focus=0;
        assert(!key_down(no_keys,IN_R1));
        wheel_until[1]=0;
        mouse_buttons=0;
    }
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==2);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==3);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0]==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1]==SDL_SCANCODE_SPACE);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    /* Split screen with identical controllers (four DualSense, say): one SDL GUID for all of them,
     * so port.2=<GUID> (and port.3, port.4=<the same>) matches every pad; each port, port 1
     * included, still gets one of its own. */
    {
        desc.ntouchpads=0;
        desc.touchpads=NULL;
        SDL_JoystickID same[4];
        for (int i=0;i<4;++i) { same[i]=SDL_AttachVirtualJoystick(&desc); assert(same[i]!=0); }
        char guid[33];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(same[0]),guid,sizeof guid);
        for (int i=1;i<4;++i) {
            char other[33];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(same[i]),other,sizeof other);
            assert(!strcmp(guid,other));
        }
        /* Bind port 2 after player 1 has already claimed that controller. */
        assert(current_gamepad());
        const SDL_JoystickID first_player_pad=SDL_GetGamepadID(gamepad);
        for (int bound=2;bound<=4;++bound) {
            FILE *f=fopen(config,"w");
            assert(f);
            fprintf(f,"port.2=%s\n",guid);
            if (bound>=3) fprintf(f,"port.3=%s\n",guid);
            if (bound==4) fprintf(f,"port.4=%s\n",guid);
            fclose(f);
            config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
            for (int round=0;round<3;++round) {
                current_gamepad();
                for (int p=1;p<bound;++p) port_gamepad(p);
            }
            assert(gamepad && ports[1].pad);
            const SDL_JoystickID p1=SDL_GetGamepadID(gamepad), p2=SDL_GetGamepadID(ports[1].pad);
            assert(p1!=p2);
            if (bound==2) {
                assert(p1==first_player_pad);
                SDL_Joystick *second=SDL_OpenJoystick(p2);
                assert(second && SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_SOUTH,true));
                SDL_UpdateJoysticks(); SDL_UpdateGamepads();
                usleep(120000);
                assert(SDL_GetGamepadID(current_gamepad())==p1); /* player 2 cannot steal player 1 */
                assert(port_gamepad(1) && SDL_GetGamepadButton(port_gamepad(1),SDL_GAMEPAD_BUTTON_SOUTH));
                /* Issue #50: the Controllers tab shows what each port plays with, and the press
                 * marks player 2's identical controller, not player 1's. */
                const int was_opened=ports[1].opened;
                ports[1].opened=1;
                pad_poll();
                assert(status_name[0][0] && status_name[1][0] && strcmp(status_name[1],"keyboard"));
                assert(status_pressing[1] && !status_pressing[0]);
                assert(SDL_SetJoystickVirtualButton(second,SDL_GAMEPAD_BUTTON_SOUTH,false));
                SDL_UpdateJoysticks(); SDL_UpdateGamepads();
                pad_poll();
                assert(!status_pressing[1]);
                ports[1].opened=was_opened;
                SDL_CloseJoystick(second);
            }
            if (bound==3) {
                assert(ports[2].pad);
                const SDL_JoystickID p3=SDL_GetGamepadID(ports[2].pad);
                assert(p3!=p1 && p3!=p2);
            }
            if (bound==4) {
                assert(ports[2].pad && ports[3].pad);
                const SDL_JoystickID p3=SDL_GetGamepadID(ports[2].pad), p4=SDL_GetGamepadID(ports[3].pad);
                assert(p3!=p1 && p3!=p2 && p4!=p1 && p4!=p2 && p4!=p3);
                for (int p=2;p<4;++p) {
                    SDL_Joystick *other=SDL_OpenJoystick(SDL_GetGamepadID(ports[p].pad));
                    assert(other && SDL_SetJoystickVirtualButton(other,SDL_GAMEPAD_BUTTON_SOUTH,true));
                    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
                    usleep(120000);
                    assert(SDL_GetGamepadID(current_gamepad())==p1);
                    assert(SDL_SetJoystickVirtualButton(other,SDL_GAMEPAD_BUTTON_SOUTH,false));
                    SDL_CloseJoystick(other);
                }
            }
        }
        const SDL_JoystickID p1_before=SDL_GetGamepadID(gamepad);
        const SDL_JoystickID p2_before=SDL_GetGamepadID(ports[1].pad);
        assert(SDL_DetachVirtualJoystick(p2_before));
        assert(!port_gamepad(1));
        assert(SDL_GetGamepadID(current_gamepad())==p1_before);
        for (int i=0;i<4;++i) if (same[i]==p2_before) same[i]=SDL_AttachVirtualJoystick(&desc);
        assert(port_gamepad(1) && SDL_GetGamepadID(ports[1].pad)!=p1_before);
        for (int p=1;p<PORTS;++p) if (ports[p].pad) { SDL_CloseGamepad(ports[p].pad); ports[p].pad=NULL; }
        if (gamepad) SDL_CloseGamepad(gamepad);
        gamepad=NULL;
        for (int i=0;i<4;++i) assert(SDL_DetachVirtualJoystick(same[i]));
    }
    /* Four kinds of controller: first, second, third and fourth. */
    const Uint16 same_product=desc.product_id;
    char kind[4][33];
    for (int k=0;k<4;++k) kind_guid(&desc,(Uint16)(0x4001+k),kind[k]);
    for (int k=1;k<4;++k) assert(strcmp(kind[k],kind[0]));
    /* Assign player 2 the controller player 1 is holding; player 1 moves to the other pad. */
    {
        FILE *f=fopen(config,"w"); assert(f); fclose(f);
        config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
        SDL_JoystickID first=attach_kind(&desc,0x4001);
        SDL_JoystickID second=attach_kind(&desc,0x4002);
        assert(current_gamepad() && SDL_GetGamepadID(gamepad)==first);
        f=fopen(config,"w"); assert(f); fprintf(f,"port.2=%s\n",kind[0]); fclose(f);
        config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
        current_gamepad();
        assert(port_gamepad(1) && SDL_GetGamepadID(ports[1].pad)==first);
        assert(current_gamepad() && SDL_GetGamepadID(gamepad)==second);
        SDL_CloseGamepad(ports[1].pad); ports[1].pad=NULL;
        SDL_CloseGamepad(gamepad); gamepad=NULL;
        assert(SDL_DetachVirtualJoystick(first) && SDL_DetachVirtualJoystick(second));
    }
    /* Any lone split-screen pad takes player 1's place until its selected pad connects. */
    for (int player=2;player<=4;++player) {
        FILE *f=fopen(config,"w"); assert(f);
        fprintf(f,"port.1=%s\nport.%d=%s\n",kind[0],player,kind[1]); fclose(f);
        config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
        SDL_JoystickID second=attach_kind(&desc,0x4002);
        assert(current_gamepad() && SDL_GetGamepadID(gamepad)==second);
        assert(!ports[player-1].pad && !ports[player-1].present);
        SDL_JoystickID first=attach_kind(&desc,0x4001);
        usleep(1100000);
        assert(current_gamepad() && SDL_GetGamepadID(gamepad)==first);
        update_port_presence();
        assert(ports[player-1].pad && SDL_GetGamepadID(ports[player-1].pad)==second && ports[player-1].present);
        assert(SDL_DetachVirtualJoystick(first));
        assert(current_gamepad() && SDL_GetGamepadID(gamepad)==second);
        assert(!ports[player-1].pad && !ports[player-1].present);
        SDL_CloseGamepad(gamepad); gamepad=NULL;
        assert(SDL_DetachVirtualJoystick(second));
    }
    {
        FILE *f=fopen(config,"w"); assert(f);
        fprintf(f,"port.1=%s\nport.2=%s\nport.3=%s\nport.4=%s\n",kind[0],kind[1],kind[2],kind[3]); fclose(f);
        config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
        SDL_JoystickID ids[3];
        for (int i=0;i<3;++i) ids[i]=attach_kind(&desc,(Uint16)(0x4002+i));  /* all but the first */
        assert(current_gamepad() && SDL_GetGamepadID(gamepad)==ids[0]);
        assert(!ports[1].present && ports[2].present && ports[3].present);
        assert(ports[2].pad && SDL_GetGamepadID(ports[2].pad)==ids[1]);
        assert(ports[3].pad && SDL_GetGamepadID(ports[3].pad)==ids[2]);
        for (int p=1;p<PORTS;++p) if (ports[p].pad) { SDL_CloseGamepad(ports[p].pad); ports[p].pad=NULL; }
        SDL_CloseGamepad(gamepad); gamepad=NULL;
        for (int i=0;i<3;++i) assert(SDL_DetachVirtualJoystick(ids[i]));
    }
    /* split_auto (on unless split_auto=0) and no port named: each controller connected is the next
     * player, with four identical ones too; turned off, players 2-4 let theirs go and log out. */
    {
        desc.product_id=same_product;
        SDL_JoystickID same[4]={0,0,0,0};
        for (int connected=1;connected<=4;++connected) {
            FILE *f=fopen(config,"w"); assert(f); fclose(f); /* nothing written: the default */
            config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
            for (int i=0;i<connected;++i) if (!same[i]) { same[i]=SDL_AttachVirtualJoystick(&desc); assert(same[i]); }
            for (int round=0;round<3;++round) { current_gamepad(); update_port_presence(); }
            assert(split_auto==1 && gamepad);
            SDL_JoystickID used[PORTS]={SDL_GetGamepadID(gamepad)};
            for (int p=1;p<PORTS;++p) {
                assert(!!ports[p].pad==(p<connected) && ports[p].present==(p<connected));
                if (!ports[p].pad) continue;
                used[p]=SDL_GetGamepadID(ports[p].pad);
                for (int q=0;q<p;++q) assert(used[p]!=used[q]);
            }
        }
        FILE *f=fopen(config,"w"); assert(f); fputs("split_auto=0\n",f); fclose(f);
        config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
        current_gamepad();
        update_port_presence();
        assert(!split_auto);
        for (int p=1;p<PORTS;++p) assert(!ports[p].pad && !ports[p].present);
        SDL_CloseGamepad(gamepad); gamepad=NULL;
        for (int i=0;i<4;++i) assert(SDL_DetachVirtualJoystick(same[i]));
    }
    /* A pad thread stuck in SDL (device detection): the game reads its last sample until that is
     * PAD_STALE_US old, then idle gamepads, without waiting. */
    {
        pad_thread_state=1; /* as if the thread ran: reads only copy `snap` */
        pad_neutral(&snap[0].data);
        snap[0].data.buttons=BTN_CROSS; snap[0].has_pad=1; snap[0].keyboard=1;
        snap_at=now_us();
        assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS));
        snap_at=now_us()-PAD_STALE_US-1000;
        assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_CROSS) && data.left_x==128 && stale_noted);
        snap_at=now_us();
        assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS) && !stale_noted);
        pad_thread_state=-1;
    }
    SDL_Quit();
    unlink(path);
    unlink(config);
    /* Stick calibration (bbport 0.5, PR 71; per port here): a genuine pad reads straight through
     * the 5/127 dead zone; a clone biased to +16380 on all four axes gets that as its neutral and
     * its half-span as the travel, so a full push still reaches full deflection. */
    {
        PadCal c; cal_reset(&c);
        assert(c.have_center==0 && cal_enabled==1 && pad_deadzone==5 && pad_deadzone_outer==127);
        int16_t zero[4]={0,0,0,0};
        for (int i=0;i<PAD_CAL_QUIET_SAMPLES+1;++i) cal_sample(&c,zero);
        assert(!c.have_center); /* SDL's pre-report zeros are not a neutral yet */
        c.since-=PAD_CAL_TIMEOUT_US+1;
        for (int i=0;i<PAD_CAL_QUIET_SAMPLES;++i) cal_sample(&c,zero);
        assert(c.have_center && c.center[0]==0 && c.base[0]==0);
        assert(stick_axis(&c,0,0)==128 && stick_axis(&c,0,32767)==255 && stick_axis(&c,0,-32768)==0);
        assert(stick_axis(&c,0,600)==128);                /* inside the dead zone */
        assert(stick_axis(&c,0,16384)>128+50 && stick_axis(&c,0,16384)<128+70);
        PadCal clone; cal_reset(&clone);
        int16_t biased[4]={16380,16380,16380,16380};
        for (int i=0;i<PAD_CAL_QUIET_SAMPLES;++i) cal_sample(&clone,biased);
        assert(clone.have_center && clone.center[0]==16380 && clone.base[0]==16380);
        assert(stick_axis(&clone,0,16380)==128 && stick_axis(&clone,0,32760)==255 && stick_axis(&clone,0,0)==0);
    }
    puts("PASS: pad ABI, touch, controls, four-player ownership and hotplug, stalled updates, port status, stick calibration");
}
