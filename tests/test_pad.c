#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
int bbgpu_pad_rescan_take(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
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
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\n";
    assert(write(config_fd,controls,sizeof(controls)-1)==(ssize_t)(sizeof(controls)-1));
    close(config_fd);
    setenv("BB_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
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
    /* Split screen with identical controllers: one SDL GUID for all of them, so port.2=<GUID> (and
     * port.3=<the same>) matches every pad; each port, port 1 included, still gets one of its own. */
    {
        desc.ntouchpads=0;
        desc.touchpads=NULL;
        SDL_JoystickID same[3];
        for (int i=0;i<3;++i) { same[i]=SDL_AttachVirtualJoystick(&desc); assert(same[i]!=0); }
        char guid[33];
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(same[0]),guid,sizeof guid);
        for (int i=1;i<3;++i) {
            char other[33];
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(same[i]),other,sizeof other);
            assert(!strcmp(guid,other));
        }
        for (int bound=2;bound<=3;++bound) {
            FILE *f=fopen(config,"w");
            assert(f);
            fprintf(f,"port.2=%s\n",guid);
            if (bound==3) fprintf(f,"port.3=%s\n",guid);
            fclose(f);
            config_checked=0; memset(&config_mtime,0,sizeof config_mtime);
            for (int round=0;round<3;++round) {
                current_gamepad();
                for (int p=1;p<bound;++p) port_gamepad(p);
            }
            assert(gamepad && ports[1].pad);
            const SDL_JoystickID p1=SDL_GetGamepadID(gamepad), p2=SDL_GetGamepadID(ports[1].pad);
            assert(p1!=p2);
            if (bound==3) {
                assert(ports[2].pad);
                const SDL_JoystickID p3=SDL_GetGamepadID(ports[2].pad);
                assert(p3!=p1 && p3!=p2);
            }
        }
        for (int p=1;p<PORTS;++p) if (ports[p].pad) { SDL_CloseGamepad(ports[p].pad); ports[p].pad=NULL; }
        if (gamepad) SDL_CloseGamepad(gamepad);
        gamepad=NULL;
        for (int i=0;i<3;++i) assert(SDL_DetachVirtualJoystick(same[i]));
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
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture, controls, split screen with identical pads, stick calibration");
}
