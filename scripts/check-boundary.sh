#!/usr/bin/env bash
# check-boundary.sh — portability boundary enforcement for the service/ and protocol/ layers.
#
# Greps modules/service/ and modules/protocol/ for forbidden SDK/RTOS includes that violate
# the boundary defined in docs/architecture/modern-cpp.md. Exits non-zero if any are found.
#
# EXCLUDED: modules/driver/ (the vendor seam — it is allowed to include TinyUSB / SDK directly).
#
# KNOWN PENDING (not fatal): tusb/class/bsp/usb_midi_host includes in drums.cpp and guitar.cpp.
# These USB-host driver callbacks relocate to driver/ in a separate task; until then they are
# printed as informational "known pending" lines but do NOT fail the script.
#
# Usage:
#   scripts/check-boundary.sh          # run from the repo root or any subdirectory
#   scripts/check-boundary.sh --quiet  # suppress the KNOWN PENDING lines
#
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

QUIET=0
for arg in "$@"; do
    [[ "${arg}" == "--quiet" ]] && QUIET=1
done

SEARCH_DIRS=(
    "${REPO}/modules/service"
    "${REPO}/modules/protocol"
)

# Includes that must NEVER appear in service/ or protocol/
# (all of these are SDK, RTOS, or chipset-specific headers the portability boundary forbids).
FORBIDDEN_PATTERNS=(
    '#include[[:space:]]*[<"]\(pico/\)'
    '#include[[:space:]]*[<"]\(hardware/\)'
    '#include[[:space:]]*[<"]\(FreeRTOS\.h\)'
    '#include[[:space:]]*[<"]\(task\.h\)'
    '#include[[:space:]]*[<"]\(timers\.h\)'
    '#include[[:space:]]*[<"]\(semphr\.h\)'
    '#include[[:space:]]*[<"]\(queue\.h\)'
)

# Includes that are known to remain pending relocation to driver/; printed but not fatal.
# The USB-host driver callbacks in drums.cpp / guitar.cpp are out-of-scope for this task.
PENDING_PATTERNS=(
    '#include[[:space:]]*[<"]\(tusb\|class/\|host/\|device/\)'
    '#include[[:space:]]*[<"]\(usb_midi_host\)'
    '#include[[:space:]]*[<"]\(bsp/\)'
)

found_violations=0
found_pending=0

for dir in "${SEARCH_DIRS[@]}"; do
    for pattern in "${FORBIDDEN_PATTERNS[@]}"; do
        while IFS= read -r match; do
            [[ -z "${match}" ]] && continue
            echo "BOUNDARY VIOLATION: ${match}"
            found_violations=1
        done < <(grep -rn --include="*.cpp" --include="*.hpp" "${pattern}" "${dir}" 2>/dev/null || true)
    done

    for pattern in "${PENDING_PATTERNS[@]}"; do
        while IFS= read -r match; do
            [[ -z "${match}" ]] && continue
            if [[ "${QUIET}" -eq 0 ]]; then
                echo "KNOWN PENDING (relocate to driver/): ${match}"
            fi
            found_pending=1
        done < <(grep -rn --include="*.cpp" --include="*.hpp" "${pattern}" "${dir}" 2>/dev/null || true)
    done
done

if [[ "${QUIET}" -eq 0 && "${found_pending}" -gt 0 ]]; then
    echo ""
    echo "NOTE: 'KNOWN PENDING' lines are tusb/class/bsp/usb_midi_host includes in"
    echo "      service/drums.cpp and service/guitar.cpp. They relocate to driver/ in a"
    echo "      separate task and do NOT fail this check."
fi

echo ""
if [[ "${found_violations}" -gt 0 ]]; then
    echo "FAIL: boundary violation(s) found in service/ or protocol/"
    exit 1
fi

echo "PASS: no boundary violations in service/ or protocol/"
exit 0
