# CreamPuffRobot — 项目级 WorkBuddy 配置

本目录是**项目级 WorkBuddy 配置**，随仓库共享给所有协作者。

## 目录结构

```
.workbuddy-ai/
├─ settings.json           # 权限 + hooks（提交入库，团队共享）
├─ settings.local.json     # 本机私有覆盖（已 gitignore）
├─ .git-commit-authorized  # 提交授权令牌（已 gitignore，运行时生成）
├─ hooks/                  # hook 脚本（Git Bash 执行）
└─ memory/                 # 项目记忆（已 gitignore）
```

> 本机还有一整套**用户级**配置在 `~/.workbuddy-ai/`（skills、plugins、sessions 等），
> 与项目级互不冲突：同名配置项按「项目级 > 用户级」合并。

## Hooks 一览

| 事件 | matcher | 脚本 | 作用 |
| --- | --- | --- | --- |
| PreToolUse | `Bash\|PowerShell` | `guard-git.sh` | **硬拦截** git commit/push（无用户授权令牌时 exit 2） |
| PreToolUse | `Bash\|PowerShell` | `guard-dangerous-bash.sh` | 拦截 `rm -rf` 源码目录、`.git` 删除、`reset --hard`、`clean -fdx` 等 |
| PreToolUse | `Bash\|PowerShell` | `prepare-build.sh` | 编译命令前自动结束运行中的 exe，预防 LNK1168 |
| PostToolUse | `Write\|Edit` | `check-script-encoding.sh` | 校验 .bat 纯 ASCII、含中文的 .ps1 带 UTF-8 BOM |
| SessionStart | `startup\|resume` | `session-brief.sh` | 注入本仓库的关键规约（编译/PowerShell、冒烟判定、记账等） |

### 修改 hooks 后必须重新加载

WorkBuddy 在启动时对 hooks 做快照，会话中途改文件不会立即生效。
改动后请在 `/hooks` 菜单里确认一次，或重启会话。

## 提交授权机制（重点）

用户 2026-09-14 的硬性要求：**没有收到提交指令之前，禁止提交**。

### 对 AI 助手

`git commit` / `git push` 会被 `guard-git.sh` 直接拦截（exit 2），
无论用 `cd &&`、`cmd /c`、`bash -c`、`eval`、引号包裹还是换行拼接都会被识别。
被拦后**不要重试、不要换写法绕过**，正确做法是：
先向用户说明要提交哪些文件、提交信息是什么，等用户明确同意。

注意：`git status` / `git diff` / `git log` / `git show` 等只读命令不受影响。
`git commit-graph verify` 这类名字含 commit 的只读命令也已排除误报。

### 对用户

下达提交指令后，创建授权令牌：

```bash
echo "用户指令原文" > .workbuddy-ai/.git-commit-authorized
```

- 令牌 **30 分钟**内有效，过期自动失效，无需手动清理。
- 想立即撤销：删除该文件即可。
- 令牌只能由用户创建。助手不能、也不会为自己创建令牌——这是该机制的全部意义。

## 自检

改完 hooks 后可以跑一遍内置用例（含 14 种绕过尝试 + 8 种误报检查）：

```bash
bash .workbuddy-ai/hooks/_selftest.sh
```

预期：所有 `expect BLOCK` 段落输出 `BLOCK`，所有 `expect allow` 段落输出 `allow`。

## 手动维护

- 手动压缩项目历史：对助手说「压缩项目历史」。
- 编辑 hooks 脚本后请同步更新 `_selftest.sh` 里的用例。
