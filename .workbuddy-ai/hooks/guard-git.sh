#!/usr/bin/env bash
# guard-git.sh - HARD BLOCK unreviewed git commits/pushes.
#
# Project rule (CreamPuffRobot / AGENTS.md):
#   "Git: 禁止未经用户明确要求执行 commit/push"
#   Agent MUST NOT run git commit / git push on its own initiative.
#   A commit is only legal when the USER explicitly instructed it.
#
# Exit 0  = allow (read-only git command, or write command with valid authorization token)
# Exit 2  = BLOCK (message on stdout is fed back to the agent)
#
# Authorization token: .workbuddy-ai/.git-commit-authorized
#   Contains the user's instruction verbatim, e.g. "提交" / "commit 一下".
#   Valid for 30 minutes, then expires automatically.
#   Delete the file to revoke immediately.
#   The token is ALWAYS created by the user (or by a hook that verifies a
#   user message) - the agent must never create it for itself.

set -uo pipefail

INPUT="$(cat)"

PROJECT_DIR="${CODEBUDDY_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
TOKEN_FILE="$PROJECT_DIR/.workbuddy-ai/.git-commit-authorized"
TOKEN_TTL_SECONDS=1800

# ---- extract the command string from hook JSON (no jq dependency) ----
extract_cmd() {
  local json="$1"
  printf '%s' "$json" | sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\(.*\)".*/\1/p' | head -1
}

CMD_RAW="$(extract_cmd "$INPUT")"
# Unescape the common JSON escapes so the regexes below see real characters.
CMD="$(printf '%s' "$CMD_RAW" \
  | sed -e 's/\\n/ /g' -e 's/\\r/ /g' -e 's/\\t/ /g' \
        -e 's/\\"/"/g' -e "s/\\\\'/'/g" -e 's/\\\\/\\/g')"

[ -z "$CMD" ] && exit 0

# ---- normalise: collapse whitespace + newlines so "$( \n git commit" etc. still match ----
NORM="$(printf '%s' "$CMD" | tr '\n\r\t' '   ' | tr -s ' ')"

# Detect the write subcommand even when wrapped in: quotes, cd &&, cmd /c, bash -c,
# eval, powershell -Command, .bat scripts, etc. We look for the *pair* of tokens
# "git ... commit|push" appearing anywhere, which survives most wrapping forms.
is_git_write() {
  printf '%s' "$NORM" | grep -Eqi '(^|[^a-z0-9_-])git([^a-z0-9_-]|$)[^;&|]{0,120}(commit|push)([^a-z0-9_-]|$)'
}

is_git_write || exit 0

# ---- a git write was detected: look for a valid user authorization token ----
if [ -f "$TOKEN_FILE" ]; then
  MTIME="$(stat -c %Y "$TOKEN_FILE" 2>/dev/null || stat -f %m "$TOKEN_FILE" 2>/dev/null || echo 0)"
  NOW="$(date +%s)"
  AGE=$(( NOW - MTIME ))
  if [ "$AGE" -ge 0 ] && [ "$AGE" -lt "$TOKEN_TTL_SECONDS" ]; then
    # Valid, unexpired authorization -> let it through.
    exit 0
  fi
  REASON="授权令牌已过期（$(printf '%s' "$AGE") 秒前创建，TTL ${TOKEN_TTL_SECONDS}s）。"
else
  REASON="未找到授权令牌 $TOKEN_FILE。"
fi

cat <<EOF
[guard-git] BLOCKED: 检测到 git 写操作（commit/push），但本会话没有收到用户的提交指令。

项目规约（AGENTS.md / 用户 2026-09-14 明确要求）：
  「没有收到提交指令之前，禁止提交」——commit 与 push 都必须由用户显式下令。

${REASON}

你需要做的事：
 1. 不要重试，也不要用其它写法绕过（cd/&&/cmd /c/bash -c/eval/powershell 均会被识别）。
 2. 如果确实需要提交，先向用户说明准备提交哪些文件、提交信息是什么，等待用户明确同意。
 3. 用户下达提交指令后，由用户创建授权令牌：
      echo "用户指令原文" > .workbuddy-ai/.git-commit-authorized
    令牌 30 分钟内有效，之后自动失效。

当前被拦截的命令（截断显示）：
  $(printf '%s' "$CMD" | head -c 400)
EOF

exit 2
