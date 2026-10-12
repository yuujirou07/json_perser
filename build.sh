#!/bin/sh
set -eu

cd "$(dirname "$0")"
cc -std=gnu17 -Wall -Wextra -Wpedantic -Iinclude src/*.c -o json_perser
