# -*- coding: utf-8 -*-
"""CreamPuffRobot · WorkBuddy 会话记录归档脚本

把用户级 WorkBuddy 会话记录中属于本项目的部分，归档到
    .workbuddy-ai/sessions-archive/

用法：
    python tools/archive_sessions.py            # 增量归档（覆盖同名文件，不删除任何东西）
    python tools/archive_sessions.py --rebuild  # 先清空 raw/audit-log/index 再全量重建
    python tools/archive_sessions.py --check    # 只做只读统计，不写盘

设计约束：
  * 只读取用户级目录，不修改任何源文件；
  * 默认模式**不含任何删除操作**，仅覆盖写入；
  * --rebuild 只清空本脚本自己的输出目录（sessions-archive/raw|audit-log|index），其余一律不动；
  * README.md 不属于输出目录，任何模式下都不会被删除。
"""
import json
import os
import shutil
import sys
import datetime
from collections import Counter

sys.stdout.reconfigure(encoding="utf-8")

HOME = os.path.expanduser("~")
PROJ = r"D:\workspace\projects\CreamPuffRobot"
DEST = os.path.join(PROJ, ".workbuddy-ai", "sessions-archive")

OLD = os.path.join(HOME, ".workbuddy-ai")   # 旧版 WorkBuddy（2.137 ~ 2.147）
NEW = os.path.join(HOME, ".workbuddy")      # 新版 WorkBuddy（2.156+）

PROJ_N = os.path.normcase(os.path.normpath(PROJ))
CHECK_ONLY = "--check" in sys.argv
REBUILD = "--rebuild" in sys.argv

# 输出子目录白名单：清理时只允许删这三个
OUTPUT_SUBDIRS = ["raw", "audit-log", "index"]


def norm(p):
    return os.path.normcase(os.path.normpath(p or ""))


def ts(ms):
    return datetime.datetime.fromtimestamp(ms / 1000).strftime("%Y-%m-%d %H:%M:%S") if ms else ""


def cp(src, dst):
    """复制文件或目录，返回 (文件数, 字节数)"""
    if not os.path.exists(src):
        return 0, 0
    if os.path.isdir(src):
        n = b = 0
        for r, _, fs in os.walk(src):
            for f in fs:
                sp = os.path.join(r, f)
                dp = os.path.join(dst, os.path.relpath(sp, src))
                os.makedirs(os.path.dirname(dp), exist_ok=True)
                shutil.copy2(sp, dp)
                n += 1
                b += os.path.getsize(sp)
        return n, b
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return 1, os.path.getsize(src)


# ================= 1. 会话注册表 =================
reg_recs, cwd_sids, corrupt = [], set(), []
for root, tag in [(os.path.join(OLD, "sessions"), "workbuddy-ai"),
                  (os.path.join(NEW, "sessions"), "workbuddy")]:
    if not os.path.isdir(root):
        continue
    for fn in sorted(os.listdir(root)):
        if not fn.endswith(".json"):
            continue
        fp = os.path.join(root, fn)
        raw = open(fp, "rb").read()
        try:
            j = json.loads(raw.decode("utf-8"))
        except Exception as e:
            corrupt.append({"file": fn, "src": tag, "size": len(raw), "error": str(e),
                            "allZero": len(raw) > 0 and set(raw) == {0}})
            continue
        rec = {"src": tag, "file": fn, "pid": j.get("pid"), "sessionId": j.get("sessionId"),
               "cwd": j.get("cwd"), "startedAt": j.get("startedAt"), "updatedAt": j.get("updatedAt"),
               "version": j.get("version"), "kind": j.get("kind"), "url": j.get("url")}
        reg_recs.append(rec)
        if norm(rec["cwd"]) == PROJ_N:
            cwd_sids.add(rec["sessionId"])

# ================= 2. audit 流水（兼具补漏判定） =================
audit_keep, audit_rows, audit_linemeta, cmd_sids = {}, Counter(), {}, set()
for d in [os.path.join(OLD, "audit-log"), os.path.join(NEW, "audit-log")]:
    if not os.path.isdir(d):
        continue
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".jsonl"):
            continue
        out, seen, cnt = [], set(), Counter()
        for line in open(os.path.join(d, fn), encoding="utf-8", errors="replace"):
            s = line.strip()
            if not s:
                continue
            try:
                j = json.loads(s)
            except Exception:
                continue
            sid = j.get("sessionId")
            cmd = j.get("commandPreview") or ""
            is_proj_cmd = "CreamPuffRobot" in cmd or "CreamPuffRobot" in json.dumps(
                j.get("messageParams") or {}, ensure_ascii=False)
            if is_proj_cmd and sid:
                cmd_sids.add(sid)
            if (sid in cwd_sids) or (is_proj_cmd and sid):
                if s not in seen:
                    seen.add(s)
                    out.append(s)
                    cnt[sid] += 1
        if out:
            audit_keep[fn] = out
            audit_linemeta[fn] = dict(cnt)
            for k, v in cnt.items():
                audit_rows[k] += v

whitelist = cwd_sids | cmd_sids
extra_sids = sorted(cmd_sids - cwd_sids)

print("会话注册表：%d 条记录 / %d 个 cwd 匹配 sessionId / %d 个损坏文件"
      % (len(reg_recs), len(cwd_sids), len(corrupt)))
print("命令路径补入：%d 个" % len(extra_sids))
print("白名单合计：%d 场会话，audit 流水 %d 行" % (len(whitelist), sum(audit_rows.values())))
if CHECK_ONLY:
    for s in sorted(whitelist):
        print("   %s  audit=%d" % (s, audit_rows.get(s, 0)))
    sys.exit(0)

# ================= 3. 准备输出目录 =================
if REBUILD:
    assert os.path.normcase(DEST).startswith(os.path.normcase(os.path.join(PROJ, ".workbuddy-ai"))), \
        "目标目录不在项目 .workbuddy-ai 下，拒绝清理"
    for sub in OUTPUT_SUBDIRS:
        p = os.path.join(DEST, sub)
        if os.path.isdir(p):
            shutil.rmtree(p)
            print("已清理", p)
else:
    print("增量模式：只覆盖同名文件，不删除任何内容（需要彻底重建请加 --rebuild）")

for p in ["index", "audit-log", "raw/sessions/workbuddy-ai", "raw/sessions/workbuddy",
          "raw/file-history", "raw/changes-detail", "raw/changes-index", "raw/artifact-index"]:
    os.makedirs(os.path.join(DEST, p), exist_ok=True)

stats = {"files": 0, "bytes": 0}


def acc(n, b):
    stats["files"] += n
    stats["bytes"] += b


# ---- 3a. 注册表原始 json ----
reg_copied = {"workbuddy-ai": [], "workbuddy": []}
for rec in reg_recs:
    if rec["sessionId"] not in cwd_sids:
        continue
    base = OLD if rec["src"] == "workbuddy-ai" else NEW
    acc(*cp(os.path.join(base, "sessions", rec["file"]),
            os.path.join(DEST, "raw", "sessions", rec["src"], rec["file"])))
    reg_copied[rec["src"]].append(rec["file"])
for c in corrupt:
    base = OLD if c["src"] == "workbuddy-ai" else NEW
    fn = c["file"] + (".corrupt" if c["allZero"] else ".bad")
    acc(*cp(os.path.join(base, "sessions", c["file"]),
            os.path.join(DEST, "raw", "sessions", c["src"], fn)))

# ---- 3b. audit 流水 ----
for fn, lines in audit_keep.items():
    dp = os.path.join(DEST, "audit-log", fn)
    with open(dp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    acc(1, os.path.getsize(dp))

# ---- 3c. 原始快照 ----
raw_stat = {}
for sub, kind in [("file-history", "dir"), ("changes-detail", "dir"),
                  ("changes-index", "file"), ("artifact-index", "file")]:
    sd = os.path.join(OLD, sub)
    if not os.path.isdir(sd):
        continue
    n_all = b_all = 0
    hits = []
    if kind == "dir":
        for entry in sorted(os.listdir(sd)):
            if entry not in whitelist:
                continue
            n, b = cp(os.path.join(sd, entry), os.path.join(DEST, "raw", sub, entry))
            n_all += n
            b_all += b
            hits.append(entry)
    else:
        for sid in sorted(whitelist):
            sp = os.path.join(sd, sid + ".json")
            if os.path.isfile(sp):
                n, b = cp(sp, os.path.join(DEST, "raw", sub, sid + ".json"))
                n_all += n
                b_all += b
                hits.append(sid)
    raw_stat[sub] = {"sessions": len(hits), "files": n_all, "bytes": b_all, "hit": hits}
    acc(n_all, b_all)

# ================= 4. 索引 =================
sess_index = []
for sid in sorted(whitelist, key=lambda s: min([r["startedAt"] or 0 for r in reg_recs
                                               if r["sessionId"] == s] or [0])):
    rs = [r for r in reg_recs if r["sessionId"] == sid]
    sess_index.append({
        "sessionId": sid,
        "provenance": "cwd" if sid in cwd_sids else "cmd-path",
        "pids": [r["pid"] for r in rs],
        "startedAt": ts(min([r["startedAt"] for r in rs if r["startedAt"]] or [0])),
        "updatedAt": ts(max([r["updatedAt"] for r in rs if r["updatedAt"]] or [0])),
        "version": sorted({r["version"] for r in rs if r["version"]}),
        "registryFiles": [r["file"] for r in rs],
        "auditLines": audit_rows.get(sid, 0),
        "hasFileHistory": sid in raw_stat.get("file-history", {}).get("hit", []),
        "hasChangesDetail": sid in raw_stat.get("changes-detail", {}).get("hit", []),
        "hasChangesIndex": sid in raw_stat.get("changes-index", {}).get("hit", []),
        "hasArtifactIndex": sid in raw_stat.get("artifact-index", {}).get("hit", []),
    })

now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
with open(os.path.join(DEST, "index", "sessions.json"), "w", encoding="utf-8") as fh:
    json.dump({"generatedAt": now, "projectRoot": PROJ, "sessionCount": len(sess_index),
               "sessions": sess_index, "corruptRegistryFiles": corrupt,
               "extraSessionIdsByCmdPath": extra_sids}, fh, ensure_ascii=False, indent=2)

md = ["# CreamPuffRobot 会话归档 · 会话清单\n",
      "共 %d 场会话（cwd 匹配 %d + 命令路径补入 %d）。\n" % (len(sess_index), len(cwd_sids), len(extra_sids)),
      "| # | 起始 | 结束 | sessionId | pid | 版本 | 来源 | audit 行 | 快照 |",
      "| - | --- | --- | --- | --- | --- | --- | --- | --- |"]
for i, s in enumerate(sess_index, 1):
    snaps = "".join(x for x, k in [("F", "hasFileHistory"), ("C", "hasChangesDetail"),
                                   ("I", "hasChangesIndex"), ("A", "hasArtifactIndex")] if s[k])
    md.append("| %d | %s | %s | `%s` | %s | %s | %s | %d | %s |" % (
        i, s["startedAt"], s["updatedAt"], s["sessionId"][:8], ",".join(map(str, s["pids"])),
        ",".join(s["version"]) or "-", "cwd" if s["provenance"] == "cwd" else "cmd-path",
        s["auditLines"], snaps or "-"))
md.append("\n> 快照列：F=file-history  C=changes-detail  I=changes-index  A=artifact-index\n")
open(os.path.join(DEST, "index", "sessions.md"), "w", encoding="utf-8").write("\n".join(md))

with open(os.path.join(DEST, "index", "stats.json"), "w", encoding="utf-8") as fh:
    json.dump({"generatedAt": now, "sessionCount": len(sess_index), "cwdMatched": len(cwd_sids),
               "cmdPathExtra": extra_sids, "auditLines": sum(audit_rows.values()),
               "auditFiles": audit_linemeta, "registryCopied": reg_copied,
               "rawStat": raw_stat, "corrupt": corrupt,
               "totalFiles": stats["files"], "totalBytes": stats["bytes"]}, fh,
              ensure_ascii=False, indent=2)

print("-" * 60)
for k, v in raw_stat.items():
    print("  %-16s 会话 %2d | 文件 %4d | %8.2f MB" % (k, v["sessions"], v["files"], v["bytes"] / 1024 / 1024))
print("归档完成：%d 文件，%.2f MB → %s" % (stats["files"], stats["bytes"] / 1024 / 1024, DEST))
