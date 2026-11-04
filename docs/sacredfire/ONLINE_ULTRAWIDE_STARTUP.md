# Online startup with ultrawide layouts in v0.8

Enabling Online with a built-in 21:9 or 32:9 layout could stop preparation with
`unsupported native private team settings`. The team-settings adapter recognized
only the original USA/EU menus, while widescreen had already changed that asset.
Accepting the known widescreen menu then exposed a second failure: the public
matchmaking adapter read the original menu and collided with its widescreen
replacement (`entry 8897 is already replaced` on USA 1.21).

The adapters now accept the exact USA/EU widescreen hashes from the bundled
manifests. Public matchmaking reads the existing patched entry before adding its
runtime marker and behavior order, and explicitly composes that replacement.
Unknown menu edits and unrelated archive collisions remain rejected.

The native-asset regression builds each complete layout before running Online
preparation, covering both 21:9 asset variants and 32:9. It checks that private
settings change only the intended visibility/spacing bytes, private/public lobby
changes preserve widescreen layout, and unknown edits still fail validation.
All three USA variants reproduced the settings failure before the fix and pass
afterward. The previous lobby-only test did not exercise these additional menus.

The reporter confirmed the installed candidate works in a live Windows v0.8
test with Online and 32:9 enabled. This does not establish every race, transition,
resolution or multiplayer configuration. EU hashes match the bundled manifests;
no EU game dump or live EU test was available for this follow-up.
