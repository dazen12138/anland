#!/bin/sh
# Build and run the host-side consumer reconnect state-machine test.
# Deliberately independent of CMake: the full non-Android CMake build pulls in
# PipeWire for the producer, while this test only needs the consumer + daemon.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/anland_consumer_reconnect_test

cc -std=c11 -O2 -g -Wall -Wextra \
    -I"$ROOT/libdisplay_consumer" \
    -I"$ROOT/libdisplay_daemon" \
    -I"$ROOT/common" \
    "$ROOT/libdisplay_consumer/tests/consumer_reconnect_test.c" \
    "$ROOT/libdisplay_consumer/display_consumer.c" \
    "$ROOT/libdisplay_daemon/display_daemon.c" \
    "$ROOT/common/socket_utils.c" \
    -pthread -o "$OUT"

exec "$OUT"
