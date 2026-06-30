#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Applying patches from $SCRIPT_DIR ..."
git am "$SCRIPT_DIR"/*.patch
echo "Done. All patches applied."
