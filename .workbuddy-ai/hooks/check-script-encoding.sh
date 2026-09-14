#!/usr/bin/env bash
# check-script-encoding.sh - enforce ASCII-only .bat and BOM-on-.ps1 rules.
#
# Root cause it prevents (AGENTS.md, 2026-08-31 / 2026-09-01):
#   cmd.exe parses .bat as GBK -> Chinese comments become garbage commands
#       ('橀噺' 不是内部或外部命令)
#   PowerShell 5.1 parses BOM-less .ps1 as ANSI -> Chinese strings corrupt.
#
# Exit 0 = ok, Exit 2 = BLOCK (stderr is shown; message fed back to the agent).
# Non-blocking feedback is also emitted via additionalContext when the file is
# fine but worth a note.

set -uo pipefail

INPUT="$(cat)"

extract_field() {
  local json="$1" key="$2"
  printf '%s' "$json" | sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1
}

FILE_PATH="$(extract_field "$INPUT" 'file_path')"
if [ -z "$FILE_PATH" ]; then
  FILE_PATH="$(extract_field "$INPUT" 'path')"
fi
[ -z "$FILE_PATH" ] && exit 0
[ -f "$FILE_PATH" ] || exit 0

EXT="${FILE_PATH##*.}"
EXT_LOWER="$(printf '%s' "$EXT" | tr '[:upper:]' '[:lower:]')"

case "$EXT_LOWER" in
  bat|cmd)
    # Any byte >= 0x80 means non-ASCII content.
    if LC_ALL=C grep -qP '[\x80-\xFF]' "$FILE_PATH" 2>/dev/null; then
      cat <<EOF
[check-script-encoding] BLOCKED: $FILE_PATH 含非 ASCII 字符。

规约（AGENTS.md）：**.bat / .cmd 文件必须纯 ASCII**。
cmd.exe 按 GBK 解析 bat，中文注释会被拆成乱码命令并直接报错
（历史踩坑：'橀噺' 不是内部或外部命令）。

请把该文件里的所有中文注释/字符串改成英文 ASCII，然后重新保存。
EOF
      exit 2
    fi
    ;;
  ps1)
    # PowerShell 5.1 needs a UTF-8 BOM when the file contains non-ASCII text.
    if LC_ALL=C grep -qP '[\x80-\xFF]' "$FILE_PATH" 2>/dev/null; then
      HEAD3="$(head -c 3 "$FILE_PATH" | od -An -tx1 | tr -d ' \n')"
      if [ "$HEAD3" != "efbbbf" ]; then
        cat <<EOF
[check-script-encoding] BLOCKED: $FILE_PATH 含非 ASCII 字符但缺少 UTF-8 BOM。

规约（AGENTS.md）：**.ps1 含中文必须带 UTF-8 BOM**。
PowerShell 5.1 对无 BOM 文件按 ANSI 解析，中文字符串会乱码。

修复：以 UTF-8 with BOM 重新保存该文件。
EOF
        exit 2
      fi
    fi
    ;;
esac

exit 0
