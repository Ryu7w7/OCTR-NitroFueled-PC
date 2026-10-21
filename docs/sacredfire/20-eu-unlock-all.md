# Unlock Everything: European edition 1.21

`patches/CrashTeamRacingUnlockAllEU.xml` adapts Camiloop's existing Unlock Everything
patch to **CUSA14876, AppVer 01.21**. The USA file is unchanged. EU 01.00 is not covered.

## Address verification

The first six patch addresses match the USA patch. The remaining eighteen addresses
are **0x10 earlier** in the verified EU executable. Each of the 24 writes replaces a
`cmp byte [reg+0x81], 0` (the `CBuildData::_unlockAll` flag) with `test rsp, rsp` and
NOP padding of exactly the same length, preserving the original patch's behavior.

All original instructions, replacement lengths and unique signatures were checked
against the local EU executable with SHA-256:

```
39d716b70a299b2546bd39996c8592f9375d3f18f9bb8bf11f9139e9af1eb3f5
```

Some nearby unlock functions have identical instruction sequences; those lines use
additional context and a nonzero `SignatureOffset` to distinguish the sites. The
signatures intentionally describe this verified build; other EU executable variants
have not been tested. The existing compiler skips the entire patch if a signature
cannot be resolved.

## Selection and testing

The patch is disabled by default. `BUNDLED_FILES` maps it to the existing USA selection
key, and both launchers map it to `patch_unlock_all`. Users select **Unlock Everything**
normally; no separate EU switch is necessary. Back up the save before enabling it.

The contributor reports successful in-game testing on EU 1.21. Automated checks cover
region/version filtering, default-off behavior, shared INI/JSON selection, both launcher
mappings, all 24 write lengths/addresses and continued USA selection. The real EU
executable was also used locally to resolve all signatures and compile all 24 writes;
no executable, save or generated patch binary is included in the contribution.
