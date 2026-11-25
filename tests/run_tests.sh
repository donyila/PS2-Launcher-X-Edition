#!/bin/sh
# Host-side tests for the multi-profile path construction, the picture chooser
# grid and the profile identity rules used by Edit/Delete.
# These do not need the PS2SDK; any C compiler works.
set -eu

cd "$(dirname "$0")/.."

CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/ps2l_profiles_path_test"

$CC -std=c99 -Wall -Wextra -Werror -I. -o "$OUT" tests/profiles_path_test.c src/profiles_path.c src/profiles_grid.c
"$OUT"
