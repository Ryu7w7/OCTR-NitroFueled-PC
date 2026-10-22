# Unified Windows Mods library

The former Patches page and separate package UI are consolidated into one Mods page.
Built-in INI-backed patches remain first. Add files / Add folder imports PAK, ZIP,
XML and unpacked packages into mods/; old patches/ XML stays readable. Creator
metadata is displayed through fixed-position expandable cards, using existing
ImGui and Windows dialog primitives. Shared child-window padding fixes rows that
previously touched the sidebar after the first item.

The loader validates declarative mod.json schema 2 packages, composes independent
archive-entry replacements, rejects conflicting whole-archive or executable writes,
and prepares private copied archives. It never patches through a hardlink. A package
session snapshots the selection, holds a lock and commits only its separate save
profile on successful exit. Standalone XML retains the legacy save behavior.

Two startup observations informed the implementation:

- Package compatibility rejection originally reached stderr without the saved run
  log. run.py now reports the reason through fail(), so the launcher and log agree.
- Windows once refused the final directory rename after all payload checks passed.
  The preserved ready profile validated and the identical rename later succeeded.
  Publication now retries only transient Windows access/sharing errors for a bounded
  interval; existing destinations and unrelated errors still fail. The exact locking
  process was not identified, and no system protection settings were changed.

The creator guide complements the supplied native PAK/IGZ handbook, retained with
its original attribution and contents. Online compatibility remains author-supplied
metadata, not server approval. No actual HUD mod or game resources are included.

Validation: native frontend build and visual UI review; 53 new synthetic tests pass.
The full Windows run has 177 passes, 38 skips and nine baseline failures/errors,
identical to unmodified main's failures (missing bash and Linux driver assumptions).
A separately packaged HUD composed with current 32:9, and offline launch was user-
confirmed. This does not certify arbitrary mods, multiplayer or every resolution.
Full engine/bash builds and Linux/AppImage integration were not performed here.
