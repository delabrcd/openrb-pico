#!/usr/bin/env bash
# Fast semantic C++ lint via clang-tidy inside the build container.
# Provides parse-level diagnostics (missing casts, type errors, use-after-free, etc.)
# plus modernize/bugprone/performance checks — without the multi-minute firmware build.
# Requires build/compile_commands.json (run scripts/build.sh once first).
#
#   scripts/check.sh                            # check git-changed .cpp/.h/.hpp in modules/+test/
#   scripts/check.sh modules/foo/bar.cpp        # check specific file(s)
#   scripts/check.sh modules/a.cpp test/b.cpp   # multiple explicit files
#   scripts/check.sh --list                     # print files that would be checked (dry-run)
#
# Override the build tree via ORB_CC_DIR (default: build).  E.g.:
#   ORB_CC_DIR=build-debug scripts/check.sh modules/foo.cpp
#
# ENGINE:  clang-tidy 18 reads compile_commands.json from the build tree.
# TARGET:  thumbv6m-none-eabi (RP2040 / Cortex-M0+, Thumb-only).
# ARM FIX: arm-none-eabi-g++ is probed at runtime for its sysroot include paths, which
#          are injected via --extra-arg=-isystem so ARM/newlib headers resolve correctly.
# CHECKS:  see .clang-tidy at the repo root (bugprone/modernize/performance/clang-diagnostic).
# OUTPUT:  host paths (/home/.../modules/...) via sed remap of container /work, so
#          file:line references are clickable from the host terminal / editor.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# ---- parse arguments ----
LIST_ONLY=0
EXPLICIT_FILES=()
for arg in "$@"; do
    case "$arg" in
        --list|-l) LIST_ONLY=1 ;;
        --help|-h)
            sed -n '2,/^[^#]/{ /^#/{ s/^# \{0,1\}//; p }; /^[^#]/q }' "$0"
            exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) EXPLICIT_FILES+=("$arg") ;;
    esac
done

# ---- require compile_commands.json ----
CC_DIR="${ORB_CC_DIR:-build}"
if [[ ! -f "${REPO}/${CC_DIR}/compile_commands.json" ]]; then
    echo "!! ${CC_DIR}/compile_commands.json not found -- run scripts/build.sh first" >&2
    exit 1
fi

# ---- collect source files to check ----
HOST_FILES=()
if [[ ${#EXPLICIT_FILES[@]} -gt 0 ]]; then
    for f in "${EXPLICIT_FILES[@]}"; do
        [[ "$f" != /* ]] && f="${REPO}/${f}"
        if [[ ! -f "$f" ]]; then
            echo "!! file not found: $f" >&2; exit 1
        fi
        HOST_FILES+=("$f")
    done
else
    # Changed-files mode: git diff HEAD + untracked files, limited to our source dirs.
    mapfile -t _changed < <(
        { git -C "${REPO}" diff --name-only HEAD 2>/dev/null || true; } \
            | grep -E '^(modules|test)/.*\.(cpp|h|hpp)$' || true
    )
    mapfile -t _untracked < <(
        { git -C "${REPO}" ls-files --others --exclude-standard 2>/dev/null || true; } \
            | grep -E '^(modules|test)/.*\.(cpp|h|hpp)$' || true
    )
    for rel in "${_changed[@]}" "${_untracked[@]}"; do
        [[ -z "$rel" ]] && continue
        f="${REPO}/${rel}"
        [[ -f "$f" ]] && HOST_FILES+=("$f")
    done
fi

if [[ ${#HOST_FILES[@]} -eq 0 ]]; then
    echo ">> no changed .cpp/.h/.hpp files found under modules/ or test/"
    echo "   Pass file paths explicitly, or make some edits first."
    exit 0
fi

# ---- dry-run listing ----
if [[ "${LIST_ONLY}" -eq 1 ]]; then
    echo ">> would check ${#HOST_FILES[@]} file(s):"
    for f in "${HOST_FILES[@]}"; do printf '   %s\n' "${f#${REPO}/}"; done
    exit 0
fi

# ---- convert host paths to container /work paths ----
C_FILES=()
for f in "${HOST_FILES[@]}"; do C_FILES+=("/work${f#${REPO}}"); done

echo ">> clang-tidy: ${#HOST_FILES[@]} file(s) [build tree: ${CC_DIR}]"
for f in "${HOST_FILES[@]}"; do printf '   %s\n' "${f#${REPO}/}"; done
echo ""

# ---- run clang-tidy inside the build container ----
# The container command:
#   1. Installs clang-tidy if absent (image rebuild after Dockerfile edit skips this).
#   2. Probes arm-none-eabi-g++ for its sysroot includes and injects them via -isystem.
#   3. Runs clang-tidy with --target=thumbv6m-none-eabi to match the RP2040 MCU.
#   4. Captures output to a temp file so the exit code is preserved after grep filtering.
#
# Output is piped through sed on the HOST to remap /work -> REPO root (host path).
# File paths without spaces only (standard for this project).
_C_FILES_STR="${C_FILES[*]}"
_CC_DIR_STR="${CC_DIR}"

build_run "
set -uo pipefail

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo '>> clang-tidy not in image; installing (rebuild image after Dockerfile edit to avoid this)...' >&2
    DEBIAN_FRONTEND=noninteractive apt-get update -qq >/dev/null \
        && apt-get install -y -q --no-install-recommends clang-tidy >/dev/null 2>&1
fi

_arm_incs=\$(arm-none-eabi-g++ -mcpu=cortex-m0plus -mthumb -v -x c++ /dev/null -fsyntax-only 2>&1 \
    | awk '/^#include <...> search starts here:/{p=1;next} /^End of search list/{p=0} p' \
    | sed 's/^[[:space:]]*//')

_extra=()
while IFS= read -r _inc; do
    [[ -n \"\$_inc\" && -d \"\$_inc\" ]] && _extra+=(\"--extra-arg=-isystem\$_inc\")
done <<< \"\$_arm_incs\"

_tmp=\$(mktemp)
clang-tidy \
    -p /work/${_CC_DIR_STR} \
    --extra-arg=--target=thumbv6m-none-eabi \
    --extra-arg=-Wno-unknown-warning-option \
    \"\${_extra[@]}\" \
    ${_C_FILES_STR} \
    > \"\$_tmp\" 2>&1; _rc=\$?

grep -Ev '^([0-9]+ (warning|error)s? generated\.?\$|Use -header-filter|Suppressed [0-9]+)' \
    \"\$_tmp\" || true
rm -f \"\$_tmp\"
exit \$_rc
" | sed "s|/work|${REPO}|g"
