---
name: creampuff-commit
description: 在 CreamPuffRobot 仓库里安全地执行 git 提交——校验授权令牌、按文件名精确暂存（刻意排除 config/）、用临时文件承载中文提交消息、提交后复核。当用户说「执行提交 / 提交一下 / commit」时使用。
agent_created: true
---

# CreamPuffRobot 安全提交

## 红线（先看这条）

- **禁止主动 commit / push**。只有用户明确说「执行提交」才动手。
- **授权令牌 `.workbuddy-ai/.git-commit-authorized` 只能由用户创建**，助手**绝不代为创建**（TTL 30 min）。
  - 令牌不存在或已过期 → **停下，告知用户需要他创建令牌**，不要绕。
  - ⚠ 观察到的事实：令牌**不会被 hook 消费**，提交成功后仍留在盘上。**不要代为删除**，在汇报里提一句即可。
- **不 push**，除非用户明说。

## 第 1 步：前置检查

```bash
ls -la --time-style=+%H:%M:%S .workbuddy-ai/.git-commit-authorized ; date "+%H:%M:%S"
git status --short
```

令牌时间 + 30 min 必须晚于当前时间。过期即停。

## 第 2 步：暂存——显式列文件名，不用 `git add -A`

```bash
git add <文件1> <文件2> ...
git diff --cached --stat
```

**⚠ 必须刻意排除的两个文件**（用户手动测试产生的运行时改动，用户已表态「先不管」）：

- `config/config.json`
- `config/process.json`

暂存后 `git status --short` 里它们应仍是**未暂存的 ` M`**（第二列 M）。

## 第 3 步：改过 Markdown 表格 → 先扫列数

```bash
python <md-table-check skill>/scripts/check_md_tables.py TEST_RECORD.md <其他改过的 md>
```

末行「列数异常行数 = 0」才算干净。

## 第 4 步：提交消息走临时文件

**⚠ 严禁**把含**反引号 / `$` / 括号 / 中文**的多行消息塞进 shell 引号或 `python -c "…"`——
双引号内反引号照样触发命令替换，正文里的 `` `cmd` `` 会被**真的执行**。

```bash
# 用 Write 工具写 .workbuddy-ai/tmp_commit_msg.txt（该前缀已被 .gitignore 的 .workbuddy-ai/tmp_* 覆盖）
git commit -F .workbuddy-ai/tmp_commit_msg.txt
```

**⚠ 临时文件必须在 `git add` 之后写**，否则可能被误暂存。

## 第 5 步：消息风格（对齐既有提交）

```
<type>: <一句话主题> (TR-xxx, TR-yyy)

<背景：哪个用例 / 什么现象驱动>

- <改动点 1，含关键文件与函数名>
- <改动点 2>
...

验证：<编译结果 / 冒烟计数 / 预检结果>
```

`type` 用 `fix` / `feat` / `docs` / `config`；正文用 `-` 分点；末尾单列「验证：」段。
若某项判据仍待用户确认，**在末尾如实写明**，不要写成已完成。

## 第 6 步：提交后复核

```bash
git status --short          # 应只剩 config/config.json 与 config/process.json 两个未暂存
git log --oneline -3
git show --stat HEAD
rm -f .workbuddy-ai/tmp_commit_msg.txt
ls .workbuddy-ai/tmp_commit_msg.txt 2>/dev/null || echo TMP_REMOVED   # rm 会被静默拦截，必须复核
```

## 第 7 步：同步记录

- 提交号写进当日 `.workbuddy-ai/memory/YYYY-MM-DD.md`。
- 若提交含台账/状态变更，确认 `TEST_RECORD.md` 与 `doc/` 里对应状态已同步，**不留过时的「待复测」**。
