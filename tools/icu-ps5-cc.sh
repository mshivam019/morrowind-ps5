#!/usr/bin/env bash
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
source "$root/tools/env.sh"
compiler="$PS5SDK_ROOT/native-app-boilerplate/tooling/prospero-clang18"
[[ $(basename "$0") == *cxx* ]] && compiler="$compiler++"
# ICU builds archives only. Configure's link checks are compile checks because
# executable PS5 linking requires the final application's platform linker.
args=("$@")
compile=false
for arg in "${args[@]}"; do case "$arg" in -c|-E|-S|--version|-v) compile=true;; esac; done
if $compile; then exec "$compiler" "${args[@]}"; fi
exec "$compiler" -c -o a.out "${args[@]}"
