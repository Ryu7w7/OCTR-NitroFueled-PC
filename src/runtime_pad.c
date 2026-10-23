/* libScePad on SDL3 gamepads and the keyboard. SDL events are pumped by the window thread
 * (gpu/shim/window.cpp); joysticks are updated and sampled by this file's own thread (bb:pad,
 * see pad_thread below), and the game's threads read the last sample without calling SDL.
 *
 * Keyboard layout (also with a gamepad connected: both drive the game, unless a split-screen port
 * 2..4 is bound to the keyboard, bbport.ini port.N=keyboard: that player plays on it alone):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right).
 *
 * Stick neutral and travel (bbport 0.5, PR 71): SDL exposes no way to read a pad's calibration
 * and some clones report a biased neutral (a Switch-style pad was seen returning both sticks at a
 * constant ~ +/-16380) and use only part of SDL's -32768..32767 span. The neutral of each axis is
 * taken from a quiet window after the pad opens (SDL returns zero until the first report arrives,
 * so those samples are skipped) and subtracted when all four axes are biased; otherwise (a genuine
 * pad, perhaps opened with a stick held) the neutral is 0. A biased axis is scaled by its own
 * travel, refined by the largest push seen per direction. The stick is then converted as shadPS4
 * does, with an inner/outer dead zone (BB_PAD_DEADZONE, default 5; BB_PAD_DEADZONE_OUTER, default
 * 127). BB_PAD_CENTER=lx,ly,rx,ry overrides the neutral, BB_PAD_CENTER_CAL=0 disables the
 * measurement. SacredFire keeps one calibration per controller port (split screen). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#ifdef _WIN32
#define strcasestr SDL_strcasestr /* not in the MinGW C library */
#endif

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static SDL_Gamepad *other_gamepads[8];
static int other_count;
/* SacredFire: ports 2..4 for split screen: the gamepad named in bbport.ini (port.N=), opened while
 * connected; the game sees user N logged in while the port has its gamepad. Port 1 is `gamepad`. */
#define PORTS 4
typedef struct { char want[64]; SDL_Gamepad *pad; int opened, present; } Port;
static Port ports[PORTS]; /* ports[0].want overrides BB_GAMEPAD for port 1 */
/* port.N=keyboard (N 2..4): that player plays on the keyboard, always logged in, and port 1 keeps
 * its gamepad alone (one person on the keyboard, another on a gamepad). */
static int is_keyboard(const char *want) { return !SDL_strcasecmp(want,"keyboard"); }
static int keyboard_port(void) { for (int p=1;p<PORTS;++p) if (is_keyboard(ports[p].want)) return p; return 0; }
static int is_preferred(SDL_JoystickID id, const char *want);
static int claimed_by_other_port(SDL_JoystickID id, int except);
static unsigned pad_rescan_attempts; /* misses in a row looking for a bound controller (current_gamepad) */
static struct timespec config_mtime; static uint64_t config_checked;
/* split_auto (the in-game menu's and the launchers' "assign controllers automatically"; on unless
 * split_auto=0): each port 2..4 without a controller named takes the next connected one that no
 * player has, in the order they were connected, so two to four people play without choosing
 * anything (players 2-4 save as guests, runtime_savedata.c). Off, such a port is unused and an
 * idle player's pad is taken over by the one touched last (one person switching controllers). */
static int split_auto=-1; /* -1 until bbport.ini is read */
static void load_ports(void) {
    const char *path=getenv("BB_CONFIG");
    FILE *f=path ? fopen(path,"r") : NULL;
    char fresh[PORTS][64]; memset(fresh,0,sizeof fresh);
    int auto_fresh=1;
    if (f) {
        char line[512];
        while (fgets(line,sizeof line,f)) {
            if (!strncmp(line,"split_auto=",11)) { auto_fresh=line[11]!='0'; continue; }
            if (strncmp(line,"port.",5) || line[5]<'1' || line[5]>'4' || line[6]!='=') continue;
            char *v=line+7; v[strcspn(v,"\r\n")]=0;
            while (*v==' ' || *v=='\t') ++v;
            snprintf(fresh[line[5]-'1'],sizeof fresh[0],"%.63s",v);
        }
        fclose(f);
    }
    for (int p=0;p<PORTS;++p) {
        if (strcmp(fresh[p],ports[p].want)) {
            pad_rescan_attempts=0; /* a new binding is looked for afresh */
            snprintf(ports[p].want,sizeof ports[p].want,"%.63s",fresh[p]);
            if (!p && gamepad) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
            if (p && ports[p].pad) { SDL_CloseGamepad(ports[p].pad); ports[p].pad=NULL; }
            if (p && gamepad && fresh[p][0] && !is_keyboard(fresh[p]) &&
                is_preferred(SDL_GetGamepadID(gamepad),fresh[p])) {
                int count=0, another=0;
                SDL_JoystickID *ids=SDL_GetGamepads(&count);
                for (int i=0;ids && i<count;++i)
                    if (ids[i]!=SDL_GetGamepadID(gamepad) && is_preferred(ids[i],fresh[p]) &&
                        !claimed_by_other_port(ids[i],p)) another=1;
                SDL_free(ids);
                if (!another) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
            }
            if (p) printf("Runtime: controller port %d -> %s\n",p+1,fresh[p][0] ? fresh[p] : "(none)");
        }
    }
    if (auto_fresh!=split_auto) {
        split_auto=auto_fresh;
        printf("Runtime: controllers for players 2-4 %s\n",split_auto ? "assigned automatically" : "as chosen per port");
        if (!split_auto) for (int p=1;p<PORTS;++p) if (!ports[p].want[0] && ports[p].pad) { SDL_CloseGamepad(ports[p].pad); ports[p].pad=NULL; }
    }
}
/* bbport.ini re-read when it changes (the in-game menu writes it); called under lock. */
static void refresh_ports(uint64_t now) {
    if (now-config_checked<1000000) return;
    config_checked=now;
    const char *path=getenv("BB_CONFIG");
    struct stat st;
    if (!path || stat(path,&st)) return;
#ifdef _WIN32
    /* No nanoseconds in the CRT's stat: the size tells two writes within a second apart. */
    if (st.st_mtime==config_mtime.tv_sec && (long)st.st_size==config_mtime.tv_nsec) return;
    config_mtime.tv_sec=st.st_mtime; config_mtime.tv_nsec=(long)st.st_size;
#else
    if (st.st_mtim.tv_sec==config_mtime.tv_sec && st.st_mtim.tv_nsec==config_mtime.tv_nsec) return;
    config_mtime=st.st_mtim;
#endif
    load_ports();
}
/* 1 when the gamepad id is claimed by a port other than `except` (ports 2..4 with a name). A port
 * that has its gamepad open claims that one pad only: identical controllers share one SDL GUID and
 * name, so port.2=<GUID> matches every one of them, and port 1 would be left with none. A port still
 * without its pad reserves every match from port 1 and from the ports after it (port 2 before 3
 * before 4), so two ports bound to the same GUID take one pad each instead of yielding to each other. */
static int claimed_by_other_port(SDL_JoystickID id, int except) {
    for (int p=1;p<PORTS;++p) {
        if (p==except) continue;
        /* An open pad is its port's, named or taken automatically (split_auto). */
        if (ports[p].pad) { if (SDL_GetGamepadID(ports[p].pad)==id) return 1; continue; }
        if (!ports[p].want[0] || is_keyboard(ports[p].want)) continue;
        if ((except==0 || p<except) && is_preferred(id,ports[p].want)) return 1;
    }
    return 0;
}
/* A controller a named port 2..4 is still waiting for, or player 1's chosen one while player 1 is
 * on another: a port taking pads automatically (split_auto) leaves it to them. */
static const char *preferred_gamepad(void);
static int wanted_by_named_port(SDL_JoystickID id) {
    for (int p=1;p<PORTS;++p)
        if (!ports[p].pad && ports[p].want[0] && !is_keyboard(ports[p].want) && is_preferred(id,ports[p].want)) return 1;
    const char *want=preferred_gamepad();
    return want && (!gamepad || !is_preferred(SDL_GetGamepadID(gamepad),want)) && is_preferred(id,want);
}
/* A controller that drops (Bluetooth, battery) is logged with the time since the game first read
 * the pads and the rumble sent until then, so a log tells a dropped pad from other input trouble. */
static uint64_t now_us(void);
static uint64_t pad_started_us, rumble_calls, rumble_sent_count;
static void note_disconnect(SDL_Gamepad *g, int port) {
    const char *name=SDL_GetGamepadName(g);
    printf("Runtime: controller port %d: %s disconnected at %.0f s (rumble: %llu requests, %llu sent)\n",
           port,name ? name : "gamepad",pad_started_us ? (double)(now_us()-pad_started_us)/1e6 : 0.0,
           (unsigned long long)rumble_calls,(unsigned long long)rumble_sent_count);
    fflush(stdout);
}
/* Per-port stick calibration (the functions follow the axis helpers below). */
typedef struct PadCal {
    int have_center, started, quiet_count;
    int center[4], prev[4];
    int base[4];                       /* 0: normal pad; else |neutral| of a biased axis */
    int max[4][2];                     /* largest push seen per direction on a biased axis */
    uint64_t since;
    uint64_t sampled;                  /* now_us() of the last cal_sample (read_gamepad) */
} PadCal;
static PadCal cals[PORTS];             /* cals[0]: port 1 (`gamepad`), cals[p]: ports[p].pad */
static void cal_reset(PadCal *c);
/* The gamepad of port p (1..3), opened when its named controller is connected (or, with
 * split_auto and no name, the next free one); NULL otherwise. */
static SDL_Gamepad *port_gamepad(int p) {
    Port *port=&ports[p];
    const int automatic=!port->want[0] && split_auto;
    if ((!port->want[0] && !automatic) || is_keyboard(port->want)) { if (port->pad) { SDL_CloseGamepad(port->pad); port->pad=NULL; } return NULL; }
    if (port->pad && !SDL_GamepadConnected(port->pad)) { note_disconnect(port->pad,p+1); SDL_CloseGamepad(port->pad); port->pad=NULL; }
    if (port->pad) return port->pad;
    /* Player 1 has the first controller before the others take theirs. */
    if (automatic && !gamepad) return NULL;
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int i=0; ids && i<count && !port->pad; ++i) {
        if (!automatic && !is_preferred(ids[i],port->want)) continue;
        if (gamepad && SDL_GetGamepadID(gamepad)==ids[i]) continue; /* port 1 has it */
        if (claimed_by_other_port(ids[i],p) || (automatic && wanted_by_named_port(ids[i]))) continue;
        port->pad=SDL_OpenGamepad(ids[i]);
        if (port->pad) cal_reset(&cals[p]);
        if (port->pad) printf("Runtime: controller port %d: %s connected\n",p+1,SDL_GetGamepadName(port->pad));
    }
    SDL_free(ids);
    return port->pad;
}
/* Logins/logouts of users 2..4 follow their port's gamepad; called under lock once a second. */
/* Weak: tests/test_pad.c links this module without runtime_services.c. */
__attribute__((weak)) void runtime_services_user_change(int user, int login);
static void update_port_presence(void) {
    for (int p=1;p<PORTS;++p) {
        const int present=port_gamepad(p)!=NULL || is_keyboard(ports[p].want);
        if (present!=ports[p].present) { ports[p].present=present; if (runtime_services_user_change) runtime_services_user_change(p+1,present); }
    }
}
int runtime_pad_user_present(int user) {
    if (user==1) return 1;
    if (user<2 || user>PORTS) return 0;
    pthread_mutex_lock(&lock);
    const int present=ports[user-1].present;
    pthread_mutex_unlock(&lock);
    return present;
}
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
/* Per-port stick calibration (see the file header). */
#define PAD_CAL_QUIET 512              /* an axis is quiet when it moved no more than this */
#define PAD_CAL_QUIET_SAMPLES 8
#define PAD_CAL_TIMEOUT_US 3000000u    /* after this, an all-zero reading is accepted as the neutral */
#define PAD_CAL_BIAS 8192              /* |neutral| past this: a biased neutral and a reduced travel */
#define PAD_CAL_REFINE 90              /* % of the base travel a push must reach to count as the full scale */
#define PAD_CAL_INTERVAL_US 15000u     /* one calibration sample a frame at most */
static int cal_enabled=-1, pad_deadzone=-1, pad_deadzone_outer=-1, cal_manual, cal_manual_center[4];
static void cal_load(void) {
    static int loaded;
    if (loaded) return;
    loaded=1;
    const char *v=getenv("BB_PAD_CENTER_CAL"); cal_enabled=!(v && *v=='0');
    /* Inner/outer dead zone, as shadPS4's analog_deadzone: [inner, outer] maps linearly to the
     * full deflection, so an axis with a reduced travel (outer below 127) reaches full. */
    v=getenv("BB_PAD_DEADZONE"); pad_deadzone=v && *v ? atoi(v) : 5;
    if (pad_deadzone<0) pad_deadzone=0; else if (pad_deadzone>126) pad_deadzone=126;
    v=getenv("BB_PAD_DEADZONE_OUTER"); pad_deadzone_outer=v && *v ? atoi(v) : 127;
    if (pad_deadzone_outer<=pad_deadzone) pad_deadzone_outer=pad_deadzone<127 ? pad_deadzone+1 : 127;
    v=getenv("BB_PAD_CENTER");
    if (v && *v && sscanf(v,"%d,%d,%d,%d",&cal_manual_center[0],&cal_manual_center[1],
                          &cal_manual_center[2],&cal_manual_center[3])==4) {
        cal_manual=1;
        printf("Runtime: pad center (BB_PAD_CENTER): lx=%d ly=%d rx=%d ry=%d\n",
               cal_manual_center[0],cal_manual_center[1],cal_manual_center[2],cal_manual_center[3]);
    }
}
static void cal_set_base(PadCal *c) {
    int biased=0;
    for (int i=0;i<4;++i) {
        const int a=abs(c->center[i]);
        c->base[i]=a>=PAD_CAL_BIAS ? a : 0;   /* a biased axis spans one half of the int16 range */
        c->max[i][0]=c->max[i][1]=0;
        biased|=c->base[i]!=0;
    }
    if (biased)
        printf("Runtime: pad travel (biased neutral, per-axis scaling): lx=%d ly=%d rx=%d ry=%d\n",
               c->base[0]?c->base[0]:32768,c->base[1]?c->base[1]:32768,
               c->base[2]?c->base[2]:32768,c->base[3]?c->base[3]:32768);
}
/* Full-scale travel of one direction. A biased axis starts from its neutral's magnitude and is
 * refined by the largest push seen (once that push is close to the base travel). */
static int cal_travel(const PadCal *c,int axis,int dir) {
    const int base=c->base[axis] ? c->base[axis] : 32768;
    return c->base[axis] && c->max[axis][dir]*100>=base*PAD_CAL_REFINE ? c->max[axis][dir] : base;
}
/* A newly opened pad has its own neutral. */
static void cal_reset(PadCal *c) {
    cal_load();
    memset(c,0,sizeof *c);
    if (cal_manual) { memcpy(c->center,cal_manual_center,sizeof c->center); c->have_center=1; }
    else if (!cal_enabled) c->have_center=1;
    if (c->have_center) cal_set_base(c);
}
/* The value each axis holds while the sticks are untouched (SDL's pre-report zeros are skipped);
 * once it is known, the largest push seen per direction on a biased axis (see cal_travel). */
static void cal_sample(PadCal *c,const int16_t raw[4]) {
    if (c->have_center) {
        for (int i=0;i<4;++i) {
            if (!c->base[i]) continue;
            const int d=raw[i]-c->center[i], dir=d>=0, travel=dir ? d : -d;
            if (travel>c->max[i][dir]) c->max[i][dir]=travel;
        }
        return;
    }
    int allzero=1; for (int i=0;i<4;++i) if (raw[i]) allzero=0;
    int quiet=1; for (int i=0;i<4;++i) if (c->started && abs(raw[i]-c->prev[i])>PAD_CAL_QUIET) quiet=0;
    const uint64_t now=now_us();
    if (!c->started) { c->started=1; c->since=now; }
    if (allzero && now-c->since<PAD_CAL_TIMEOUT_US) quiet=0;
    if (quiet) {
        if (++c->quiet_count>=PAD_CAL_QUIET_SAMPLES) {
            /* Only the clone pattern (all four axes biased) is taken as the neutral: a genuine pad
             * opened with a stick held steady would otherwise keep that push as its neutral. */
            int biased=0;
            for (int i=0;i<4;++i) biased+=abs(raw[i])>=PAD_CAL_BIAS;
            for (int i=0;i<4;++i) c->center[i]=biased==4 ? raw[i] : 0;
            c->have_center=1;
            if (biased==4)
                printf("Runtime: pad neutral: lx=%d ly=%d rx=%d ry=%d\n",c->center[0],c->center[1],c->center[2],c->center[3]);
            else if (biased)
                printf("Runtime: pad neutral 0 (%d of 4 axes off-center at open: a stick held, not a biased pad)\n",biased);
            cal_set_base(c);
        }
    } else c->quiet_count=0;
    for (int i=0;i<4;++i) c->prev[i]=raw[i];
}
/* One stick axis in the PS4 0..255 scale: neutral subtracted, the axis' own travel read as
 * -128..127, then the inner/outer dead zone mapped to the full deflection, as shadPS4 does. */
static uint8_t stick_axis(const PadCal *c,int axis,int16_t raw) {
    const int d=raw-c->center[axis];
    const int travel=cal_travel(c,axis,d>=0 ? 1 : 0);
    int v=travel>0 ? (int)((long long)d*128/travel) : 0;
    if (v>127) v=127; else if (v<-128) v=-128;
    const int mag=abs(v);
    if (mag<=pad_deadzone || pad_deadzone>=pad_deadzone_outer) v=0;
    else {
        int scaled=(int)(128.0*(mag-pad_deadzone)/(float)(pad_deadzone_outer-pad_deadzone));
        if (scaled>128) scaled=128;
        v = v>=0 ? scaled : -scaled;
    }
    int x=v+128;
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* BB_GAMEPAD (the launcher's controller choice): its SDL GUID, or part of its name. Issue #15:
 * wheels and other controllers connected for good came first. */
static const char *preferred_gamepad(void) {
    static const char *want; static int read;
    if (!read) { want=getenv("BB_GAMEPAD"); if (want && !*want) want=NULL; read=1; }
    return ports[0].want[0] ? ports[0].want : want;
}
static int is_preferred(SDL_JoystickID id, const char *want) {
    char guid[33];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id),guid,sizeof guid);
    const char *name=SDL_GetGamepadNameForID(id);
    return !strcasecmp(guid,want) || (name && strcasestr(name,want));
}
/* Any button, trigger or stick input on a gamepad right now. */
static int gamepad_active(SDL_Gamepad *g) {
    for (int b=0;b<SDL_GAMEPAD_BUTTON_COUNT;++b) if (SDL_GetGamepadButton(g,(SDL_GamepadButton)b)) return 1;
    static const SDL_GamepadAxis axes[]={SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY};
    for (size_t a=0;a<sizeof(axes)/sizeof(*axes);++a) { int v=SDL_GetGamepadAxis(g,axes[a]); if (v>12000 || v<-12000) return 1; }
    return SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)>8000 || SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)>8000;
}
/* The chosen gamepad, else the first while it is not connected (checked again every second, it
 * is taken as soon as it connects); the controller used last takes over (below); called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    static int on_preferred; static uint64_t last_scan;
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { note_disconnect(gamepad,1); SDL_CloseGamepad(gamepad); gamepad=NULL; }
    const uint64_t now=now_us();
    refresh_ports(now);
    const char *want=preferred_gamepad();
    /* SacredFire: in a container SDL gets no hot-plug notification (no udev), and the Steam Deck
     * sometimes shows a controller connected after launch only to a fresh enumeration. While the
     * chosen gamepad is missing, port 1 has no pad at all or a named port 2..4 is unfulfilled, the
     * gamepad subsystem is restarted every 5 s, which enumerates devices afresh (open handles are
     * dropped and reopened by the scans below); the in-game menu's "Rescan controllers" button does
     * it at once. */
    static uint64_t last_reinit; static unsigned reinits;
#define attempts pad_rescan_attempts
    /* Only ports with a controller bound to them are looked for (ports 2..4 without one are not
     * in use); port 1 always is, so it is looked for while it has no pad at all. After
     * PAD_RESCAN_ATTEMPTS misses in a row the bound controller counts as disconnected and the
     * rescans stop until the button, a binding change or a connected pad starts them again. */
    enum { PAD_RESCAN_ATTEMPTS=6 };
    int port_missing=0;
    for (int p=1;p<PORTS;++p) if (ports[p].want[0] && !is_keyboard(ports[p].want) && !ports[p].pad) port_missing=1;
    const int requested=bbgpu_pad_rescan_take();
    if (requested) attempts=0;
    static int allow_auto_reinit = -1;
    if (allow_auto_reinit < 0) allow_auto_reinit = (getenv("BB_PAD_AUTO_RESTART") != NULL);
    const int rescan_due=now-last_reinit>5000000 && attempts<PAD_RESCAN_ATTEMPTS;
    if (!gamepad || requested || (allow_auto_reinit && port_missing && rescan_due) || (want && !on_preferred && now-last_scan>1000000)) {
        last_scan=now;
        int count=0, pick=-1;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        /* The chosen gamepad bound to another port is connected, not missing: port 1 does without
         * it, and no restart of the gamepad subsystem drops that port's pad every 5 s. */
        int want_seen=0;
        for (int i=0; want && ids && i<count; ++i)
            if (is_preferred(ids[i],want)) { want_seen=1; if (pick<0 && !claimed_by_other_port(ids[i],0)) pick=i; }
        /* Under Windows/Wine, SDL_QuitSubSystem/SDL_InitSubSystem blocks for 400-500 ms while wineserver/HID
         * re-enumerates devices, causing periodic 5-second stutters if any port is unfulfilled.
         * Only perform a full subsystem restart if explicitly requested via the menu button, or if BB_PAD_AUTO_RESTART=1. */
        if (requested || (allow_auto_reinit && (((want ? !want_seen : count==0) || port_missing) && rescan_due))) { /* the plain scan found nothing usable */
            last_reinit=now;
            SDL_free(ids); ids=NULL;
            if (gamepad) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
            for (int p=1;p<PORTS;++p) if (ports[p].pad) { SDL_CloseGamepad(ports[p].pad); ports[p].pad=NULL; }
            for (int i=0;i<other_count;++i) if (other_gamepads[i]) {
                SDL_CloseGamepad(other_gamepads[i]); other_gamepads[i]=NULL;
            }
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) { sdl_ready=-1; return NULL; }
            ids=SDL_GetGamepads(&count);
            if (requested) printf("Runtime: controllers re-enumerated on request: %d found\n",count);
            else if (reinits++<3) printf("Runtime: %s; devices re-enumerated: %d found\n",want && pick<0 ? "chosen gamepad not seen" : "no gamepad or a port's controller missing",count);
            pick=-1; /* an index into the list before the restart */
            want_seen=0;
            for (int i=0; want && ids && i<count; ++i)
                if (is_preferred(ids[i],want)) { want_seen=1; if (pick<0 && !claimed_by_other_port(ids[i],0)) pick=i; }
            /* Found what was missing: attempts start over; else one more miss, the last one reported. */
            int still_missing=(want ? !want_seen : count==0);
            for (int p=1;p<PORTS && !still_missing;++p) if (ports[p].want[0] && !is_keyboard(ports[p].want) && !ports[p].pad) {
                still_missing=1;
                for (int i=0; ids && i<count; ++i) if (is_preferred(ids[i],ports[p].want)) still_missing=0;
            }
            if (!still_missing) attempts=0;
            else if (++attempts==PAD_RESCAN_ATTEMPTS)
                printf("Runtime: a bound controller was not found in %d rescans: treated as disconnected (the in-game menu's Rescan controllers tries again)\n",PAD_RESCAN_ATTEMPTS);
        }
        if (pick<0 && !gamepad && ids) for (int i=0;i<count && pick<0;++i) if (!claimed_by_other_port(ids[i],0)) pick=i;
        /* If player 1's chosen pad is absent, use the first available split-screen pad for
         * player 1. Otherwise its input can silently create a different user's save. */
        int port_fallback=0;
        if (pick<0 && !gamepad && ids && ports[0].want[0]) {
            for (int p=1;p<PORTS && pick<0;++p) {
                if (!ports[p].want[0] || is_keyboard(ports[p].want)) continue;
                for (int i=0;i<count;++i) if (is_preferred(ids[i],ports[p].want) &&
                    !claimed_by_other_port(ids[i],p)) { pick=i; break; }
            }
            if (pick>=0) {
                for (int p=1;p<PORTS;++p) if (ports[p].pad && SDL_GetGamepadID(ports[p].pad)==ids[pick]) {
                    SDL_CloseGamepad(ports[p].pad);
                    ports[p].pad=NULL;
                }
                port_fallback=1;
            }
        }
        if (pick>=0 && (!gamepad || SDL_GetGamepadID(gamepad)!=ids[pick])) {
            if (gamepad) SDL_CloseGamepad(gamepad);
            gamepad=SDL_OpenGamepad(ids[pick]);
            cal_reset(&cals[0]); /* a new pad has its own neutral */
            on_preferred=want && gamepad && is_preferred(ids[pick],want);
            if (gamepad) {
                ++connected_count;
                printf("Runtime: gamepad connected: %s%s\n",SDL_GetGamepadName(gamepad),
                       !want ? "" : on_preferred ? " (the chosen one)" : " (the chosen one is not connected)");
            }
        }
        SDL_free(ids);
        if (port_fallback && gamepad) update_port_presence();
    }
    /* SacredFire: the controller touched last is the active one. Every other connected gamepad is
     * kept open and polled ten times a second; one that produces input while the active one is idle
     * takes over (and ends the search for the BB_GAMEPAD choice). Pick up any pad and press a button. */
    static uint64_t last_switch_scan;
    if (now-last_switch_scan>100000) {
        last_switch_scan=now;
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        for (int i=0;i<other_count;++i) {
            SDL_Gamepad *other=other_gamepads[i];
            if (other && (!SDL_GamepadConnected(other) || claimed_by_other_port(SDL_GetGamepadID(other),0))) {
                SDL_CloseGamepad(other);
                other_gamepads[i]=NULL;
            }
        }
        for (int i=0; ids && i<count; ++i) {
            if (gamepad && SDL_GetGamepadID(gamepad)==ids[i]) continue;
            if (claimed_by_other_port(ids[i],0)) continue;
            int known=0;
            for (int j=0;j<other_count;++j) if (other_gamepads[j] && SDL_GetGamepadID(other_gamepads[j])==ids[i]) known=1;
            if (!known) {
                int slot=-1;
                for (int j=0;j<other_count;++j) if (!other_gamepads[j]) { slot=j; break; }
                if (slot<0 && other_count<8) slot=other_count++;
                if (slot>=0) other_gamepads[slot]=SDL_OpenGamepad(ids[i]);
            }
        }
        SDL_free(ids);
        if (gamepad && !gamepad_active(gamepad)) {
            for (int j=0;j<other_count;++j) {
                if (!other_gamepads[j] || !gamepad_active(other_gamepads[j]) ||
                    claimed_by_other_port(SDL_GetGamepadID(other_gamepads[j]),0)) continue;
                SDL_Gamepad *previous=gamepad;
                gamepad=other_gamepads[j]; other_gamepads[j]=previous; on_preferred=1;
                printf("Runtime: gamepad switched to %s (it was used)\n",SDL_GetGamepadName(gamepad));
                break;
            }
        }
    }
    static uint64_t last_presence;
    if (now-last_presence>1000000) { last_presence=now; update_port_presence(); }
    return gamepad;
}
#undef attempts
/* Controls: what each PS4 input is bound to. Defaults below; bbport.ini (BB_CONFIG) lines
 * key.<input>=<SDL key names> and pad.<input>=<SDL gamepad button names>, comma-separated,
 * replace an input's binding (empty: unbound). Inputs: the buttons (cross ... right, touchpad =
 * a left-side click, touchpad_right), and on the keyboard the sticks: move_* (left), look_*
 * (right). Gamepad names as SDL's: a b x y back start leftstick rightstick leftshoulder
 * rightshoulder dpup dpdown dpleft dpright touchpad misc1 paddle1-4, plus lefttrigger and
 * righttrigger. The keyboard works next to a gamepad (the Steam Deck always has one): its buttons
 * add to the gamepad's, a held move/look key moves the stick all the way. */
enum {
    IN_CROSS, IN_CIRCLE, IN_SQUARE, IN_TRIANGLE, IN_L1, IN_R1, IN_L2, IN_R2, IN_L3, IN_R3,
    IN_OPTIONS, IN_TOUCHPAD, IN_TOUCHPAD_RIGHT, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_MOVE_UP, IN_MOVE_DOWN, IN_MOVE_LEFT, IN_MOVE_RIGHT, IN_LOOK_UP, IN_LOOK_DOWN, IN_LOOK_LEFT,
    IN_LOOK_RIGHT, IN_COUNT
};
static const char *const input_names[IN_COUNT]={
    "cross","circle","square","triangle","l1","r1","l2","r2","l3","r3","options","touchpad",
    "touchpad_right","up","down","left","right","move_up","move_down","move_left","move_right",
    "look_up","look_down","look_left","look_right",
};
static const uint32_t input_buttons[IN_COUNT]={
    BTN_CROSS,BTN_CIRCLE,BTN_SQUARE,BTN_TRIANGLE,BTN_L1,BTN_R1,BTN_L2,BTN_R2,BTN_L3,BTN_R3,
    BTN_OPTIONS,BTN_TOUCHPAD,0,BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT,
};
#define MAX_BIND 4
enum { PAD_LEFT_TRIGGER=SDL_GAMEPAD_BUTTON_COUNT, PAD_RIGHT_TRIGGER }; /* triggers as buttons */
typedef struct { int key_count, pad_count; SDL_Scancode keys[MAX_BIND]; int pad[MAX_BIND]; } Binding;
static Binding bindings[IN_COUNT];
static int bindings_ready;

static void bind_defaults(void) {
    static const struct { int input; SDL_Scancode key; } keys[]={
        {IN_CROSS,SDL_SCANCODE_SPACE}, {IN_CIRCLE,SDL_SCANCODE_LSHIFT}, {IN_SQUARE,SDL_SCANCODE_E},
        {IN_TRIANGLE,SDL_SCANCODE_Q}, {IN_L1,SDL_SCANCODE_1}, {IN_R1,SDL_SCANCODE_3},
        {IN_L2,SDL_SCANCODE_R}, {IN_R2,SDL_SCANCODE_F}, {IN_L3,SDL_SCANCODE_Z}, {IN_R3,SDL_SCANCODE_C},
        {IN_OPTIONS,SDL_SCANCODE_RETURN}, {IN_TOUCHPAD,SDL_SCANCODE_TAB},
        {IN_TOUCHPAD_RIGHT,SDL_SCANCODE_BACKSPACE},
        {IN_UP,SDL_SCANCODE_I}, {IN_DOWN,SDL_SCANCODE_K}, {IN_LEFT,SDL_SCANCODE_J}, {IN_RIGHT,SDL_SCANCODE_L},
        {IN_MOVE_UP,SDL_SCANCODE_W}, {IN_MOVE_DOWN,SDL_SCANCODE_S}, {IN_MOVE_LEFT,SDL_SCANCODE_A},
        {IN_MOVE_RIGHT,SDL_SCANCODE_D}, {IN_LOOK_UP,SDL_SCANCODE_UP}, {IN_LOOK_DOWN,SDL_SCANCODE_DOWN},
        {IN_LOOK_LEFT,SDL_SCANCODE_LEFT}, {IN_LOOK_RIGHT,SDL_SCANCODE_RIGHT},
    };
    static const struct { int input, button; } pads[]={
        {IN_CROSS,SDL_GAMEPAD_BUTTON_SOUTH}, {IN_CIRCLE,SDL_GAMEPAD_BUTTON_EAST},
        {IN_SQUARE,SDL_GAMEPAD_BUTTON_WEST}, {IN_TRIANGLE,SDL_GAMEPAD_BUTTON_NORTH},
        {IN_L1,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {IN_R1,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {IN_L2,PAD_LEFT_TRIGGER}, {IN_R2,PAD_RIGHT_TRIGGER},
        {IN_L3,SDL_GAMEPAD_BUTTON_LEFT_STICK}, {IN_R3,SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {IN_OPTIONS,SDL_GAMEPAD_BUTTON_START},
        {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_BACK}, {IN_TOUCHPAD,SDL_GAMEPAD_BUTTON_TOUCHPAD},
        {IN_UP,SDL_GAMEPAD_BUTTON_DPAD_UP}, {IN_DOWN,SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {IN_LEFT,SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {IN_RIGHT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    memset(bindings,0,sizeof bindings);
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        Binding *b=&bindings[keys[i].input]; b->keys[b->key_count++]=keys[i].key;
    }
    for (size_t i=0;i<sizeof(pads)/sizeof(*pads);++i) {
        Binding *b=&bindings[pads[i].input]; b->pad[b->pad_count++]=pads[i].button;
    }
}
/* The mouse in key.<input>= lines, as the launchers capture it (tools/gpu_capabilities.c
 * --read-input; the Windows launcher's Keyboard tab): "Mouse Left", "Mouse Middle", "Mouse Right",
 * "Mouse X1", "Mouse X2", "Wheel Up", "Wheel Down" (spaces optional). Kept among the keys as
 * codes past the keyboard's scancodes; a wheel step holds its "key" for WHEEL_HOLD_MS. */
#define MOUSE_KEY_BASE SDL_SCANCODE_COUNT
enum { MOUSE_WHEEL_UP=6, MOUSE_WHEEL_DOWN=7, WHEEL_HOLD_MS=120 };
static const char *const mouse_names[]={"mouseleft","mousemiddle","mouseright","mousex1","mousex2", /* SDL_BUTTON_* 1..5 */
                                        "wheelup","wheeldown"};
static SDL_MouseButtonFlags mouse_buttons; /* this sample's */
static int mouse_focus;                    /* the game window has the focus: the mouse counts */
static _Atomic uint64_t wheel_until[2];   /* up, down: SDL_GetTicks() until which the step holds */
static bool SDLCALL wheel_watch(void *unused, SDL_Event *e) {
    (void)unused;
    if (e->type==SDL_EVENT_MOUSE_WHEEL && e->wheel.y!=0) {
        const float y=e->wheel.direction==SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        wheel_until[y>0 ? 0 : 1]=SDL_GetTicks()+WHEEL_HOLD_MS;
    }
    return true;
}
static SDL_Scancode key_from_name(const char *name) {
    char flat[32]; size_t n=0;
    for (const char *c=name;*c && n<sizeof flat-1;++c) if (*c!=' ') flat[n++]=(char)SDL_tolower((unsigned char)*c);
    flat[n]=0;
    for (int i=0;i<7;++i) {
        if (strcmp(flat,mouse_names[i])) continue;
        static int watching;
        if (i+1>=MOUSE_WHEEL_UP && !watching) watching=SDL_AddEventWatch(wheel_watch,NULL);
        return (SDL_Scancode)(MOUSE_KEY_BASE+i+1);
    }
    return SDL_GetScancodeFromName(name);
}
static int mouse_key_down(int button) {
    if (!mouse_focus) return 0;
    if (button>=MOUSE_WHEEL_UP) return SDL_GetTicks()<wheel_until[button-MOUSE_WHEEL_UP];
    return (mouse_buttons & SDL_BUTTON_MASK(button))!=0;
}
static int pad_button_from_name(const char *name) {
    if (!SDL_strcasecmp(name,"lefttrigger")) return PAD_LEFT_TRIGGER;
    if (!SDL_strcasecmp(name,"righttrigger")) return PAD_RIGHT_TRIGGER;
    const SDL_GamepadButton b=SDL_GetGamepadButtonFromString(name);
    return b==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)b;
}
/* key.<input>= / pad.<input>= lines of the settings file. */
static void load_bindings(void) {
    bind_defaults();
    load_ports();
    const char *path=getenv("BB_CONFIG");
    FILE *f=path ? fopen(path,"r") : NULL;
    if (!f) return;
    char line[512];
    while (fgets(line,sizeof line,f)) {
        const int keyboard=!strncmp(line,"key.",4), pad=!strncmp(line,"pad.",4);
        char *eq=strchr(line,'=');
        if ((!keyboard && !pad) || !eq) continue;
        *eq=0;
        int input=-1;
        for (int i=0;i<IN_COUNT;++i) if (!strcmp(line+4,input_names[i])) input=i;
        if (input<0 || (pad && input>=IN_MOVE_UP)) { printf("Runtime: controls: unknown input %s\n",line); continue; }
        Binding *b=&bindings[input];
        if (keyboard) b->key_count=0; else b->pad_count=0;
        for (char *name=strtok(eq+1,",\r\n"); name; name=strtok(NULL,",\r\n")) {
            while (*name==' ') ++name;
            for (char *end=name+strlen(name); end>name && end[-1]==' ';) *--end=0;
            if (!*name) continue;
            if (keyboard) {
                const SDL_Scancode s=key_from_name(name);
                if (s==SDL_SCANCODE_UNKNOWN) printf("Runtime: controls: unknown key \"%s\" for %s\n",name,line+4);
                else if (b->key_count<MAX_BIND) b->keys[b->key_count++]=s;
            } else {
                const int button=pad_button_from_name(name);
                if (button<0) printf("Runtime: controls: unknown gamepad button \"%s\" for %s\n",name,line+4);
                else if (b->pad_count<MAX_BIND) b->pad[b->pad_count++]=button;
            }
        }
    }
    fclose(f);
}
static int key_down(const bool *k, int input) {
    for (int i=0;i<bindings[input].key_count;++i) {
        const int key=(int)bindings[input].keys[i];
        if (key>MOUSE_KEY_BASE ? mouse_key_down(key-MOUSE_KEY_BASE) : k[key]) return 1;
    }
    return 0;
}
/* The bound gamepad buttons' state; triggers as their analog value. */
static int pad_value(SDL_Gamepad *g, int input) {
    int value=0;
    for (int i=0;i<bindings[input].pad_count;++i) {
        const int b=bindings[input].pad[i];
        const int v=b==PAD_LEFT_TRIGGER ? trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
                  : b==PAD_RIGHT_TRIGGER ? trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                  : SDL_GetGamepadButton(g,(SDL_GamepadButton)b) ? 255 : 0;
        if (v>value) value=v;
    }
    return value;
}

/* key held for the negative / positive direction: the stick all the way, else the gamepad's. */
static uint8_t key_axis(uint8_t value, int negative, int positive) {
    return negative || positive ? (uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0)) : value;
}
static void apply_keyboard(PadData *d, const bool *k) {
    for (int i=IN_CROSS;i<=IN_RIGHT;++i)
        if (i!=IN_TOUCHPAD && i!=IN_TOUCHPAD_RIGHT && key_down(k,i)) d->buttons|=input_buttons[i];
    if (key_down(k,IN_TOUCHPAD)) touch_click(d,0);
    if (key_down(k,IN_TOUCHPAD_RIGHT)) touch_click(d,1);
    if (key_down(k,IN_L2)) d->l2=255;
    if (key_down(k,IN_R2)) d->r2=255;
    d->left_x=key_axis(d->left_x,key_down(k,IN_MOVE_LEFT),key_down(k,IN_MOVE_RIGHT));
    d->left_y=key_axis(d->left_y,key_down(k,IN_MOVE_UP),key_down(k,IN_MOVE_DOWN));
    d->right_x=key_axis(d->right_x,key_down(k,IN_LOOK_LEFT),key_down(k,IN_LOOK_RIGHT));
    d->right_y=key_axis(d->right_y,key_down(k,IN_LOOK_UP),key_down(k,IN_LOOK_DOWN));
}

static void pad_neutral(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1;
}
/* The gamepad's part of a sample (d neutral); on the pad thread, under lock. */
static void read_gamepad(PadData *d, SDL_Gamepad *g, PadCal *cal) {
    {
        int touch_right=0;
        for (int i=IN_CROSS;i<=IN_RIGHT;++i) {
            const int v=pad_value(g,i);
            if (i==IN_L2) d->l2=(uint8_t)v;
            if (i==IN_R2) d->r2=(uint8_t)v;
            if (i==IN_TOUCHPAD_RIGHT) touch_right=v>30;
            else if (v>30) d->buttons|=input_buttons[i];
        }
        static const SDL_GamepadAxis stick_axes[4]={SDL_GAMEPAD_AXIS_LEFTX,SDL_GAMEPAD_AXIS_LEFTY,
                                                    SDL_GAMEPAD_AXIS_RIGHTX,SDL_GAMEPAD_AXIS_RIGHTY};
        int16_t raw[4];
        for (int i=0;i<4;++i) raw[i]=(int16_t)SDL_GetGamepadAxis(g,stick_axes[i]);
        cal_load();
        /* The neutral's quiet window counts samples at the game's rate (one a frame), not the
         * pad thread's. */
        const uint64_t now=now_us();
        if (now-cal->sampled>=PAD_CAL_INTERVAL_US) { cal->sampled=now; cal_sample(cal,raw); }
        uint8_t *axis_out[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
        for (int i=0;i<4;++i) *axis_out[i]=stick_axis(cal,i,raw[i]); /* center 0 until the neutral is known */
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
        if (touch_right) touch_click(d,1);
    }
}

/* SacredFire: SDL's device detection can block for a long time on Windows (DirectInput asking a
 * slow HID device its name, after any controller connects or turns off) while it holds SDL's
 * joystick lock; the game, reading the pad through SDL, froze until it was done. The bb:pad thread
 * now updates SDL and samples every port's gamepad into `snap`; the game's threads copy that
 * (snap_lock) and never wait on SDL. While an update is stuck, the gamepads read as idle after
 * PAD_STALE_US, and the keyboard and the game carry on. */
typedef struct { PadData data; int has_pad, keyboard; } PortSnap;
static pthread_mutex_t snap_lock=PTHREAD_MUTEX_INITIALIZER; /* snap, snap_at, rumble_want */
static PortSnap snap[PORTS];
static uint64_t snap_at;            /* now_us() of the last sample; 0 until the first */
static uint8_t snap_connected_count;
static struct { uint16_t low, high; int pending; } rumble_want[PORTS]; /* the game's last request */
static int pad_thread_state;        /* 0 not started, 1 running; -1: no thread, pad_read_state polls (tests) */
#define PAD_STALE_US 250000u
#define PAD_POLL_NS 2000000u
static void rumble(int slot, SDL_Gamepad *g, uint16_t low, uint16_t high);
/* One sample of every port's gamepad, and the rumble asked since the last one. */
/* Issue #50: what each port plays with, for the in-game Controllers tab (bbgpu_pad_status): the
 * gamepad's name, "keyboard" or none, and whether a button or a stick is held, so that pressing
 * a button shows which player each controller is (identical ones included). */
typedef struct { char name[64]; int pressing; } PortStatus;
static PortStatus status_sent[PORTS];
static int status_ready;
static int pad_pressing(const PadData *d) {
    const uint8_t sticks[4]={d->left_x,d->left_y,d->right_x,d->right_y};
    for (int i=0;i<4;++i) if (sticks[i]<88 || sticks[i]>168) return 1;
    return (d->buttons & ~BTN_TOUCHPAD)!=0 || d->touch_count>0;
}
static void report_status(const PortStatus *now) {
    for (int p=0;p<PORTS;++p) {
        if (status_ready && !strcmp(now[p].name,status_sent[p].name) && now[p].pressing==status_sent[p].pressing) continue;
        status_sent[p]=now[p];
        bbgpu_pad_status(p,now[p].name,now[p].pressing);
    }
    status_ready=1;
}
static void pad_poll(void) {
    PortSnap fresh[PORTS];
    PortStatus status[PORTS];
    struct { uint16_t low, high; int pending; } want[PORTS];
    pthread_mutex_lock(&lock);
    if (!bindings_ready) { load_bindings(); bindings_ready=1; }
    SDL_Gamepad *g=current_gamepad();
    for (int p=0;p<PORTS;++p) {
        const int keyboard=p ? is_keyboard(ports[p].want) : !keyboard_port();
        /* Ports 2..4: their own gamepad, or the keyboard (port.N=keyboard), once the game opened them. */
        SDL_Gamepad *pad=!p ? g : keyboard || !ports[p].opened ? NULL : port_gamepad(p);
        pad_neutral(&fresh[p].data);
        fresh[p].has_pad=pad!=NULL;
        fresh[p].keyboard=keyboard;
        if (pad) read_gamepad(&fresh[p].data,pad,&cals[p]);
        const char *name=pad ? SDL_GetGamepadName(pad) : keyboard ? "keyboard" : "";
        snprintf(status[p].name,sizeof status[p].name,"%s",name ? name : "gamepad");
        status[p].pressing=pad && pad_pressing(&fresh[p].data);
    }
    pthread_mutex_lock(&snap_lock);
    memcpy(want,rumble_want,sizeof want);
    for (int p=0;p<PORTS;++p) rumble_want[p].pending=0;
    pthread_mutex_unlock(&snap_lock);
    for (int p=0;p<PORTS;++p) {
        SDL_Gamepad *pad=p ? ports[p].pad : gamepad;
        if (want[p].pending && pad) rumble(p,pad,want[p].low,want[p].high);
    }
    const uint8_t count=connected_count;
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&snap_lock);
    memcpy(snap,fresh,sizeof snap);
    snap_at=now_us();
    snap_connected_count=count;
    pthread_mutex_unlock(&snap_lock);
    report_status(status);
}
static void *pad_thread(void *unused) {
    (void)unused;
    for (;;) {
        const uint64_t start=now_us();
        if (sdl_ready>0) SDL_UpdateJoysticks(); /* the window thread's event pump no longer does */
        const uint64_t took=now_us()-start;
        if (took>=PAD_STALE_US) {
            printf("Runtime: SDL took %.1f s to update the controllers (detecting devices); they read as idle meanwhile\n",
                   (double)took/1e6);
            fflush(stdout);
        }
        pad_poll();
        SDL_DelayNS(PAD_POLL_NS);
    }
    return NULL;
}
/* A port's sample for the game: its gamepad from the pad thread's last sample (idle once that is
 * PAD_STALE_US old), then the keyboard. Called under read_lock, never under lock. */
static int stale_noted;
static void sample_host_from(PadData *d, int p) {
    if (pad_thread_state<0) pad_poll();
    PortSnap s;
    pthread_mutex_lock(&snap_lock);
    s=snap[p];
    const uint64_t at=snap_at;
    const uint8_t count=snap_connected_count;
    pthread_mutex_unlock(&snap_lock);
    const uint64_t now=now_us();
    const int stale=at && now-at>PAD_STALE_US;
    if (stale && !stale_noted) {
        printf("Runtime: controller updates stalled (SDL is busy detecting devices): gamepads read as idle\n");
        fflush(stdout);
    }
    stale_noted=stale;
    if (!at || stale) { pad_neutral(&s.data); s.has_pad=0; }
    if (!at) s.keyboard=0; /* the controls are loaded with the first sample */
    *d=s.data;
    d->connected_count=count ? count : 1;
    d->timestamp=now;
    if (bbgpu_overlay_captures_input()) { pad_neutral(d); d->connected_count=count ? count : 1; d->timestamp=now; return; }
    const bool *k=s.keyboard && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (k) {
        /* Mouse buttons count, like keys, only while the game window has the focus. */
        mouse_focus=SDL_GetKeyboardFocus()!=NULL;
        mouse_buttons=mouse_focus ? SDL_GetMouseState(NULL,NULL) : 0;
        apply_keyboard(d,k);
    }
}
static void sample_host(PadData *d) { sample_host_from(d,0); }
/* Ports 2..4: their own gamepad, or the keyboard (port.N=keyboard); no scripted input. */
static void sample_port(PadData *d, int p) { sample_host_from(d,p); }

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* Touches as the DualShock 4 reports them: every finger that goes down gets a new id (1..127, kept
 * while it stays down) and the time since the first one went down. With id 0 and no hold time
 * the game ignored touchpad presses: no gesture menu from the touchpad, Back or Tab. */
static void touch_ids(PadData *d) {
    static uint8_t next_id=1, ids[2]; static int down[2]; static uint64_t since;
    for (int i=0;i<2;++i) {
        const int now=i<d->touch_count;
        if (now && !down[i]) { ids[i]=next_id; next_id=next_id==127 ? 1 : next_id+1; }
        down[i]=now;
        if (now) d->touches[i].id=ids[i];
    }
    if (!d->touch_count) since=0;
    else if (!since) since=d->timestamp;
    d->touch_held_time=d->touch_count ? (uint32_t)(d->timestamp-since) : 0;
}
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) return;
    record_sample(d);
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    touch_ids(d);
}

static pthread_mutex_t read_lock=PTHREAD_MUTEX_INITIALIZER; /* the game side of sampling (sample) */
static ABI int32_t pad_init(void) {
    pthread_mutex_lock(&lock);
    initialized=1;
    if (!pad_thread_state) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,pad_thread,NULL)==0) { pthread_detach(thread); pad_thread_state=1; }
        else { pad_thread_state=-1; puts("Runtime: no controller thread: the pads are read on the game's thread"); }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Handles are the user ids: 1 is the player's port, 2..4 the split-screen ports while assigned. */
static int handle_open(int32_t handle) {
    if (handle==PAD_HANDLE) return opened;
    return handle>=2 && handle<=PORTS && ports[handle-1].opened;
}
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<1 || user>PORTS) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already, assigned=1;
    if (user==1) { already=opened; opened=1; }
    else { assigned=ports[user-1].want[0]!=0; already=ports[user-1].opened; if (assigned) ports[user-1].opened=1; }
    pthread_mutex_unlock(&lock);
    if (!assigned) return ERR_INVALID_ARG;
    if (already) return ERR_ALREADY_OPENED;
    printf("Runtime: pad opened for user %d (%s)\n",user,user==1 ? "SDL gamepad or keyboard" : ports[user-1].want);
    return user;
}
static ABI int32_t pad_close(int32_t handle) {
    if (!handle_open(handle)) return ERR_INVALID_HANDLE;
    if (handle==PAD_HANDLE) opened=0; else ports[handle-1].opened=0;
    return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (!handle_open(handle)) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&read_lock);
    if (handle==PAD_HANDLE) sample(data); else sample_port(data,handle-1);
    ++reads; if (!pad_started_us) pad_started_us=now_us();
    pthread_mutex_unlock(&read_lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (!handle_open(handle)) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (!handle_open(handle)) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&snap_lock);
    info->connected_count=snap_connected_count ? snap_connected_count : 1;
    pthread_mutex_unlock(&snap_lock);
    return 0;
}
/* Rumble: the game sets it every frame, and in a race the strength changes almost every time.
 * Each change is a report sent to the controller; over Bluetooth that many (60+ a second) can
 * make Xbox and 8BitDo pads drop the connection. A new strength goes out at most every
 * RUMBLE_INTERVAL_US; stopping goes out at once, and the same strength again only renews SDL's
 * timer (SDL sends nothing for it). BB_RUMBLE=0 turns rumble off. Called under lock, by
 * pad_poll with the game's last request (pad_vibration). */
#define RUMBLE_INTERVAL_US 50000u
typedef struct { SDL_Gamepad *pad; uint16_t low, high; uint64_t at; } RumbleSent;
static RumbleSent rumble_state[PORTS];
static void rumble(int slot, SDL_Gamepad *g, uint16_t low, uint16_t high) {
    static int enabled=-1;
    if (enabled<0) { const char *v=getenv("BB_RUMBLE"); enabled=!(v && *v=='0'); }
    if (!enabled || slot<0 || slot>=PORTS) return;
    RumbleSent *r=&rumble_state[slot];
    const uint64_t now=now_us();
    const int same_pad=r->pad==g;
    if (same_pad && low==r->low && high==r->high) { SDL_RumbleGamepad(g,low,high,1000); return; }
    if (same_pad && (low || high) && now-r->at<RUMBLE_INTERVAL_US) return; /* a later frame sends it */
    SDL_RumbleGamepad(g,low,high,1000);
    r->pad=g; r->low=low; r->high=high; r->at=now;
    ++rumble_sent_count;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (!handle_open(handle)) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    const int slot=handle==PAD_HANDLE ? 0 : handle-1;
    pthread_mutex_lock(&snap_lock);
    ++rumble_calls;
    rumble_want[slot].low=(uint16_t)(param[0]*257);
    rumble_want[slot].high=(uint16_t)(param[1]*257);
    rumble_want[slot].pending=1;
    pthread_mutex_unlock(&snap_lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle_open(handle) ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }
/* SacredFire: the DualShock light bar: no host equivalent; a valid handle accepts any colour. */
static ABI int32_t pad_set_light_bar(int32_t handle, const void *color) { if (!color) return ERR_INVALID_ARG; return pad_ok_handle(handle); }

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadSetLightBar",pad_set_light_bar}, {"scePadResetLightBar",pad_ok_handle},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) {
    printf("Runtime: pad reads=%zu, gamepad=%s, rumble %llu requests, %llu sent\n",reads,
           gamepad ? SDL_GetGamepadName(gamepad) : "none",(unsigned long long)rumble_calls,(unsigned long long)rumble_sent_count);
}
