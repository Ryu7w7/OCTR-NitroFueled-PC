// Discord account authorization for the launcher (OAuth2, scope identify), the browser half.
//
// Authorize listens on a loopback port, opens Discord's authorization page in the browser
// and waits for Discord to redirect the browser back with a code. The code is useless on its
// own: the RyuNET server, which holds the application's client secret, trades it for the
// player's Discord account (ryunet-demonware-ctr/server/discord_link.go).
#pragma once

#include <functional>
#include <string>

namespace DiscordLink {

/// Loopback ports tried in order; each redirect URI http://127.0.0.1:<port>/discord must be
/// registered in the Discord Developer Portal and allowed by the server.
inline constexpr int kPorts[] = {53134, 53135, 53136};

struct Result {
    bool ok = false;
    std::string code;          ///< authorization code (ok)
    std::string redirect_uri;  ///< the redirect it was issued for (the server needs it)
    std::string error;         ///< "denied", "timeout", "busy", "network" or Discord's error
};

/// Blocking (run it on a worker thread). open_url opens the authorization page; by default the
/// browser (tests pass their own). Spanish replies in the browser page when spanish is set.
Result Authorize(const std::string& client_id, int timeout_seconds, bool spanish,
                 const std::function<void(const std::string& url)>& open_url = {});

/// "Sign in through Steam" (OpenID 2.0) the same way: Steam's sign-in page in the browser, back to
/// http://127.0.0.1:<port>/steam. On success, code holds the raw query of that return address
/// (Steam's signed openid.* fields) for the server to verify with Steam
/// (ryunet-demonware-ctr/server/steam_link.go); the launcher never sees the password.
Result SteamSignIn(int timeout_seconds, bool spanish, const std::function<void(const std::string& url)>& open_url = {});

/// application/x-www-form-urlencoded value.
std::string FormEncode(const std::string& value);

}  // namespace DiscordLink
