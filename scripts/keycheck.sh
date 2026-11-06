#!/usr/bin/env bash
# Every config key the plugin READS must be spelled with the current prefix.
#
# WHY THIS EXISTS. The Phase-9 rename moved every key to plugin:hyprdrive: and
# kept plugin:scrolloverview: registered as an alias so old configs keep
# working. That combination has a failure mode with no symptom at build time and
# no error at runtime: a getter left spelled with the OLD prefix still RESOLVES
# -- to the alias -- and quietly returns the registered default forever, because
# nothing sets the alias. The plugin runs, logs nothing, and ignores the user.
#
# Measured 2026-08-12: 47 such literals survived the rename in Dock.cpp,
# Locations.cpp, Window.cpp and OverviewInternal.hpp. dock:autohide therefore
# read `true` no matter what any config or gate wrote, so the dock never
# appeared on a plain desktop, and five green gates went red -- x32, y11, y4,
# z1, z2 -- with five different-looking failures and one cause. A grep would
# have caught it in a second; two suite runs and a bisect did instead.
#
# Compat.cpp is exempt: it is the file whose whole job is to know both names.
set -u
cd "$(dirname "$0")/.."

BAD=$(grep -n '"plugin:scrolloverview:' -- src/*.cpp src/*.hpp 2>/dev/null | grep -v '^src/Compat\.')
if [ -n "$BAD" ]; then
    echo "keycheck: FAIL -- these read a key through the deprecated alias, which"
    echo "silently returns the registered default because nothing ever sets it:"
    printf '%s\n' "$BAD" | sed 's/^/  /'
    exit 1
fi

echo "keycheck: OK -- every config key is read under the current prefix"
