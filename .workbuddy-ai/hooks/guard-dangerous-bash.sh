#!/usr/bin/env bash
# guard-dangerous-bash.sh - block destructive shell operations in this repo.
#
# Exit 0 = allow, Exit 2 = BLOCK (stdout message is fed back to the agent).
# Conservative by design: it only targets unambiguously destructive patterns,
# so normal build / smoke / log-reading commands are never affected.

set -uo pipefail

INPUT="$(cat)"

PROJECT_DIR="${CODEBUDDY_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

extract_cmd() {
  local json="$1"
  printf '%s' "$json" | sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\(.*\)".*/\1/p' | head -1
}

CMD_RAW="$(extract_cmd "$INPUT")"
CMD="$(printf '%s' "$CMD_RAW" \
  | sed -e 's/\\n/ /g' -e 's/\\r/ /g' -e 's/\\t/ /g' \
        -e 's/\\"/"/g' -e "s/\\\\'/'/g" -e 's/\\\\/\\/g')"

[ -z "$CMD" ] && exit 0

block() {
  cat <<EOF
[guard-dangerous-bash] BLOCKED: 检测到高危破坏性命令。

原因：$1

项目规约：本仓库（CreamPuffRobot）不做递归删除/清空，尤其禁止对以下路径操作：
  - 工程根及以上目录（D:/workspace/projects/CreamPuffRobot 或更上层）
  - src/ config/ doc/ tests/ 任意源码或配置目录
  - .git/（版本历史不可恢复）
  - out/ 之外的任何非生成目录

如果确实需要清理，请：
  1. 明确列出要删除的具体文件路径（不要用通配符批量删除）；
  2. 先向用户说明并取得确认；
  3. 优先用回收站语义或先备份。

被拦截的命令：
  $(printf '%s' "$CMD" | head -c 400)
EOF
  exit 2
}

# --- rm -rf / rm -fr / rm -r 加通配符, targeting anything but build artefacts ---
if printf '%s' "$CMD" | grep -Eqi '(^|[;&|[:space:]])(rm|rmdir)[[:space:]]+-[a-z]*[rf][a-z]*[[:space:]]+'; then
  # Allow only when the target is clearly a build/temp artefact.
  if ! printf '%s' "$CMD" | grep -Eqi 'rm[[:space:]]+-[a-z]*[rf][a-z]*[[:space:]]+[^;&|]*(out/|/tmp/|build_|\.log|\.obj|\.exe|\.pdb|\.ilk)' ; then
    block "递归删除命令，且目标不是已知的构建产物目录。"
  fi
fi

# --- Windows destructive deletions ---
if printf '%s' "$CMD" | grep -Eqi 'del[[:space:]]+/[sq]([[:space:]]|$)|rd[[:space:]]+/s|remove-item[[:space:]].*-recurse.*-force'; then
  block "Windows 递归删除命令（del /S /Q、rd /s、Remove-Item -Recurse -Force）。"
fi

# --- .git destruction ---
if printf '%s' "$CMD" | grep -Eqi '(rm|del|rd|remove-item)[^;&|]{0,80}\.git(/|\\\\|"|'"'"'|[[:space:]]|$)'; then
  block "试图删除 .git 目录，版本历史将不可恢复。"
fi

# --- git history rewrite / destructive discard ---
if printf '%s' "$CMD" | grep -Eqi '(^|[^a-z0-9_-])git([^a-z0-9_-]|$)[^;&|]{0,80}(reset[[:space:]]+--hard|clean[[:space:]]+-[a-z]*[fdx]|push[[:space:]].*--force|checkout[[:space:]]+--[[:space:]])'; then
  block "git 历史重写/强制丢弃本地改动（reset --hard / clean -fdx / push --force / checkout --）。"
fi

# --- fork bomb / disk destroy ---
if printf '%s' "$CMD" | grep -Eqi ':\(\)[[:space:]]*\{.*\};[[:space:]]*:|mkfs\.|dd[[:space:]].*of=/dev/'; then
  block "fork bomb 或磁盘设备写入命令。"
fi

# --- chmod/chown -R on the repo root ---
if printf '%s' "$CMD" | grep -Eqi '(chmod|chown)[[:space:]]+-[a-z]*R[[:space:]].*[[:space:]]("?/\.$|"?'"$PROJECT_DIR"'"?$|\.git)'; then
  block "对仓库根目录或 .git 递归变更权限/属主。"
fi

exit 0
