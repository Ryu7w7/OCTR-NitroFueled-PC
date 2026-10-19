#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared platform and publisher launch settings for the Windows and Linux runners."""
import argparse
import os
import sys
from pathlib import Path

# Add project root to sys.path to reuse setup_online
PORT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_ROOT))
from run import external_psn_backend, setup_online

EXPORT_KEYS = (
    "BB_USER_NAME", "BB_ONLINE", "BB_NET_TRACE", "BB_ONLINE_SERVER", "BB_ONLINE_NAME",
    "BB_ONLINE_ACCOUNT_ID", "BB_ONLINE_PORT", "BB_ONLINE_FRIENDS", "BB_ONLINE_KEY", "BB_ONLINE_KEYHASH", "BB_ONLINE_INVENTORY",
    "BB_THEME", "BB_PSN_BACKEND", "BB_PSN_BACKEND_CONFIG", "BB_ONLINE_TUNNEL",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--env-file", type=Path, required=True)
    args = parser.parse_args()
    env = dict(os.environ)
    try:
        setup_online(env, args.config, args.data)
        message = ("External PSN backend selected" if external_psn_backend(env) else
                   f"RyuNET Online configured for {env.get('BB_ONLINE_NAME', 'offline')}")
    except Exception as e:
        print(f"Online setup warning: {e}", file=sys.stderr)
        if external_psn_backend(env):
            return 1  # An explicitly selected backend never falls back to RyuNET.
        message = None
    with args.env_file.open("wb") as output:
        for key in EXPORT_KEYS:
            if key in env:
                output.write(f"{key}={env[key]}".encode("utf-8") + b"\0")
    print(message or "RyuNET Online disabled")


if __name__ == "__main__":
    sys.exit(main())
