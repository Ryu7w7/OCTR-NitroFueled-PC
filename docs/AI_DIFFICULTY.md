# AI difficulty

Modified AI difficulty is available in private Single Race and Cup, in Normal or
Itemless, and in offline Arcade Single Race and Cup.

| Mode | Modified AI difficulty |
| --- | --- |
| Private Single: Normal / Itemless | Supported |
| Private Cup: Normal / Itemless | Supported |
| Offline Arcade Single / Cup | Supported |
| Battle | Not supported |
| Other Arcade modes | Not supported |
| Public matchmaking | Not supported |

Unsupported modes retain native difficulty choices and tuning. Leaving a
supported mode restores native tuning, so private difficulty settings do not
carry into public matchmaking or Battle.

## Private compatibility

With AI enabled, Hard, Very Hard, Extreme, and Nightmare require compatible
participants. Easy, Medium, and No AI do not require this capability.

Compatibility is checked before starting. A native notice names incompatible
players, and everyone stays in the lobby. Confirming the notice only closes it.

## Verification

I tested the modified AI difficulty compatibility gate with an outdated client.

Automated tests cover allowed and excluded modes, stale menu state, and restoring
native tuning after mode changes. The latest difficulty restrictions still need
in-game verification across the supported and excluded modes.

See [private race modes](PRIVATE_RACE_MODES.md) for the overall mode restrictions.
