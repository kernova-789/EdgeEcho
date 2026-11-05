#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
player_path="${project_root}/build/edgeecho"
if [[ ! -x "${player_path}" ]]; then
    printf 'EdgeEcho executable was not found. Run %s/tools/build.sh first.\n' "${project_root}" >&2
    exit 1
fi
if [[ -d "${project_root}/.deps/qt/usr/lib/x86_64-linux-gnu" ]]; then
    export LD_LIBRARY_PATH="${project_root}/.deps/qt/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
export XDG_CACHE_HOME="${project_root}/.cache/qt/cache"
export XDG_CONFIG_HOME="${project_root}/.cache/qt/config"
export XDG_DATA_HOME="${project_root}/.cache/qt/data"
export XDG_RUNTIME_DIR="${project_root}/.cache/qt/runtime"
mkdir -p -- "${XDG_CACHE_HOME}" "${XDG_CONFIG_HOME}" "${XDG_DATA_HOME}" "${XDG_RUNTIME_DIR}"
chmod 700 -- "${XDG_RUNTIME_DIR}"
export TMPDIR="${XDG_RUNTIME_DIR}"
export TMP="${XDG_RUNTIME_DIR}"
export TEMP="${XDG_RUNTIME_DIR}"
cd -- "${project_root}"
exec "${player_path}" "$@"
