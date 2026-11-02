// Standalone protocol test; does not need SDL, Vulkan, or the game.
#include "../gpu/shim/bbport_mod_sync.h"
#include <cassert>

int main() {
    using BbModSync::Parse;
    const std::string head = "CRASHNL_SYNC_1\n100.000\n";
    auto downloading = Parse(head + "downloading\n25\n100\nBig Smoke\n", 101);
    assert(downloading.visible && downloading.completed / downloading.total == 0.25);
    assert(Parse(head + "downloading\n25\n100\nBig Smoke\n", 111).phase == "error");
    assert(!Parse(head + "ready\n100\n100\nBig Smoke\n", 104).visible);
    assert(Parse(head + "ready\n100\n100\nBig Smoke\n", 102).visible);
    assert(!Parse(head + "hidden\n0\n0\n\n", 101).visible);
    assert(Parse(head + "fallback\n0\n100\nUsing Crash\n", 101).visible);
    assert(!Parse(head + "fallback\n0\n100\nUsing Crash\n", 109).visible);
    assert(!Parse(head + "downloading\n101\n100\nBig Smoke\n", 101).visible);
    assert(!Parse(head + "downloading\nnan\n100\nBig Smoke\n", 101).visible);
    assert(!Parse(head + "downloading\n1\n100\nBig Smoke\nextra\n", 101).visible);
    assert(!Parse(head + "downloading\n1\n100\nBig Smoke\n", 90).visible);
    assert(!Parse(head + "unknown\n1\n100\nBig Smoke\n", 101).visible);
    assert(!Parse(std::string(1025, 'a'), 101).visible);
}
