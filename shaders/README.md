# Shader recipe

The first time a new player meets a track, racer, kart or effect, the port has to translate its
PS4 shaders (GCN) to Vulkan and build the pipelines, and the game waits for it: stops of tens to
hundreds of milliseconds, a few a minute in the first races. The port's shader cache keeps them
for the next sessions, but every new player and every new GPU or driver started from nothing.

`recipe.zip` lets the first start prepare them all at once instead. It holds the cache entries of
long play sessions without the parts that depend on a GPU:

- `.meta`: a shader permutation, the draw state it was translated for (resource table copy,
  vertex buffer descriptors, stage settings) and its resources;
- `.key`: a pipeline (the shaders of its stages and its fixed-function state).

It holds no game code. The shaders themselves come from the player's own dump:
`scripts/gcn_shaders.py` reads them from the game's `archives/*.pak` (the `shaders/` entries) into
`out/gcn_shaders.bin` when the archives change (about a second). Every PS4 shader carries a hash
in its binary info, the name the port's cache uses; the USA and EU editions (1.00 and 1.21) have
the same 3,001 shaders.

## At the start

`run.py` / `run.sh` set `BB_SHADER_RECIPE` and `BB_GCN_SHADERS`; the GPU library then, once per
cache (a new GPU, driver or port build starts a new cache):

1. translates each recipe entry again with this GPU's profile, reading the recorded draw state
   instead of game memory (`Shader::Info::Replay`), and has the driver check the result;
2. saves it in the player's cache, which loads it as always;
3. creates each recipe pipeline whose shaders were prepared, and saves those the driver accepts.

Shaders the player's cache already has stay as they are (recipe pipelines that use them are left
out). Anything not prepared is compiled during play, as before. An entry that stopped the game
while it was translated is left out at the next start (`recipe.attempt`, `recipe.bad` in the
cache folder); a cache found damaged turns the recipe off for that cache. The log says what was
done:

    Shaders: recipe prepared in 6.4 s: 358 shaders in 5.1 s (...), 309 pipelines in 1.3 s (...)

`BB_SHADER_RECIPE=0` leaves the recipe out; another path uses that file.

## Making the recipe

Play with an empty data folder for as long as possible (all tracks, racers, karts, modes; the
game's random rivals help), always in the same folder, so the cache numbers each shader's
permutations once, then:

    python tools/shader_recipe.py <data>/user/cache/CUSA13795 --out shaders/recipe.zip

Caches of different sessions or players cannot be combined file by file: each numbers the
permutations of a shader in the order it met them.
