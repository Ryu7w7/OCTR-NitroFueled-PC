// SPDX-License-Identifier: GPL-2.0-or-later
// Voice chat in a room, private, public or Ranked (RyuNET server/voice.go): the microphone in Opus
// to the server, which forwards it to the room's other players; their voices mixed into the
// speakers, in a race as loud as their karts are near and from their side (voice_proximity).
// Push to talk (a key, V by default) or open microphone (above a level). Off by default:
// the menu's Voice tab (bbport_overlay.cpp) turns it on and picks the microphone.

#pragma once

#include <string>
#include <vector>

namespace BbVoice {

/// Once, after the settings are read: the voice thread (it waits while voice is off).
void Start();

/// Nicknames heard in the last moments (this player's own when it speaks).
std::vector<std::string> Speaking();

/// This player's voice goes out now.
bool Talking();

/// The microphone's level, 0..1 (the Voice tab's meter), while voice is on or the test runs.
float MicLevel();

/// The recording devices' names (SDL), for the Voice tab.
std::vector<std::string> InputDevices();

/// For the Voice tab.
enum class State { Off, NoRoom, Joining, Connected, NoServer, NoMicrophone };
State Now();

/// The Voice tab's microphone test: this player hears itself, nothing is sent.
void SetTest(bool on);
bool Testing();

} // namespace BbVoice
