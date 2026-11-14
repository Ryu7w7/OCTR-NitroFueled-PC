# Zydis 4.1.0, amalgamated

The single-file release of Zydis (Zyan Disassembler Library) and Zycore, as published at
https://github.com/zyantific/zydis/releases/tag/v4.1.0 (`zydis-amalgamated.zip`), unchanged:
`src/Zydis.c` and `include/Zydis/Zydis.h` (where `#include <Zydis/Zydis.h>` finds it).

The Windows build compiles it into sacredfire.exe (gpu/CMakeLists.txt) instead of shipping MSYS2's
`libzydis.dll`: a disassembler DLL on its own next to a game is what antivirus heuristics take for a
cheat tool, and v0.8.4's was flagged. Other builds use the system's Zydis.

License: MIT (LICENSE), Copyright (c) 2014-2024 Florian Bernd, Joel Hoener and Zydis contributors.
