#!/usr/bin/env bash
# press.sh <tokens> [hold_seconds]
f=$(dirname -- "$(realpath -- "$0")")/../out/pad
echo "$1" > $f; sleep ${2:-0.3}; : > $f
