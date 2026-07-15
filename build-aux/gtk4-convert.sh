#!/bin/sh
# Convert a GTK3 GtkBuilder .ui file to GTK4.
# The grep strips can-default, which gtk4-builder-tool leaves behind
# (see NetworkManager-fortisslvpn's Makefile.am).
set -e
tool="$1"
input="$2"
output="$3"
"$tool" simplify --3to4 "$input" | grep -v can-default > "$output"
