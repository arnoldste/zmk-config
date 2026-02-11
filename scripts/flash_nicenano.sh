#!/usr/bin/env bash
set -euo pipefail

# Default location for downloaded UF2 files.
FW_DIR="$HOME/Downloads/Firmware"
MODE="normal"

usage() {
  cat <<'EOF'
Usage:
  flash_nicenano.sh [--dir PATH] [--mode normal|tester|main-only|reset-only|tester-only|list]

Modes:
  normal      settings_reset -> steering_wheel (default)
  tester      settings_reset -> tester_pro_micro
  main-only   only steering_wheel firmware
  reset-only  only settings_reset
  tester-only only tester_pro_micro
  list        list matching files and exit
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      FW_DIR="$2"
      shift 2
      ;;
    --mode)
      MODE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -d "$FW_DIR" ]]; then
  echo "Firmware folder not found: $FW_DIR" >&2
  exit 1
fi

export COPYFILE_DISABLE=1

find_latest_for_kind() {
  local kind="$1"
  local best=""
  local best_m=0
  local f=""
  local m=0
  local b=""
  local l=""
  while IFS= read -r -d '' f; do
    b="$(basename "$f")"
    l="$(printf '%s' "$b" | tr '[:upper:]' '[:lower:]')"
    case "$kind" in
      reset)
        [[ "$l" == *.uf2 ]] || continue
        [[ "$l" == *nice_nano* ]] || continue
        [[ "$l" == *settings*reset* ]] || continue
        ;;
      main)
        [[ "$l" == *.uf2 ]] || continue
        [[ "$l" == *nice_nano* ]] || continue
        [[ "$l" == *steering_wheel* ]] || continue
        [[ "$l" == *settings*reset* ]] && continue
        [[ "$l" == *tester_pro_micro* ]] && continue
        ;;
      tester)
        [[ "$l" == *.uf2 ]] || continue
        [[ "$l" == *nice_nano* ]] || continue
        [[ "$l" == *tester_pro_micro* ]] || continue
        ;;
      *)
        return 1
        ;;
    esac

    m="$(stat -f '%m' "$f")"
    if (( m > best_m )); then
      best_m="$m"
      best="$f"
    fi
  done < <(find "$FW_DIR" -maxdepth 1 -type f -name '*.uf2' -print0)

  if [[ -n "$best" ]]; then
    printf '%s\n' "$best"
    return 0
  fi
  return 1
}

find_uf2_volume() {
  local vol=""
  for vol in /Volumes/*; do
    [[ -f "$vol/INFO_UF2.TXT" ]] && { printf '%s\n' "$vol"; return 0; }
    [[ -f "$vol/INFO_UF2.txt" ]] && { printf '%s\n' "$vol"; return 0; }
  done
  return 1
}

wait_for_mount() {
  local timeout_s="${1:-60}"
  local waited=0
  local vol=""
  echo "Press RESET once now (enter UF2 mode)..." >&2
  while (( waited < timeout_s * 10 )); do
    if vol="$(find_uf2_volume)"; then
      printf '%s\n' "$vol"
      return 0
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  return 1
}

wait_for_unmount() {
  local vol="$1"
  local timeout_s="${2:-60}"
  local waited=0
  while (( waited < timeout_s * 10 )); do
    if [[ ! -d "$vol" ]] || [[ ! -f "$vol/INFO_UF2.TXT" && ! -f "$vol/INFO_UF2.txt" ]]; then
      return 0
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  return 1
}

flash_one() {
  local uf2="$1"
  local vol=""
  local target=""

  if [[ ! -f "$uf2" ]]; then
    echo "UF2 not found: $uf2" >&2
    return 1
  fi

  xattr -c "$uf2" 2>/dev/null || true

  echo
  echo "=== Flashing: $(basename "$uf2") ==="
  vol="$(wait_for_mount 90)" || {
    echo "Timeout waiting for UF2 drive. Check cable/bootloader." >&2
    return 1
  }
  if [[ ! -d "$vol" ]]; then
    echo "UF2 volume disappeared before write: $vol" >&2
    return 1
  fi
  target="$vol/FIRMWARE.UF2"
  cat "$uf2" > "$target"
  sync
  echo "Written to $target"
  echo "Do NOT press reset again. Waiting for auto-reboot..."
  wait_for_unmount "$vol" 90 || {
    echo "UF2 drive did not unmount in time." >&2
    return 1
  }
  echo "Flash done."
}

print_selection() {
  local reset_file="${1:-}"
  local main_file="${2:-}"
  local tester_file="${3:-}"
  echo "FW_DIR: $FW_DIR"
  echo "reset : ${reset_file:-<missing>}"
  echo "main  : ${main_file:-<missing>}"
  echo "tester: ${tester_file:-<missing>}"
}

RESET_UF2="$(find_latest_for_kind reset || true)"
MAIN_UF2="$(find_latest_for_kind main || true)"
TESTER_UF2="$(find_latest_for_kind tester || true)"

if [[ "$MODE" == "list" ]]; then
  print_selection "$RESET_UF2" "$MAIN_UF2" "$TESTER_UF2"
  exit 0
fi

print_selection "$RESET_UF2" "$MAIN_UF2" "$TESTER_UF2"

case "$MODE" in
  normal)
    [[ -n "$RESET_UF2" ]] || { echo "Missing settings_reset UF2 in $FW_DIR" >&2; exit 1; }
    [[ -n "$MAIN_UF2"  ]] || { echo "Missing steering_wheel UF2 in $FW_DIR" >&2; exit 1; }
    flash_one "$RESET_UF2"
    flash_one "$MAIN_UF2"
    ;;
  tester)
    [[ -n "$RESET_UF2"  ]] || { echo "Missing settings_reset UF2 in $FW_DIR" >&2; exit 1; }
    [[ -n "$TESTER_UF2" ]] || { echo "Missing tester_pro_micro UF2 in $FW_DIR" >&2; exit 1; }
    flash_one "$RESET_UF2"
    flash_one "$TESTER_UF2"
    ;;
  main-only)
    [[ -n "$MAIN_UF2" ]] || { echo "Missing steering_wheel UF2 in $FW_DIR" >&2; exit 1; }
    flash_one "$MAIN_UF2"
    ;;
  reset-only)
    [[ -n "$RESET_UF2" ]] || { echo "Missing settings_reset UF2 in $FW_DIR" >&2; exit 1; }
    flash_one "$RESET_UF2"
    ;;
  tester-only)
    [[ -n "$TESTER_UF2" ]] || { echo "Missing tester_pro_micro UF2 in $FW_DIR" >&2; exit 1; }
    flash_one "$TESTER_UF2"
    ;;
  *)
    echo "Invalid mode: $MODE" >&2
    usage
    exit 1
    ;;
esac

echo
echo "All requested flashes completed."
echo "If needed, unplug/replug USB and test:"
echo "  ioreg -p IOUSB -w0 -l | grep -E '\"USB Product Name\"|\"idVendor\"|\"idProduct\"'"
