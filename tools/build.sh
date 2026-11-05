#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# All generated files and optional dependency downloads stay inside this project.
build_path="${project_root}/build"
extra_args=()
if [[ -d "${project_root}/.deps/qt/usr/lib/x86_64-linux-gnu/cmake/Qt6" ]]; then
    extra_args+=("-DCMAKE_PREFIX_PATH=${project_root}/.deps/qt/usr")
    extra_args+=("-DCMAKE_LIBRARY_PATH=${project_root}/.deps/qt/usr/lib/x86_64-linux-gnu")
    extra_args+=("-DCMAKE_INCLUDE_PATH=${project_root}/.deps/qt/usr/include")
    export LD_LIBRARY_PATH="${project_root}/.deps/qt/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
cmake -S "${project_root}" -B "${build_path}" -DCMAKE_BUILD_TYPE=Release "${extra_args[@]}" "$@"
cmake --build "${build_path}" --parallel 2
ctest --test-dir "${build_path}" --output-on-failure
