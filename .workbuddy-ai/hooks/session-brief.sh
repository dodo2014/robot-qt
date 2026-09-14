#!/usr/bin/env bash
# session-brief.sh - inject the project's hard rules at session start.
#
# SessionStart stdout is added to the agent's context, so this is the cheapest
# way to make sure a fresh session knows the non-obvious traps of this repo
# without re-reading all of AGENTS.md.

set -uo pipefail

cat <<'EOF'
[CreamPuffRobot 项目级提示]
- 新会话先读 AGENTS.md（唯一权威技术事实源）；改核心逻辑前先读 TEST_RECORD.md 防回归。
- 文档/术语表述按 doc/doc_output_standards_guide.md：术语黑名单（机翻高危词）见其 §3，核心术语表见 §4，写文档/代码/评审前先过一遍。该指南只约束「怎么表述」，技术事实仍以 AGENTS.md 为准。
- git：没有用户明确的提交指令，禁止 commit/push（另有 PreToolUse 硬拦截）。
- 编译必须在 PowerShell 执行（Git Bash 调 cmd /c 会坏 vcvars 路径）；改动代码须同时编 Debug + Release。
- 编译前若 exe 在运行会撞 LNK1168；本仓库 hook 已在编译命令前自动结束进程。
- Sim 冒烟：out\smoke\sim_smoke.ps1，通过判定 = 当日日志含 "Initialize complete"（存活 != 通过）。
- 程序硬编码读工程根 config/config.json；不要把 exe 拷到别处做副本冒烟。
- .bat 必须纯 ASCII；.ps1 含中文必须带 UTF-8 BOM。
- 每修完 bug / 完成功能后，必须在 TEST_RECORD.md 追加一行台账。
- 项目记忆在 .workbuddy-ai/memory/（MEMORY.md + 每日日志）；跨会话历史在 .history/history.txt。
EOF

exit 0
