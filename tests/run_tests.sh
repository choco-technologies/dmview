#!/bin/bash
# run_tests.sh - assemble the test views and run test_libdmview
#
# Usage: run_tests.sh <dmf dir> <fixtures source dir> <fixtures output dir>
#
# The views are fixtures/*.dmvs; todmv (from the todmv repository's
# manifest - it is not in the dmod registry) assembles them into the output
# directory, which test_libdmview was built to read (LIBDMVIEW_FIXTURES_DIR).
set -e

DMF_DIR="$1"
SOURCES="$2"
OUTPUT="$3"
TODMV_MANIFEST="${TODMV_MANIFEST:-https://raw.githubusercontent.com/choco-technologies/todmv/refs/heads/main/manifest.dmm}"

export DMOD_DMF_DIR="$DMF_DIR"
mkdir -p "$OUTPUT"

dmf-get install todmv -m "$TODMV_MANIFEST" -y > /dev/null
for source in "$SOURCES"/*.dmvs; do
    dmod_loader "$DMF_DIR/todmv.dmf" --args "$source" -o "$OUTPUT/$(basename "$source" .dmvs).dmv"
done

dmf-get install -d "$DMF_DIR/test_libdmview-local.dmd" -y
dmod_loader "$DMF_DIR/test_libdmview.dmf"
