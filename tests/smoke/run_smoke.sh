#!/bin/bash

set -e

echo "Checking executables..."

test -f ../../bin/openmc-rq
test -f ../../bin/openmc-rs
test -f ../../bin/edge-receiver-rq
test -f ../../bin/edge-receiver-rs

echo "Checking configuration..."

test -d ../../config

echo "Smoke tests passed."
