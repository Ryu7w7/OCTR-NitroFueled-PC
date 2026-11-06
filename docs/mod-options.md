# Configurable mods

The Windows Mods page shows a wrench for format-2 packages declaring
`configuration`. Other packages keep their existing controls. The dialog supports
dropdowns and checkboxes, Save, Cancel, and Reset to defaults. Close the game before
editing, and restart it to apply saved choices.
An option may declare `"advanced": true` to appear in the collapsed **Advanced
settings** section. It is still validated, saved, and applied when that section
is closed. Omit the field (or use `false`) for a normal option.

Boolean options may instead declare `"row": "Classic", "column": "Drift Meter"`.
The Windows dialog places these cells in one compact table, preserving declaration
order for rows and columns. Every cell must be unique; both labels are required,
plain text, at most 80 characters. A grid cell cannot also declare advanced=true.
No custom UI code or external library is used.

Independent choices can add static patches without multiplying every base variant:

```json
"stock-meter": {
  "type": "boolean", "label": "Stock drift meter", "default": true,
  "row": "Classic", "column": "Drift Meter",
  "patches": {"false": ["patches/stock-off.xml"], "true": ["patches/stock-on.xml"]},
  "useOperations": {"false": ["custom-art"], "true": []}
}
```

`patches` and `useOperations` are optional value-to-list mappings. Boolean keys
are the strings `true`/`false`; choice keys are choice IDs. If present, cover every
value, using empty arrays where nothing is needed. Operation names refer to the
manifest's `operationSets`; those still require a `variants` declaration. Every
referenced XML/payload is inventoried and validated, including inactive choices.
At launch, selected additions compose with the selected base variant. Ordinary
signature and overlap/conflict checks apply; these declarations cannot run scripts.
If both the base variant and an option select the same named operation set, that
set is applied once. Distinct sets still undergo ordinary resource conflict checks.

Add `mod-options` to `requiresCapabilities`; add `variants` when declaring base
variants or operationSets. Older launchers
reject the unknown capability rather than silently applying an arbitrary variant.
The engine needs no new feature: the launcher resolves a static variant before
preparing the existing file/archive overlays and verified executable patches.

Example fields to add to an otherwise complete format-2 manifest:

```json
{
  "requiresCapabilities": ["mod-options", "variants", "xml-patches"],
  "configuration": {
    "placement": {
      "type": "choice",
      "label": "Meter position",
      "description": "Choose where the meter appears.",
      "default": "corner",
      "choices": {"corner": "In the corner", "center": "Centered"}
    },
    "show-ticks": {
      "type": "boolean",
      "label": "Show tick marks",
      "default": true
    }
  },
  "variants": [
    {
      "id": "corner-ticks",
      "whenOptions": {"placement": ["corner"], "show-ticks": [true]},
      "patches": ["patches/corner-ticks.xml"]
    },
    {
      "id": "corner-plain",
      "whenOptions": {"placement": ["corner"], "show-ticks": [false]},
      "patches": ["patches/corner-plain.xml"]
    },
    {
      "id": "center-ticks",
      "whenOptions": {"placement": ["center"], "show-ticks": [true]},
      "patches": ["patches/center-ticks.xml"]
    },
    {
      "id": "center-plain",
      "whenOptions": {"placement": ["center"], "show-ticks": [false]},
      "patches": ["patches/center-plain.xml"]
    }
  ]
}
```

List all referenced XML/payloads in `files` using the normal package builder.
Shared `operations`, `patches`, and `operationSets` retain their usual meaning.
`whenOptions` conditions use typed arrays: JSON booleans for checkboxes and choice
IDs for dropdowns. An optional `when` object can also constrain game settings;
both conditions must match. Exactly one variant must match. Packages conditioned
only on options are checked for complete, unambiguous coverage when imported;
mixed game-setting/options variants are checked again at launch.

There are at most 16 options, 2–32 entries per dropdown, 2048 option combinations,
and 256 variants. Option/choice IDs are lowercase letters, digits and hyphens,
starting with a letter or digit and at most 80 characters. Labels are at most 160
characters; optional descriptions are at most 1,000. Both are single-line plain
text. No mod-supplied expressions, commands, Python, or UI code are evaluated.

Choices are stored in `mod-state/options.json`, keyed by the manifest's stable
mod ID and option IDs. Renaming a ZIP or upgrading its version retains compatible
choices. Removed choices, changed types, and new options use the author's defaults.
Keep IDs stable when their meaning stays the same. Disabling/re-enabling a mod
retains its choices; the launcher does not rewrite the installed package. Saved
effective choices participate in overlay cache identity and variant resolution.

Configuration saves verify the currently displayed package hash and validate all
submitted values. If the package changed, reload before editing again. Standard
signature, archive checksum, edition, online, and conflict checks still apply to
the selected variant. This feature does not establish compatibility by itself.

This change adds Windows UI and shared Python package support. A configuration
editor in the Linux launcher is not included.
