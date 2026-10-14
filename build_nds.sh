#!/usr/bin/env bash

set -euo pipefail

dsm_script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
dsm_clean_first=false

show_usage() {
    printf '%s\n' \
        'Usage: ./build_nds.sh [--clean]' \
        '' \
        'Build only the reusable Readmanna.nds reader.' \
        'No books or page images are processed or embedded.' \
        '' \
        '  --clean  Remove previous compiler output before building.' \
        '  --help   Show this message.'
}

while (($# > 0)); do
    case "$1" in
        --clean)
            dsm_clean_first=true
            ;;
        --help|-h)
            show_usage
            exit 0
            ;;
        *)
            printf 'Error: unknown option: %s\n\n' "$1" >&2
            show_usage >&2
            exit 2
            ;;
    esac
    shift
done

dsm_devkitpro="${DEVKITPRO:-}"
dsm_devkitarm="${DEVKITARM:-}"

if [[ -z "$dsm_devkitpro" || ! -d "$dsm_devkitpro" ]]; then
    if [[ -n "$dsm_devkitarm" && -f "$dsm_devkitarm/ds_rules" ]]; then
        dsm_devkitpro="$(cd -- "$dsm_devkitarm/.." && pwd -P)"
    elif [[ -d /opt/devkitpro ]]; then
        dsm_devkitpro=/opt/devkitpro
    else
        printf '%s\n' \
            'Error: devkitPro was not found.' \
            'Set DEVKITPRO to its installation directory (normally /opt/devkitpro).' >&2
        exit 1
    fi
fi

if [[ -z "$dsm_devkitarm" || ! -f "$dsm_devkitarm/ds_rules" ]]; then
    dsm_devkitarm="$dsm_devkitpro/devkitARM"
fi

if [[ ! -f "$dsm_devkitarm/ds_rules" ]]; then
    printf '%s\n' \
        "Error: $dsm_devkitarm/ds_rules was not found." \
        'Install the devkitPro NDS development group or correct DEVKITARM.' >&2
    exit 1
fi

if ! dsm_make="$(command -v make 2>/dev/null)"; then
    printf '%s\n' \
        'Error: GNU Make was not found in PATH.' \
        'Install make, then run this script again.' >&2
    exit 1
fi

export DEVKITPRO="$dsm_devkitpro"
export DEVKITARM="$dsm_devkitarm"

dsm_target='build/Readmanna'
dsm_rom="$dsm_script_dir/$dsm_target.nds"

if [[ "$dsm_clean_first" == true ]]; then
    "$dsm_make" -C "$dsm_script_dir" "TARGET=$dsm_target" clean
fi

"$dsm_make" \
    -C "$dsm_script_dir" \
    --always-make \
    "TARGET=$dsm_target" \
    NITRO_FILES=

if [[ ! -s "$dsm_rom" ]]; then
    printf 'Error: the build completed without creating %s\n' "$dsm_rom" >&2
    exit 1
fi

dsm_rom_bytes="$(wc -c < "$dsm_rom")"
printf '\nBuilt reader ROM:\n  %s (%s bytes)\n' "$dsm_rom" "$dsm_rom_bytes"
