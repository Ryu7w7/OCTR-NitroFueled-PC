// Discord Rich Presence for the launcher, over Discord's local IPC pipe
// (\\.\pipe\discord-ipc-N): no SDK or extra DLL. A background thread owns the pipe, so a
// missing or slow Discord never stalls the UI; it retries every few seconds and
// reconnects when Discord restarts.
//
// Invites: an activity with a party and a join secret gets Discord's "Join" button and the
// chat's "+ > Invite to play". Whoever accepts receives the secret in THEIR launcher, through
// TakeJoin().
#pragma once

#include <cstdint>
#include <string>

namespace DiscordPresence {

enum class Status { Off, NoAppId, NoDiscord, Connected, Error };

struct Activity {
    std::string details;  // first line, e.g. "Playing online"
    std::string state;    // second line, optional
    int64_t start = 0;    // Unix seconds; shows "elapsed"; 0 = none
    std::string large_image;  // asset key uploaded to the Discord application, optional
    std::string large_text;
    // Joinable: all four set, or none. Discord shows "(size of max)" after the state.
    std::string party_id;
    int party_size = 0;
    int party_max = 0;
    std::string join_secret;  // handed to whoever joins; at most 128 characters
};

/// Desired presence, called every frame: only a change reaches Discord. activity == nullptr
/// clears it; enabled == false (or an empty app_id) disconnects, which also clears it.
void Update(bool enabled, const std::string& app_id, const Activity* activity);

/// Accept "Ask to Join" requests on the player's behalf (the request only hands over the join
/// secret, the same thing an invite sent from the chat does).
void SetAcceptJoinRequests(bool accept);

/// The join secret of an invite or "Join" this player accepted in Discord, oldest first.
bool TakeJoin(std::string& secret);

/// Connection state, for the launcher's status line.
Status GetStatus();
std::string LastError();

/// Clears the presence and joins the thread. Safe to call more than once.
void Shutdown();

}  // namespace DiscordPresence
