#!/bin/zsh
set -euo pipefail
exec zsh "${0:A:h}/scripts/build-qt.sh" "$@"
