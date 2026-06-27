#!/usr/bin/env bash
# Apply the two submodule patches into the checked-out submodules. Idempotent:
# re-running is a no-op once applied. See ../docs (usb-stack-saga.md) for *why*
# each patch exists and which library versions still need it.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

apply_patch() {  # <submodule-dir> <patch-file>
    local dir="$1" patch="$2" name
    name="$(basename "${patch}")"
    if git -C "${dir}" apply --reverse --check "${patch}" 2>/dev/null; then
        echo "   already applied: ${name}"
    elif git -C "${dir}" apply --check "${patch}" 2>/dev/null; then
        git -C "${dir}" apply "${patch}"
        echo "   applied:         ${name}"
    else
        echo "   !! cannot apply (context mismatch / wrong submodule rev?): ${name}" >&2
        return 1
    fi
}

echo ">> applying submodule patches ..."
apply_patch "${REPO}/external/pico-sdk/lib/tinyusb" \
            "${REPO}/patches/tinyusb-0.18-hub-descriptor.patch"
apply_patch "${REPO}/external/Pico-PIO-USB" \
            "${REPO}/patches/pico-pio-usb-0.7.2-pid-mismatch.patch"
