# CreamPuffRobot · WorkBuddy 会话归档

本目录把**用户级 WorkBuddy 会话记录中属于本项目的那部分**，从用户目录沉淀到仓库内，使其不随 WorkBuddy 的运行时清理而丢失。

- 归档时间：2026-10-09
- 项目根：`D:\workspace\projects\CreamPuffRobot`
- 源目录：`C:\Users\11318\.workbuddy-ai\`（旧版 WorkBuddy 2.137 ~ 2.147）、`C:\Users\11318\.workbuddy\`（新版 2.156）
- 数据范围：2026-09-14 ~ 2026-10-09

---

## 一、先说清楚一件事：这里没有「聊天原文」

WorkBuddy 的会话数据分三层，**只有后两层在本机存在**：

| 层 | 内容 | 本机是否有 |
| --- | --- | --- |
| 对话正文 | user / assistant 完整 messages（jsonl） | ❌ **没有**。旧版目录下无 `projects/`；新版从 2026-10-08 起才有，本项目只留下当前这一场 |
| 会话注册表 | pid / sessionId / cwd / 版本 / 起止时间 | ✅ 有，但仅为索引 |
| 运行时副产物 | 工具调用流水、文件版本快照、变更明细 | ✅ 有，且**可按 sessionId 还原到「哪次请求改了什么」** |

所以本归档能回答「某天 AI 执行过哪些命令、动过哪些文件、改了什么内容」，**不能**回答「当时双方具体说了什么」。真正的对话原话从未落到磁盘上——这一点在设计上就是如此，不是本次归档的疏漏。

---

## 二、目录结构

```
sessions-archive/
├─ README.md                 本说明
├─ index/                    轻量索引（建议入库）
│   ├─ sessions.json         会话登记表（机器可读，含每场会话的证据齐备情况）
│   ├─ sessions.md           会话清单（人可读表格）
│   └─ stats.json            归档统计与校验结果
├─ audit-log/                轻量：本项目工具调用流水（建议入库，3.8 MB）
│   └─ YYYY-MM-DD.jsonl      按日期分文件，已按 sessionId 过滤 + 去重
├─ raw/                      重量：原始证据快照（建议 gitignore，约 74 MB）
│   ├─ sessions/             会话注册表原始 json（按来源分子目录）
│   │   ├─ workbuddy-ai/     旧版
│   │   └─ workbuddy/        新版
│   ├─ file-history/         各会话涉及文件的**版本快照**（`<hash>@vN`）
│   ├─ changes-detail/       单次文件变更的完整明细（含旧/新内容）
│   ├─ changes-index/        各会话变更索引（summary + 文件列表）
│   └─ artifact-index/       各会话产出物索引
└─ tools/
    └─ archive_sessions.py   归档脚本（可重跑）
```

## 三、归档口径（白名单怎么来的）

会话身份以 `sessionId`（UUID）为准，白名单来源两条，**并集**：

1. **cwd 匹配**：`sessions/*.json` 中 `cwd` 等于本项目根路径 → 15 场。
2. **命令路径补入**：`audit-log` 中 `commandPreview` 出现本项目路径 → 补入 5 场（`index/sessions.json` 的 `extraSessionIdsByCmdPath`）。

合计 **20 场会话**、**3036 行工具调用流水**。

`index/sessions.json` 里每场会话带 `provenance` 字段标明来源，`cwd` 或 `cmd-path`。

### 为什么需要第 2 条

会话注册表**按 pid 命名文件**（如 `21412.json`），pid 会被系统复用后覆盖，导致历史会话的注册记录丢失。实测 31 个 pid 文件只对应 15 个 sessionId，而真实会话至少 20 场——若只按 cwd 筛，会漏掉 5 场。5 场补入会话的命令样例均指向本项目路径，已逐条核对，无外来会话混入。

## 四、已知缺口与异常

- **注册文件损坏**：`raw/sessions/workbuddy-ai/21412.json.corrupt` 原始内容为 462 字节全零（写入中断的典型产物），无法解析。它对应 2026-09-16 的一场会话，该场另经命令路径补入（`f52bed45-...`），流水未丢。
- **当前会话无流水**：`d8c7a773-...`（2026-10-09，本次归档所在会话）只有注册记录，audit-log 覆盖 19 / 20 个 sessionId。
- **流水非全量**：audit-log 只记录经安全审查的命令类事件，不含纯文本对话与部分只读操作。

## 五、证据关联链路

副产物之间可交叉还原，`sessionId` 是主键：

```
sessionId ──┬─ audit-log/<日期>.jsonl   每行含 requestId / toolCallId / commandPreview / decision
            ├─ changes-detail/<sessionId>/cd_<changeId>.<requestId>.json   单次变更的完整明细
            ├─ changes-index/<sessionId>.json                             该会话变更汇总
            ├─ artifact-index/<sessionId>.json                            该会话产出物
            └─ file-history/<sessionId>/<hash>@vN                         文件版本全文
```

`changes-detail` 与 `audit-log` 通过 `requestId` 对齐，可重建「某次请求触发了哪些命令、产生了哪些文件改动」。

## 六、入库口径（2026-10-09 用户裁定）

| 部分 | 体积 | 状态 |
| --- | --- | --- |
| `index/` | 20 KB | 入库 |
| `audit-log/` | 3.78 MB | 入库（信息独特，git 里没有等价物） |
| `raw/` | ~74 MB | **已排除** |

`raw/` 的绝大部分是**源码文件在各个时刻的内容快照**，而源码本身的版本历史已在 git 中，再存一份会显著拖重仓库，故整体不入库——已在 `.gitignore` 排除。实际入库 19 个文件（`README.md` 1 + `index/` 3 + `audit-log/` 14 + `tools/` 1）。

新克隆的仓库若需要 `raw/`，在本机跑一次归档即可重建（见第七节）——但前提是本机仍存在对应的用户级源目录，换机器无法凭归档本身恢复。

## 七、如何更新

对助手说「更新会话归档」即可，归档逻辑见 `tools/archive_sessions.py`：

```bash
python .workbuddy-ai/sessions-archive/tools/archive_sessions.py --check     # 只读：看有哪些会话、各多少流水
python .workbuddy-ai/sessions-archive/tools/archive_sessions.py            # 增量：覆盖写入，不删任何东西
python .workbuddy-ai/sessions-archive/tools/archive_sessions.py --rebuild  # 全量重建：先清空输出目录
```

每次运行都会：重新扫描两个用户级根目录重建 sessionId 白名单（含命令路径补入）→ 重写 `raw/`、`audit-log/`、`index/` → 输出统计。

- **默认（增量）模式不含任何删除操作**，仅覆盖同名文件；源端已消失的条目会残留，需要彻底清理时用 `--rebuild`。
- `--rebuild` 只清空 `raw/`、`audit-log/`、`index/` 三个自身输出目录，并对目标路径做前缀断言，**不会触碰 `README.md` 或 `tools/`**。
- 全程只读取用户级目录，不修改任何源文件；也不会触碰 `.workbuddy-ai/.git-commit-authorized`。
