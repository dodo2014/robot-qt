#!/usr/bin/env bash
# prepare-build.sh - kill a running CreamPuffRobot.exe before a build.
#
# Root cause it prevents (AGENTS.md):
#   LNK1168 "cannot open CreamPuffRobot.exe for writing" happens whenever the
#   user is still running the Debug exe. build_debug.bat / build_release.bat
#   already auto-retry, but doing it up front saves a whole failed build cycle.
#
# Exit 0 always (never blocks). Output goes to stderr so it never pollutes
# the agent-visible stdout channel.

set -uo pipefail

INPUT="$(cat)"

extract_cmd() {
  local json="$1"
  printf '%s' "$json" | sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\(.*\)".*/\1/p' | head -1
}

CMD_RAW="$(extract_cmd "$INPUT")"
CMD="$(printf '%s' "$CMD_RAW" | sed -e 's/\\n/ /g' -e 's/\\"/"/g')"
[ -z "$CMD" ] && exit 0

# Only act on real build invocations.
if ! printf '%s' "$CMD" | grep -Eqi '(ninja|cmake[[:space:]]+--build|msbuild|build_debug\.bat|build_release\.bat|vcvars64)'; then
  exit 0
fi

# Is the exe actually running?
if tasklist //FI "IMAGENAME eq CreamPuffRobot.exe" 2>/dev/null | grep -qi 'CreamPuffRobot.exe'; then
  echo "[prepare-build] CreamPuffRobot.exe is running -> stopping it to avoid LNK1168." >&2
  taskkill //IM CreamPuffRobot.exe //F >/dev/null 2>&1
  sleep 1
fi

exit 0
