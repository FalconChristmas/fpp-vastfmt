#!/bin/bash
set -e

# Nothing to undo outside the plugin directory. The install only runs make in
# place, and FPP removes that directory itself, so there are no services,
# timers, cron entries, symlinks or files elsewhere to reverse. Clearing the
# build output keeps a later reinstall off stale objects; make clean is a no-op
# if the build never ran, so this is safe to run twice.
#
# The plugin's settings in config/plugin.fpp-vastfmt are deliberately left
# alone - they are the operator's own, and reinstalling should not lose the
# frequency and RDS text. The privacy block says so.
cd "$(dirname "$0")/.."
make clean 2>/dev/null || true

exit 0
