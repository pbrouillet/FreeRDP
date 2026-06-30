#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Applying patches from $SCRIPT_DIR ..."
# Only the numbered git format-patch series is applied via git am.
# The combined copilot-agent.patch is a git-apply diff (see README.md).
git am "$SCRIPT_DIR"/[0-9]*.patch
echo "Done. All patches applied."
