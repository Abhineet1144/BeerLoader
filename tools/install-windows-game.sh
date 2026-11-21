#!/usr/bin/env bash
# Run a Windows game installer (Inno Setup, including repacks; other installers in GUI mode)
# under Wine, installing into a Linux directory that Beer can then launch from.
#
# Usage:
#   tools/install-windows-game.sh [--silent] [--ram-limit GB|none] [--prefix DIR] <setup.exe> <install-dir>
#
#   --silent         Inno Setup only: no wizard, default components, no prompts.
#   --ram-limit GB   Cap the installer and every tool it starts at GB GiB of RAM (a systemd
#                    cgroup, so it holds for any installer). Over the cap, memory goes to swap
#                    instead of the rest of the system. Asked interactively when not given and
#                    run from a terminal; `none` or empty means no cap. Env: BEER_INSTALLER_RAM_GB.
#                    Prefer this to a repack's own "limit RAM" checkbox (FH6's breaks its unpack).
#   --prefix DIR     Wine prefix to use (default: ~/.beer/installer-prefix). The prefix only
#                    hosts the installer; the game files go to <install-dir>.
#
# The installer log is written to <install-dir>/../<name>-install.log.
set -euo pipefail

silent=0
ram_limit="${BEER_INSTALLER_RAM_GB:-}"
prefix="${BEER_INSTALLER_PREFIX:-$HOME/.beer/installer-prefix}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --silent) silent=1; shift ;;
        --ram-limit) ram_limit="$2"; shift 2 ;;
        --prefix) prefix="$2"; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        --) shift; break ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[[ $# -eq 2 ]] || { sed -n '2,19p' "$0" >&2; exit 2; }

total_gb=$(( $(awk '/^MemTotal:/{print $2}' /proc/meminfo) / 1024 / 1024 ))
if [[ -z "$ram_limit" && -t 0 ]]; then
    read -r -p "[install] RAM limit for the installer in GiB (1-${total_gb}, empty = no limit): " ram_limit
fi
case "${ram_limit,,}" in
    ""|none|0) ram_limit="" ;;
    *[!0-9]*) echo "--ram-limit: expected whole GiB or 'none', got '$ram_limit'" >&2; exit 2 ;;
    *) (( ram_limit <= total_gb )) || { echo "--ram-limit: $ram_limit GiB exceeds the ${total_gb} GiB installed" >&2; exit 2; }
       command -v systemd-run >/dev/null || { echo "--ram-limit needs systemd-run" >&2; exit 1; } ;;
esac

setup="$(realpath -e "$1")"
target="$(realpath -m "$2")"
# Wine build: $WINE if set, else the newest GE-Proton9, else the newest GE-Proton, else the system wine.
# Verified with a FitGirl repack (32-bit Inno + ISDone, 64-bit helper tools):
#  - GE-Proton10-34 cannot load 64-bit EXEs with stripped relocations at their fixed 0x400000 base
#    (it maps them elsewhere and they crash at once in ntdll), so xtool.exe dies part-way through.
#  - Ubuntu's Wine 10 (experimental WoW64 mode) spins in the 32-bit setup before reading the archives.
#  - GE-Proton9-27 handles both.
ge_wine() {
    ls -d "$HOME"/.steam/steam/compatibilitytools.d/$1/files/bin/wine \
          "$HOME"/.local/share/Steam/compatibilitytools.d/$1/files/bin/wine 2>/dev/null | sort -V | tail -1 || true
}
wine_bin="${WINE:-}"
[[ -n "$wine_bin" ]] || wine_bin="$(ge_wine 'GE-Proton9-*')"
[[ -n "$wine_bin" ]] || wine_bin="$(ge_wine 'GE-Proton*')"
[[ -n "$wine_bin" ]] || wine_bin="$(command -v wine || true)"
[[ -x "$wine_bin" ]] || { echo "no Wine found (install wine or GE-Proton, or set WINE=)" >&2; exit 1; }
# Distributions often keep wineserver off PATH (Debian/Ubuntu: /usr/lib/<triplet>/wine).
wineserver="$(dirname "$wine_bin")/wineserver"
[[ -x "$wineserver" ]] || wineserver="$(command -v wineserver || ls /usr/lib/*/wine/wineserver /usr/lib/wine/wineserver 2>/dev/null | head -1 || true)"
[[ -x "$wineserver" ]] || { echo "wineserver not found" >&2; exit 1; }
wine() { "$wine_bin" "$@"; }
wineserver() { "$wineserver" "$@"; }
wineboot() { "$wine_bin" wineboot "$@"; }
winepath() { "$wine_bin" winepath "$@" 2>/dev/null; }
# One prefix per Wine build: a prefix upgraded by one build can misbehave under another.
# Proton-style builds live in <build>/files/bin/wine; the system wine keeps the plain prefix.
if [[ -z "${BEER_INSTALLER_PREFIX:-}" && "$prefix" == "$HOME/.beer/installer-prefix" &&
      "$wine_bin" == */files/bin/wine ]]; then
    prefix="$prefix-$(basename "$(dirname "$(dirname "$(dirname "$wine_bin")")")" | tr -c 'A-Za-z0-9.\n-' _)"
fi
echo "[install] wine: $wine_bin"
echo "[install] prefix: $prefix"

# Installers launch their unpack tools by bare name from the current directory. With this set
# (some sandboxes and shells export it) Windows search rules skip the current directory, the
# tool is never found and repacks report "archive data corrupted (decompression fails)".
unset NoDefaultCurrentDirectoryInExePath
# Repack installers are 32-bit and reserve large decompression buffers; Proton's Wine can give
# them the full 4 GiB address space (ignored by other Wine builds).
export WINE_LARGE_ADDRESS_AWARE="${WINE_LARGE_ADDRESS_AWARE:-1}"
mkdir -p "$target" "$prefix"
export WINEPREFIX="$prefix" WINEDEBUG="${WINEDEBUG:--all}" WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
if [[ ! -f "$prefix/system.reg" ]]; then
    echo "[install] creating Wine prefix $prefix"
    wineboot -i >/dev/null 2>&1
    wineserver -w
fi

# Free space check: an installer that runs out of space mid-unpack leaves a broken tree.
avail_gb=$(( $(df --output=avail -k "$target" | tail -1) / 1024 / 1024 ))
src_gb=$(( $(du -sk "$(dirname "$setup")" | cut -f1) / 1024 / 1024 ))
echo "[install] source ~${src_gb} GiB, free at target ${avail_gb} GiB"
if (( avail_gb < src_gb )); then
    echo "[install] not enough free space at $target" >&2; exit 1
fi

win_target="$(winepath -w "$target")"
name="$(basename "$target")"
log="$(dirname "$target")/${name}-install.log"
args=()
if grep -aq "Inno Setup" "$setup"; then
    args+=("/DIR=$win_target" "/SP-" "/NOICONS" "/LOG=$(winepath -w "$log")")
    (( silent )) && args+=("/VERYSILENT" "/SUPPRESSMSGBOXES" "/NORESTART" "/NOCANCEL")
    echo "[install] Inno Setup installer -> $win_target"
else
    (( silent )) && echo "[install] not Inno Setup: --silent ignored, choose $win_target in the wizard"
fi

# The cap applies to the cgroup, so it covers the tools the installer starts too (and the
# wineserver, which this first Wine process spawns). No server may be running already, or new
# processes would still be started from outside the cap.
limit=()
if [[ -n "$ram_limit" ]]; then
    wineserver -k 2>/dev/null || true
    limit=(systemd-run --user --scope --quiet --collect -p "MemoryMax=${ram_limit}G" -p "MemoryHigh=$(( ram_limit * 1024 * 9 / 10 ))M" --)
    echo "[install] RAM limit: ${ram_limit} GiB (of ${total_gb} GiB)"
else
    echo "[install] RAM limit: none"
fi

cd "$(dirname "$setup")"
start=$(date +%s)
set +e
"${limit[@]}" "$wine_bin" "$setup" "${args[@]}"
rc=$?
wineserver -w
set -e
mins=$(( ($(date +%s) - start) / 60 ))
echo "[install] installer exited with $rc after ${mins} min"

echo "[install] executables in $target:"
find "$target" -maxdepth 2 -iname '*.exe' -printf '  %P (%s bytes)\n' | sort
du -sh "$target"
[[ -f "$log" ]] && echo "[install] log: $log"
exit "$rc"
