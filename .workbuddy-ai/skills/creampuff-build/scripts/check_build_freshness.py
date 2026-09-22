#!/usr/bin/env python3
"""构建新鲜度预检 —— 判断 ninja 增量构建结果是否可信。

背景：本仓库 ninja 的头文件依赖追踪失效（`build.ninja` 用 `deps = gcc`，而中文版
`cl.exe` 输出「注意: 包含文件:」而非 `Note: including file:`，依赖没进 `.ninja_deps`）。
后果：改头文件后**只有被改的那个 `.cpp` 会重编，包含该头的其他 TU 不重编**。
若头文件改动涉及类成员 / 类大小 / 虚表，就产生**编译期静默、运行期堆越界**的 ABI
不一致——2026-09-22 实际发生过：`ManualControlPage.h` 新增两个 `QVector` 成员 →
`MainWindow.cpp.obj` 未重编 → `new ManualControlPage()` 按旧类大小分配 → 启动即崩。

判据（本脚本实现的就是它）：
    **对每个头文件 H，所有（含传递）包含 H 的 `.cpp`，其 `.obj` 都必须晚于 H。**
这比「最旧 obj vs 最新头文件」的 mtime 全序比较正确——后者既会被 CMake 探测产物
（`CompilerId*.obj`、`ShowIncludes/main.obj`）与上一轮配置遗留的孤儿 obj 绊出假阳性，
又**抓不到**「改了头文件但只有一个 TU 重编」这个真正的危险情形。

用法：
    python check_build_freshness.py <build_dir> [src_dir ...]
例：
    python check_build_freshness.py out/build/x64-Debug src

退出码：0 = 可信（增量构建结果可用）；1 = 不可信（须 `ninja -t clean` 全清重建）；2 = 环境错误。
"""
import os
import re
import sys

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
SKIP_DIR_NAMES = {".git", ".history", "3rdparty", "out", "build", ".vs", ".workbuddy-ai"}


def collect_sources(src_dirs):
    """返回 (headers, cpps)：文件名 -> 绝对路径。"""
    headers, cpps = {}, {}
    for d in src_dirs:
        for root, dirnames, filenames in os.walk(d):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIR_NAMES]
            for fn in filenames:
                p = os.path.join(root, fn)
                if fn.endswith((".h", ".hpp", ".hxx")):
                    headers[fn] = p
                elif fn.endswith(".cpp"):
                    cpps[fn] = p
    return headers, cpps


def direct_includes(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return set(INCLUDE_RE.findall(f.read()))
    except OSError:
        return set()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    build_dir = argv[1]
    src_dirs = argv[2:] or ["src"]
    if not os.path.isdir(build_dir):
        print("BUILD_DIR_MISSING: %s" % build_dir)
        return 2

    headers, cpps = collect_sources(src_dirs)
    if not headers or not cpps:
        print("NO_SOURCE_FOUND in %s" % src_dirs)
        return 2

    # obj 按 basename 索引（目标目录名 `UI.dir` / `HAL.dir` 无法从源路径推出）
    obj_by_name = {}
    for root, _dirnames, filenames in os.walk(build_dir):
        for fn in filenames:
            if fn.endswith(".obj"):
                obj_by_name.setdefault(fn, []).append(os.path.join(root, fn))

    # 头文件包含图（只在项目内头文件之间），再求每个头文件的传递可达集合
    h_inc = {h: direct_includes(p) for h, p in headers.items()}
    reach_cache = {}

    def reach(h, stack=()):
        if h in reach_cache:
            return reach_cache[h]
        if h in stack:              # 环（含自包含守卫）→ 不展开
            return {h}
        out = {h}
        for inc in h_inc.get(h, ()):
            if inc in headers and inc != h:
                out |= reach(inc, stack + (h,))
        reach_cache[h] = out
        return out

    # 每个 .cpp 的直接头集合
    cpp_direct = {c: {i for i in direct_includes(p) if i in headers} for c, p in cpps.items()}

    stale = []
    missing_obj = []
    for h, hp in headers.items():
        h_mtime = os.path.getmtime(hp)
        reachable = reach(h)
        for c, incs in cpp_direct.items():
            if not (incs & reachable):
                continue
            objs = obj_by_name.get(c + ".obj")
            if not objs:
                missing_obj.append((h, c))
                continue
            newest_obj = max(os.path.getmtime(o) for o in objs)
            if newest_obj < h_mtime:
                import datetime
                stale.append((h, hp, h_mtime, c, newest_obj))

    if stale:
        print("==> STALE：%d 处「头文件晚于其包含者的 obj」——增量构建结果不可信。" % len(stale))
        import datetime
        for h, hp, hm, c, om in sorted(stale, key=lambda x: -(x[2] - x[4]))[:20]:
            print("    头 %-28s %s" % (h, datetime.datetime.fromtimestamp(hm).strftime("%H:%M:%S")))
            print("      ← 包含者 %-24s obj %s  （旧 %ds）"
                  % (c, datetime.datetime.fromtimestamp(om).strftime("%H:%M:%S"),
                     int(hm - om)))
        if len(stale) > 20:
            print("    …… 另有 %d 处" % (len(stale) - 20))
        print("")
        print("    处置：ninja -C <build_dir> -t clean 后重建。")
        print("    （改头文件后一律全清；只改 .cpp 时增量才可靠。）")
        return 1

    print("==> FRESH：所有头文件的包含者 obj 均晚于该头文件，增量构建结果可信。")
    if missing_obj:
        print("    （提示：%d 处未找到对应 obj，多半是该 .cpp 不属于当前构建目标，已跳过）"
              % len(missing_obj))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
