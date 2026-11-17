// SPDX-License-Identifier: GPL-2.0-or-later
// Voice chat in a private room: see bbport_voice.h and RyuNET server/voice.go.
//
// One thread does it all, every few milliseconds:
//   - the room: in a private room (src/runtime_room_mode.c names it after its host), this player
//     joins its voice on the server (GET /ryunet/voice/join, signed) and leaves it on leaving;
//   - the microphone (SDL, mono, 48 kHz) in frames of 20 ms: while the push-to-talk key is held,
//     or the open microphone is above its level, each frame goes out in Opus (24 kbit/s) by UDP
//     to the server's NAT port ("RYUVO1" | token | 1 | seq | Opus), with a keepalive every 5 s;
//   - the others' frames from the server ("RYUVO1" | 1 | nick | seq | Opus): one Opus decoder and
//     one SDL audio stream per speaker, all bound to the default playback device (SDL mixes them).

#include "bbport_voice.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <SDL3/SDL.h>
#include <opus/opus.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
#endif

#include "bbport_settings.h"

extern "C" {
int runtime_online_mode_room_on(void);                    // src/runtime_online_mode.c
void runtime_room_mode_host_name(char out[24]);           // src/runtime_room_mode.c
const char* runtime_np_get_online_name(void);             // src/runtime_np_online.c
int runtime_ryunet_request(const char* method, const char* target, char* body, size_t body_size);
int net_server_ipv4(uint32_t* ip);                        // src/runtime_net_bridge.c
uint16_t net_server_nat_port(void);
int net_bridge_active(void);
int runtime_public_match_active(void);                    // src/runtime_public_itemless.c
struct RuntimeVoiceKart { char name[24]; float pos[3]; int here; }; // src/runtime.h
int runtime_voice_karts(struct RuntimeVoiceKart* out, int max); // src/runtime_item_lead.c
}

namespace BbOverlay {
bool CapturesInput(); // bbport_overlay.cpp: the menu, the chat or a text field has the keyboard
}

namespace BbVoice {
namespace {

constexpr int kRate = 48000;
constexpr int kFrame = kRate / 50; // 20 ms
constexpr int kMaxPayload = 400;
constexpr char kMagic[6] = {'R', 'Y', 'U', 'V', 'O', '1'};
constexpr double kSpeakingMs = 300.0;

double NowMs() {
    using namespace std::chrono;
    return double(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

struct Speaker {
    OpusDecoder* decoder = nullptr;
    SDL_AudioStream* stream = nullptr; // stereo: the voice from its kart's side
    double last = 0;
    uint16_t seq = 0;
    bool any = false;
};

// ---- proximity: this player's kart (the listener), the way it goes, and the others' sides ----
struct Listener {
    bool valid = false;
    float pos[3] = {};
    float forward[3] = {1, 0, 0};
    float last[3] = {};
    bool has_last = false;
    float travel[3] = {}; // movement along each axis: the least is the vertical one
    int up = 2;           // Alchemy is Z-up; confirmed by the travel once the kart has moved
};
Listener g_listener;
RuntimeVoiceKart g_karts[8];
int g_kart_count = 0;
// In game units: full voice this near, silent this far (a track is several thousand across).
constexpr float kNear = 1500.0f, kFar = 9000.0f;

void UpdateListener() {
    g_kart_count = runtime_voice_karts(g_karts, 8);
    const RuntimeVoiceKart* me = nullptr;
    for (int i = 0; i < g_kart_count; ++i)
        if (g_karts[i].here) {
            me = &g_karts[i];
            break;
        }
    Listener& l = g_listener;
    if (!me) {
        l = Listener{};
        return;
    }
    l.valid = true;
    std::memcpy(l.pos, me->pos, sizeof l.pos);
    if (!l.has_last) {
        std::memcpy(l.last, l.pos, sizeof l.pos);
        l.has_last = true;
        return;
    }
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = l.pos[k] - l.last[k];
    for (int k = 0; k < 3; ++k) l.travel[k] += std::fabs(d[k]);
    if (l.travel[0] + l.travel[1] + l.travel[2] > 3000.0f)
        l.up = int(std::min_element(l.travel, l.travel + 3) - l.travel);
    d[l.up] = 0.0f;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len > 2000.0f) { // put back on the track: no direction from that
        std::memcpy(l.last, l.pos, sizeof l.pos);
    } else if (len > 40.0f) { // it moved: the way it goes is the way the camera looks
        for (int k = 0; k < 3; ++k) l.forward[k] = d[k] / len;
        std::memcpy(l.last, l.pos, sizeof l.pos);
    }
}

// The left and right gains of a speaker: its kart's distance and side from this player's kart.
void Gains(const std::string& nick, float volume, bool proximity, float& left, float& right) {
    left = right = volume * 0.70710678f; // centred, the same power as one side
    const Listener& l = g_listener;
    if (!proximity || !l.valid) return;
    const RuntimeVoiceKart* them = nullptr;
    for (int i = 0; i < g_kart_count; ++i)
        if (!g_karts[i].here && !SDL_strcasecmp(g_karts[i].name, nick.c_str())) them = &g_karts[i];
    if (!them) return; // not racing (a spectator, or between races): heard as in the lobby
    float to[3];
    for (int k = 0; k < 3; ++k) to[k] = them->pos[k] - l.pos[k];
    const float distance = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    const float loud = std::clamp((kFar - distance) / (kFar - kNear), 0.0f, 1.0f);
    to[l.up] = 0.0f;
    const float flat = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    float side = 0.0f;
    if (flat > 1.0f) {
        // right = forward x up (the up axis as a unit vector)
        float upv[3] = {0, 0, 0};
        upv[l.up] = 1.0f;
        const float* f = l.forward;
        const float r[3] = {f[1] * upv[2] - f[2] * upv[1], f[2] * upv[0] - f[0] * upv[2], f[0] * upv[1] - f[1] * upv[0]};
        side = std::clamp((to[0] * r[0] + to[1] * r[1] + to[2] * r[2]) / flat, -1.0f, 1.0f);
        if (BbSettings::Get().voice_swap_sides.load(std::memory_order_relaxed)) side = -side;
    }
    // constant power across the field; never quite silent on the far side
    const float angle = (side * 0.8f + 1.0f) * 0.25f * 3.14159265f;
    left = volume * loud * std::cos(angle);
    right = volume * loud * std::sin(angle);
}

std::mutex g_mutex;                       // g_speakers, g_state
std::map<std::string, Speaker> g_speakers; // by nickname, while heard lately
std::atomic<float> g_level{0.0f};
std::atomic<bool> g_talking{false};
std::atomic<bool> g_test{false};
std::atomic<int> g_state{int(State::Off)};
std::string g_me;

void SetState(State s) {
    g_state.store(int(s), std::memory_order_relaxed);
}

bool ValidNick(const std::string& n) {
    if (n.size() < 3 || n.size() > 16) return false;
    for (const char c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

void CloseSocket(Socket& s) {
    if (s == kNoSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    s = kNoSocket;
}

Socket OpenSocket() {
#ifdef _WIN32
    static const bool wsa = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!wsa) return kNoSocket;
#endif
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return s;
#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    return s;
}

void DropSpeaker(Speaker& sp) {
    if (sp.stream) SDL_DestroyAudioStream(sp.stream);
    if (sp.decoder) opus_decoder_destroy(sp.decoder);
    sp = Speaker{};
}

void DropSpeakers() {
    std::lock_guard lk{g_mutex};
    for (auto& [nick, sp] : g_speakers) DropSpeaker(sp);
    g_speakers.clear();
}

// GET /ryunet/voice/join: the token (16 bytes) and the server's name for the room, or false.
bool Join(const std::string& me, const std::string& room, uint8_t token[16], std::string& key) {
    char target[160], body[256] = {};
    std::snprintf(target, sizeof target, "/ryunet/voice/join?name=%s&room=%s", me.c_str(), room.c_str());
    const int status = runtime_ryunet_request("GET", target, body, sizeof body);
    const char* at = status == 200 ? std::strstr(body, "\"token\":\"") : nullptr;
    if (!at) {
        std::printf("Voice: joining %s's room: server answered %d\n", room.c_str(), status);
        std::fflush(stdout);
        return false;
    }
    at += 9;
    for (int i = 0; i < 16; ++i) {
        unsigned v = 0;
        if (std::sscanf(at + 2 * i, "%2x", &v) != 1) return false;
        token[i] = uint8_t(v);
    }
    std::string was = key;
    key.clear();
    if (const char* r = std::strstr(body, "\"room\":\"")) {
        for (r += 8; *r && *r != '"' && key.size() < 40; ++r) key.push_back(*r);
    }
    if (key != was) {
        std::printf("Voice: in room %s\n", key.c_str());
        std::fflush(stdout);
    }
    return true;
}

void Leave(const std::string& me) {
    char target[96], body[64];
    std::snprintf(target, sizeof target, "/ryunet/voice/leave?name=%s", me.c_str());
    runtime_ryunet_request("GET", target, body, sizeof body);
}

SDL_AudioStream* OpenMicrophone(const std::string& name) {
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!name.empty()) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
        for (int i = 0; ids && i < count; ++i) {
            const char* n = SDL_GetAudioDeviceName(ids[i]);
            if (n && name == n) id = ids[i];
        }
        SDL_free(ids);
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};
    SDL_AudioStream* mic = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (!mic) {
        std::printf("Voice: no microphone (%s)\n", SDL_GetError());
        std::fflush(stdout);
        return nullptr;
    }
    SDL_ResumeAudioStreamDevice(mic);
    return mic;
}

void Run() {
    OpusEncoder* encoder = nullptr;
    SDL_AudioStream* mic = nullptr;
    std::string mic_name;
    SDL_AudioDeviceID speakers = 0;
    SDL_AudioStream* self = nullptr; // the test: this player's own voice back
    Socket sock = kNoSocket;
    sockaddr_in server{};
    std::string room, me, room_key;
    uint8_t token[16] = {};
    bool joined = false;
    double next_join = 0, next_keepalive = 0, open_until = 0, last_level = 0;
    uint16_t seq = 0;
    for (;;) {
        auto& s = BbSettings::Get();
        const double now = NowMs();
        const bool enabled = s.voice_enabled.load(std::memory_order_relaxed);
        const bool test = g_test.load(std::memory_order_relaxed);
        // the room: a private one, named after its host
        const char* name = runtime_np_get_online_name();
        me = name ? name : "";
        // the room: a private one by its host; a public or Ranked one by the server ("match")
        std::string want;
        if (enabled && ValidNick(me)) {
            if (runtime_online_mode_room_on()) {
                char host[24] = {};
                runtime_room_mode_host_name(host);
                if (ValidNick(host)) want = host;
            } else if (runtime_public_match_active()) {
                want = "match";
            }
        }
        if (want != room) {
            if (joined) Leave(me);
            joined = false;
            room = want;
            room_key.clear();
            next_join = 0;
            DropSpeakers();
        }
        // a public room may change (its host left): asked again now and then; the same room keeps
        // the token, another one gives a new one and other voices
        if (!room.empty() && (!joined || room == "match") && now >= next_join) {
            const std::string was = room_key;
            const bool ok = Join(me, room, token, room_key);
            if (ok && joined && room_key != was) DropSpeakers();
            if (ok && !joined) next_keepalive = 0;
            joined = ok || (joined && room == "match");
            next_join = now + (joined ? 15000 : 10000);
        }
        if (!enabled && !test) {
            if (mic) SDL_DestroyAudioStream(mic), mic = nullptr;
            if (self) SDL_DestroyAudioStream(self), self = nullptr;
            DropSpeakers();
            if (speakers) SDL_CloseAudioDevice(speakers), speakers = 0;
            CloseSocket(sock);
            g_level = 0.0f;
            g_talking = false;
            SetState(State::Off);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        // devices and the socket
        if (!speakers) speakers = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        std::string want_mic;
        {
            std::lock_guard lk{BbSettings::PortsMutex()};
            want_mic = s.voice_input.data();
        }
        if (!mic || want_mic != mic_name) {
            if (mic) SDL_DestroyAudioStream(mic);
            mic = OpenMicrophone(want_mic);
            mic_name = want_mic;
        }
        if (!encoder) {
            int error = 0;
            encoder = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &error);
            if (encoder) opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
        }
        if (sock == kNoSocket && !net_bridge_active()) {
            uint32_t ip = 0;
            if (net_server_ipv4(&ip)) {
                sock = OpenSocket();
                server.sin_family = AF_INET;
                server.sin_addr.s_addr = ip;
                server.sin_port = htons(net_server_nat_port());
            }
        }
        if (test && !self && speakers) {
            const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};
            self = SDL_CreateAudioStream(&spec, nullptr);
            if (self) SDL_BindAudioStream(speakers, self);
        } else if (!test && self) {
            SDL_DestroyAudioStream(self), self = nullptr;
        }
        SetState(!mic ? State::NoMicrophone
                 : room.empty() ? State::NoRoom
                 : sock == kNoSocket ? State::NoServer
                 : joined ? State::Connected
                          : State::Joining);
        // the microphone, a frame at a time
        const float gain = s.voice_mic_gain.load(std::memory_order_relaxed);
        const bool open = s.voice_open_mic.load(std::memory_order_relaxed);
        const float threshold = s.voice_threshold.load(std::memory_order_relaxed);
        int key = s.voice_key.load(std::memory_order_relaxed);
        int16_t pcm[kFrame];
        while (mic && SDL_GetAudioStreamAvailable(mic) >= int(sizeof pcm)) {
            if (SDL_GetAudioStreamData(mic, pcm, sizeof pcm) != int(sizeof pcm)) break;
            float peak = 0.0f;
            for (int16_t& v : pcm) {
                const float x = std::clamp(v * gain, -32768.0f, 32767.0f);
                v = int16_t(x);
                peak = std::max(peak, std::fabs(x) / 32768.0f);
            }
            const float level = std::max(peak, float(last_level) * 0.85f);
            last_level = level;
            g_level.store(level, std::memory_order_relaxed);
            bool talking;
            if (open) {
                if (peak >= threshold) open_until = now + 400.0;
                talking = now < open_until;
            } else {
                int keys = 0;
                const bool* state = SDL_GetKeyboardState(&keys);
                talking = state && key > 0 && key < keys && state[key] && !BbOverlay::CapturesInput();
            }
            if (test) {
                if (self) SDL_PutAudioStreamData(self, pcm, sizeof pcm);
                talking = false; // the test sends nothing
            }
            g_talking.store(talking && joined, std::memory_order_relaxed);
            if (!talking || !joined || sock == kNoSocket || !encoder) continue;
            unsigned char packet[6 + 16 + 1 + 2 + kMaxPayload];
            std::memcpy(packet, kMagic, 6);
            std::memcpy(packet + 6, token, 16);
            packet[22] = 1;
            ++seq;
            packet[23] = uint8_t(seq), packet[24] = uint8_t(seq >> 8);
            const int bytes = opus_encode(encoder, pcm, kFrame, packet + 25, kMaxPayload);
            if (bytes > 0)
                sendto(sock, reinterpret_cast<const char*>(packet), 25 + bytes, 0,
                       reinterpret_cast<const sockaddr*>(&server), sizeof server);
        }
        if (!open && !test) {
            int keys = 0;
            const bool* state = SDL_GetKeyboardState(&keys);
            if (!(state && key > 0 && key < keys && state[key])) g_talking = false;
        }
        // keepalive: the server learns (and keeps) this player's address
        if (joined && sock != kNoSocket && now >= next_keepalive) {
            next_keepalive = now + 5000;
            unsigned char packet[25];
            std::memcpy(packet, kMagic, 6);
            std::memcpy(packet + 6, token, 16);
            packet[22] = 0, packet[23] = 0, packet[24] = 0;
            sendto(sock, reinterpret_cast<const char*>(packet), sizeof packet, 0,
                   reinterpret_cast<const sockaddr*>(&server), sizeof server);
        }
        // the others, from where their karts are
        const float volume = s.voice_volume.load(std::memory_order_relaxed);
        const bool proximity = s.voice_proximity.load(std::memory_order_relaxed);
        UpdateListener();
        for (;;) {
            unsigned char in[1024];
            sockaddr_in from{};
#ifdef _WIN32
            int from_len = sizeof from;
#else
            socklen_t from_len = sizeof from;
#endif
            if (sock == kNoSocket) break;
            const int n = int(recvfrom(sock, reinterpret_cast<char*>(in), sizeof in, 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_len));
            if (n <= 0) break;
            if (n < 10 || std::memcmp(in, kMagic, 6) || in[6] != 1) continue;
            const int nick_len = in[7];
            if (nick_len < 3 || nick_len > 16 || 8 + nick_len + 2 >= n) continue;
            const std::string nick(reinterpret_cast<const char*>(in + 8), size_t(nick_len));
            const uint16_t pseq = uint16_t(in[8 + nick_len] | in[9 + nick_len] << 8);
            const unsigned char* payload = in + 10 + nick_len;
            const int payload_len = n - 10 - nick_len;
            std::lock_guard lk{g_mutex};
            Speaker& sp = g_speakers[nick];
            if (!sp.decoder) {
                int error = 0;
                sp.decoder = opus_decoder_create(kRate, 1, &error);
                const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
                sp.stream = SDL_CreateAudioStream(&spec, nullptr);
                if (sp.stream && speakers) SDL_BindAudioStream(speakers, sp.stream);
            }
            if (!sp.decoder || !sp.stream) continue;
            float left = 1.0f, right = 1.0f;
            Gains(nick, volume, proximity, left, right);
            const auto put = [&](const int16_t* mono, int samples) {
                static int16_t stereo[kFrame * 6 * 2];
                for (int i = 0; i < samples; ++i) {
                    stereo[2 * i] = int16_t(std::clamp(mono[i] * left, -32768.0f, 32767.0f));
                    stereo[2 * i + 1] = int16_t(std::clamp(mono[i] * right, -32768.0f, 32767.0f));
                }
                SDL_PutAudioStreamData(sp.stream, stereo, samples * 4);
            };
            int16_t out[kFrame * 6];
            // a few lost frames: Opus fills them in
            const int gap = sp.any ? int(uint16_t(pseq - sp.seq)) - 1 : 0;
            for (int i = 0; i > -1 && i < std::min(gap, 3); ++i) {
                const int k = opus_decode(sp.decoder, nullptr, 0, out, kFrame, 0);
                if (k > 0) put(out, k);
            }
            sp.seq = pseq, sp.any = true;
            const int k = opus_decode(sp.decoder, payload, payload_len, out, kFrame * 6, 0);
            if (k > 0) {
                // never more than a quarter second behind
                if (SDL_GetAudioStreamQueued(sp.stream) > kRate * 4 / 4) SDL_ClearAudioStream(sp.stream);
                put(out, k);
            }
            sp.last = now;
        }
        {
            std::lock_guard lk{g_mutex};
            for (auto it = g_speakers.begin(); it != g_speakers.end();) {
                if (now - it->second.last > 10000) {
                    DropSpeaker(it->second);
                    it = g_speakers.erase(it);
                } else {
                    ++it;
                }
            }
            g_me = me;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

void Start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(Run).detach(); });
}

std::vector<std::string> Speaking() {
    std::vector<std::string> out;
    const double now = NowMs();
    std::lock_guard lk{g_mutex};
    if (g_talking.load(std::memory_order_relaxed) && !g_me.empty()) out.push_back(g_me);
    for (const auto& [nick, sp] : g_speakers)
        if (now - sp.last < kSpeakingMs) out.push_back(nick);
    return out;
}

bool Talking() {
    return g_talking.load(std::memory_order_relaxed);
}

float MicLevel() {
    return g_level.load(std::memory_order_relaxed);
}

std::vector<std::string> InputDevices() {
    std::vector<std::string> out;
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    for (int i = 0; ids && i < count; ++i)
        if (const char* n = SDL_GetAudioDeviceName(ids[i])) out.emplace_back(n);
    SDL_free(ids);
    return out;
}

State Now() {
    return State(g_state.load(std::memory_order_relaxed));
}

void SetTest(bool on) {
    g_test.store(on, std::memory_order_relaxed);
}

bool Testing() {
    return g_test.load(std::memory_order_relaxed);
}

} // namespace BbVoice
