#!/usr/bin/env bash
# Build and run the PagedControls host tests.
set -e
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
echo "Building PagedControls tests..."
$CXX -std=c++17 -O2 -Wall test_paged_controls.cpp -o test_paged_controls
echo
./test_paged_controls
