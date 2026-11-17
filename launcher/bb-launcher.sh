#!/usr/bin/env bash
# One ImGui launcher on Windows and Linux. --play keeps the headless Steam entry point.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
for launcher in "$here/../bin/sacredfire-launcher" "$here/../out/launcher/sacredfire-launcher"; do
    if [[ -x $launcher ]]; then exec "$launcher" "$@"; fi
done
bash "$here/build.sh"
exec "$here/../out/launcher/sacredfire-launcher" "$@"
