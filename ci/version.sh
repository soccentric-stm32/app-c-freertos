#!/usr/bin/env bash
#
# version.sh - print the version this build reports, without a leading v
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
#
# Used by the Jenkins Package and Release stages.  The source of truth is
# APP_VERSION in CMakeLists.txt; the release tag must be v<this version>.
#

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
version="$(sed -n 's/^set(APP_VERSION "\([0-9.]*\)")/\1/p' CMakeLists.txt)"
if [[ -z "$version" ]]; then
    echo "version.sh: cannot read APP_VERSION from CMakeLists.txt" >&2
    exit 1
fi
printf '%s\n' "$version"
