// tests/test_node_tree_check.cpp — P6 节点树数据模型离线自测（2026-09-20）
//
// 用法：test_node_tree_check          运行内置验证，退出码 0 = 全部通过 / 1 = 存在失败
//
// 覆盖 doc/action_tree_concurrency_plan.md **第二步**的数据模型部分：
//   · `NodeType` + `children` 两级树；`process.json` 的 `actions` → `nodes` 读取兼容与自动升级
//   · 编号规则（§7.2：方案内唯一、自增、删除不复用、**分组也占编号**）
//   · 组内校验（§6.2 按资源互斥 / §6.4 数据依赖 / §Q5 不嵌套）
//
// ⚠ 往返用例会**改写 config/process.json**（读原内容 → 写测试内容 → 校验 → 写回原内容）。
//    恢复走 RAII，异常/提前 return 也会还原；另留 `config/process.json.p6test_bak` 供人工兜底。

#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include "ConfigPaths.h"
#include "ProcessManager.h"

namespace {

int g_failed = 0;
int g_passed = 0;

void check(bool ok, const std::string& what)
{
    if (ok) { ++g_passed; return; }
    ++g_failed;
    std::fprintf(stderr, "[FAIL] %s\n", what.c_str());
}

void checkEq(int got, int want, const std::string& what)
{
    if (got == want) { ++g_passed; return; }
    ++g_failed;
    std::fprintf(stderr, "[FAIL] %s (got %d, want %d)\n", what.c_str(), got, want);
}

void checkEqStr(const QString& got, const QString& want, const std::string& what)
{
    if (got == want) { ++g_passed; return; }
    ++g_failed;
    std::fprintf(stderr, "[FAIL] %s (got '%s', want '%s')\n",
                 what.c_str(), got.toStdString().c_str(), want.toStdString().c_str());
}

// ---- process.json 原内容保护：析构时无条件写回 ----
class ProcessJsonGuard
{
public:
    ProcessJsonGuard()
    {
        m_path = ConfigPaths::processJsonPath();
        std::ifstream in(m_path.toStdString(), std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            m_original = ss.str();
            m_hadFile = true;
        }
        // 另存一份人工兜底副本
        if (m_hadFile) {
            std::ofstream bak(m_path.toStdString() + ".p6test_bak", std::ios::binary);
            bak.write(m_original.data(), static_cast<std::streamsize>(m_original.size()));
        }
    }
    ~ProcessJsonGuard() { restore(); }

    void restore()
    {
        if (!m_hadFile) return;
        std::ofstream out(m_path.toStdString(), std::ios::binary | std::ios::trunc);
        out.write(m_original.data(), static_cast<std::streamsize>(m_original.size()));
        std::remove((m_path.toStdString() + ".p6test_bak").c_str());
        m_hadFile = false;
    }
    const QString& path() const { return m_path; }

private:
    QString m_path;
    std::string m_original;
    bool m_hadFile = false;
};

void writeFile(const QString& path, const std::string& text)
{
    std::ofstream out(path.toStdString(), std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readFile(const QString& path)
{
    std::ifstream in(path.toStdString(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ActionData makeAction(int id, const QString& name, ActionType t)
{
    ActionData a;
    a.id = id;
    a.name = name;
    a.type = t;
    return a;
}

ActionData makeGroup(int id, const QString& name, const QVector<ActionData>& kids)
{
    ActionData g;
    g.id = id;
    g.name = name;
    g.nodeType = NodeType::Group;
    g.children = kids;
    return g;
}

// ============================================================ 1) 资源表与组内校验
void TestResourcesAndValidation()
{
    std::printf("-- 资源表与组内校验（§6.2 / §6.4 / §Q5）\n");

    // Move 与 VisionGuide 占**同一组轴资源**（视觉引导内部就是两段移动动作）
    checkEq(ProcessManager::resourceKeysOf(ActionType::Move).size(), 4, "Move 占 4 项轴资源");
    checkEq(ProcessManager::resourceKeysOf(ActionType::VisionGuide).size(), 4,
            "VisionGuide 占 4 项轴资源");
    check(ProcessManager::resourceKeysOf(ActionType::Move) ==
              ProcessManager::resourceKeysOf(ActionType::VisionGuide),
          "Move 与 VisionGuide 资源集合相同");
    checkEq(ProcessManager::resourceKeysOf(ActionType::Delay).size(), 0, "Delay 不占硬件资源");

    const auto move1 = makeAction(1, "移动A", ActionType::Move);
    const auto move2 = makeAction(2, "移动B", ActionType::Move);
    const auto guide = makeAction(3, "引导", ActionType::VisionGuide);
    const auto grip  = makeAction(4, "夹爪", ActionType::Gripper);
    const auto extr  = makeAction(5, "挤出", ActionType::Extrude);
    const auto delay = makeAction(6, "延时", ActionType::Delay);
    const auto vis   = makeAction(7, "识别", ActionType::Vision);

    check(ProcessManager::validateGroupChildren({ move1, grip }).isEmpty(),
          "移动 + 夹爪 → 合法（不同资源）");
    check(ProcessManager::validateGroupChildren({ grip, extr }).isEmpty(),
          "夹爪 + 挤出 → 合法（不同资源）");
    check(ProcessManager::validateGroupChildren({ delay, delay }).isEmpty(),
          "延时 + 延时 → 合法（Delay 不占资源，§6.3）");
    check(!ProcessManager::validateGroupChildren({ move1, move2 }).isEmpty(),
          "移动 + 移动 → 拒绝（争用同一组轴）");
    check(!ProcessManager::validateGroupChildren({ move1, guide }).isEmpty(),
          "移动 + 视觉引导 → 拒绝（同一组轴资源）");
    check(!ProcessManager::validateGroupChildren({ vis, vis }).isEmpty(),
          "识别 + 识别 → 拒绝（相机资源）");
    check(ProcessManager::validateGroupChildren({ move1, vis }).isEmpty(),
          "移动 + 识别 → 合法（轴 vs 相机）");
    check(ProcessManager::validateGroupChildren({ grip, extr, delay }).isEmpty(),
          "夹爪 + 挤出 + 延时 → 合法（三资源互不相交）");

    // §6.4 数据依赖：识别不能与**引用它编号**的视觉引导同组
    ActionData dependent = makeAction(8, "引导引用7", ActionType::VisionGuide);
    dependent.sourceActionId = 7;
    check(!ProcessManager::validateGroupChildren({ vis, dependent }).isEmpty(),
          "识别 + 引用它的视觉引导 → 拒绝（数据依赖）");
    ActionData unrelated = makeAction(9, "引导引用99", ActionType::VisionGuide);
    unrelated.sourceActionId = 99;
    check(ProcessManager::validateGroupChildren({ vis, unrelated }).isEmpty(),
          "识别 + 引用其它编号的视觉引导 → 合法（无数据依赖，资源也不同）");

    // §Q5 不嵌套
    const auto inner = makeGroup(10, "内层分组", { grip });
    check(!ProcessManager::validateGroupChildren({ move1, inner }).isEmpty(),
          "分组内再放分组 → 拒绝（不能嵌套）");
}

// ============================================================ 2) 编号规则
void TestIdAllocationAndNormalize()
{
    std::printf("-- 编号规则（§7.2）\n");

    // 旧数据：全 0 编号（扁平）→ 按遍历顺序补 1..N
    SchemeData s;
    s.schemeName = "T1";
    s.nodes.push_back(makeAction(0, "A", ActionType::Move));
    s.nodes.push_back(makeAction(0, "B", ActionType::Gripper));
    s.nodes.push_back(makeAction(0, "C", ActionType::Delay));
    const int fixed = ProcessManager::normalizeActionIds(s);
    checkEq(fixed, 3, "三个无编号节点全部被补发");
    checkEq(s.nodes[0].id, 1, "旧数据第 1 个 → 编号 1");
    checkEq(s.nodes[2].id, 3, "旧数据第 3 个 → 编号 3");
    checkEq(s.nextActionId, 4, "nextActionId 抬到最大编号 + 1");
    checkEq(ProcessManager::normalizeActionIds(s), 0, "再规范化一次 → 幂等（0 改动）");

    // 树形：分组也占编号，组内子节点按序跟随
    SchemeData t;
    t.schemeName = "T2";
    t.nodes.push_back(makeAction(0, "一级1", ActionType::Move));
    t.nodes.push_back(makeGroup(0, "分组1",
        { makeAction(0, "组内1", ActionType::Gripper),
          makeAction(0, "组内2", ActionType::Extrude) }));
    t.nodes.push_back(makeAction(0, "一级2", ActionType::Delay));
    ProcessManager::normalizeActionIds(t);
    checkEq(t.nodes[0].id, 1, "一级节点 1 → 1");
    checkEq(t.nodes[1].id, 2, "分组也占编号 → 2");
    checkEq(t.nodes[1].children[0].id, 3, "组内子节点 → 3");
    checkEq(t.nodes[1].children[1].id, 4, "组内子节点 → 4");
    checkEq(t.nodes[2].id, 5, "一级节点 2 → 5");
    checkEq(t.nextActionId, 6, "nextActionId = 6（含组内子节点）");

    // 删除不复用：删掉最大编号后新增，不得复用该编号
    SchemeData d;
    d.schemeName = "T3";
    d.nodes.push_back(makeAction(0, "X", ActionType::Move));
    d.nodes.push_back(makeAction(0, "Y", ActionType::Delay));
    ProcessManager::normalizeActionIds(d);
    checkEq(d.nextActionId, 3, "两个节点后 nextActionId = 3");
    const int next = ProcessManager::allocateActionId(d);   // 模拟"分配但不插入"
    checkEq(next, 3, "分配得到 3");
    checkEq(d.nextActionId, 4, "计数器推进到 4");
    checkEq(ProcessManager::allocateActionId(d), 4, "删除后新增不复用（单调递增）");

    // 计数器落后于实际最大值 → 兜底抬升（分组内的编号也要看到）
    SchemeData e;
    e.schemeName = "T4";
    e.nextActionId = 1;
    e.nodes.push_back(makeGroup(50, "分组", { makeAction(77, "深", ActionType::Delay) }));
    checkEq(ProcessManager::allocateActionId(e), 78, "计数器兜底遍历整棵树（含组内）");

    // 重复 / 非正编号 → 补发；显式不冲突的编号保留
    SchemeData f;
    f.schemeName = "T5";
    f.nodes.push_back(makeAction(5, "P", ActionType::Delay));
    f.nodes.push_back(makeAction(5, "Q", ActionType::Delay));   // 与上一个重复
    f.nodes.push_back(makeAction(-3, "R", ActionType::Delay));  // 非正
    f.nodes.push_back(makeAction(9, "S", ActionType::Delay));
    ProcessManager::normalizeActionIds(f);
    checkEq(f.nodes[0].id, 5, "显式且不冲突的编号保留");
    check(f.nodes[1].id != 5, "重复编号被改写");
    check(f.nodes[2].id > 0, "非正编号被改写为正");
    checkEq(f.nodes[3].id, 9, "显式且不冲突的编号保留（后）");
    // 全树唯一
    {
        std::vector<int> ids{ f.nodes[0].id, f.nodes[1].id, f.nodes[2].id, f.nodes[3].id };
        bool uniq = true;
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j)
                if (ids[i] == ids[j]) uniq = false;
        check(uniq, "规范化后全树编号唯一");
    }

    // 非法嵌套 → 拉平（内层分组节点丢弃，子节点上提）
    SchemeData g;
    g.schemeName = "T6";
    ActionData nested = makeGroup(1, "外层", {});
    nested.children.push_back(makeAction(2, "正常子", ActionType::Delay));
    nested.children.push_back(makeGroup(3, "内层", { makeAction(4, "深子", ActionType::Gripper) }));
    g.nodes.push_back(nested);
    ProcessManager::normalizeActionIds(g);
    checkEq(g.nodes[0].children.size(), 2, "非法嵌套被拉平（内层分组的子节点上提）");
    check(!g.nodes[0].children[1].isGroup(), "拉平后不再有分组子节点");
    checkEqStr(g.nodes[0].children[1].name, QStringLiteral("深子"), "上提的是内层分组的子节点");
}

// ============================================================ 3) 查找
void TestLookup()
{
    std::printf("-- 全树查找（findNode / findParentGroup）\n");

    SchemeData s;
    s.schemeName = "L";
    s.nodes.push_back(makeAction(1, "一级", ActionType::Move));
    s.nodes.push_back(makeGroup(2, "分组", { makeAction(3, "组内", ActionType::Gripper) }));

    check(ProcessManager::findNode(s.nodes, 1) != nullptr, "找到一级节点");
    check(ProcessManager::findNode(s.nodes, 3) != nullptr, "找到组内子节点");
    check(ProcessManager::findNode(s.nodes, 99) == nullptr, "不存在的编号返回 nullptr");
    check(ProcessManager::findNode(s.nodes, 0) == nullptr, "编号 0 返回 nullptr");
    check(ProcessManager::findParentGroup(s.nodes, 3) != nullptr, "组内子节点有父分组");
    check(ProcessManager::findParentGroup(s.nodes, 1) == nullptr, "一级节点无父分组");
    check(ProcessManager::findParentGroup(s.nodes, 2) == nullptr, "分组自身无父分组（不能嵌套）");
}

// ============================================================ 4) JSON 往返（含旧格式升级）
void TestJsonRoundTrip(const QString& path)
{
    std::printf("-- process.json 往返：旧键 actions → 新键 nodes + nodeType\n");

    // 旧格式：顶层键 `actions`，节点无 id / 无 nodeType，无 nextActionId
    const std::string legacy = R"({
  "schemes": [
    {
      "schemeName": "旧方案",
      "actions": [
        { "name": "移动到上方", "type": 0, "speedPercent": 80,
          "points": [ { "name": "p1", "posture": "elbow_up",
                        "coord": { "x": 10.0, "y": 20.0, "z": 30.0, "r": 5.0 } } ] },
        { "name": "打开夹爪", "type": 4, "isGripperOpen": true, "gripperTarget": -3.0 },
        { "name": "延时", "type": 3, "delayMs": 250 }
      ]
    }
  ]
})";
    writeFile(path, legacy);

    auto& pm = ProcessManager::instance();
    pm.load(true);

    checkEq(pm.schemes().size(), 1, "载入 1 个方案");
    if (pm.schemes().isEmpty()) return;

    const SchemeData& s = pm.schemes()[0];
    checkEqStr(s.schemeName, QStringLiteral("旧方案"), "方案名保留");
    checkEq(s.nodes.size(), 3, "旧 actions 数组按顺序读入 nodes");
    checkEq(s.nodes[0].id, 1, "旧数据补发编号 1");
    checkEq(s.nodes[1].id, 2, "旧数据补发编号 2");
    checkEq(s.nodes[2].id, 3, "旧数据补发编号 3");
    check(!s.nodes[0].isGroup(), "旧数据默认视为动作节点");
    checkEq(static_cast<int>(s.nodes[0].type), static_cast<int>(ActionType::Move), "动作类型保留");
    checkEq(s.nodes[0].points.size(), 1, "点位保留");
    checkEq(s.nodes[1].isGripperOpen ? 1 : 0, 1, "夹爪方向保留");
    checkEq(s.nodes[2].delayMs, 250, "延时值保留");
    checkEq(s.nextActionId, 4, "nextActionId 补为 4");

    // 加入一个分组 + 组内子节点，落盘后重新载入，验证树形与 nodeType 持久化
    {
        auto& scheme = pm.schemes()[0];
        ActionData grp;
        grp.nodeType = NodeType::Group;
        grp.name = QStringLiteral("并发组");
        grp.id = ProcessManager::allocateActionId(scheme);
        ActionData child = makeAction(ProcessManager::allocateActionId(scheme),
                                      QStringLiteral("组内夹爪"), ActionType::Gripper);
        grp.children.push_back(child);
        scheme.nodes.insert(1, grp);
    }
    pm.save();

    const std::string dumped = readFile(path);
    check(dumped.find("\"nodes\"") != std::string::npos, "落盘使用新键 nodes");
    check(dumped.find("\"nodeType\"") != std::string::npos, "落盘写入 nodeType");
    check(dumped.find("\"group\"") != std::string::npos, "分组以 nodeType=group 落盘");
    check(dumped.find("\"children\"") != std::string::npos, "分组子节点写入 children");

    pm.load(true);
    checkEq(pm.schemes().size(), 1, "重新载入 1 个方案");
    if (pm.schemes().isEmpty()) return;
    const SchemeData& s2 = pm.schemes()[0];
    checkEq(s2.nodes.size(), 4, "树形往返：一级节点 4 个");
    check(s2.nodes[1].isGroup(), "第 2 个一级节点是分组");
    checkEq(s2.nodes[1].children.size(), 1, "分组保留 1 个子节点");
    checkEqStr(s2.nodes[1].children[0].name, QStringLiteral("组内夹爪"), "组内子节点名称往返一致");
    checkEq(s2.nodes[1].children[0].id, 5, "组内子节点编号往返一致");
    checkEq(s2.nextActionId, 6, "nextActionId 往返一致");

    // 再存一次 → 内容稳定（幂等：不会每次保存都改编号）
    const std::string first = readFile(path);
    pm.save();
    check(readFile(path) == first, "再次保存内容不变（编号分配幂等）");
}

} // namespace

int main()
{
#ifdef _WIN32
    // 控制台输出代码页置 UTF-8：本工程全局 /utf-8（CMakeLists.txt:45）→ 中文字面量按
    // UTF-8 字节输出，而 cmd.exe 默认代码页是 GBK(936)，直接运行会中文乱码。
    SetConsoleOutputCP(CP_UTF8);
#endif
    const ProcessJsonGuard guard;   // 无条件恢复 config/process.json

    TestResourcesAndValidation();
    TestIdAllocationAndNormalize();
    TestLookup();
    TestJsonRoundTrip(guard.path());

    std::printf("\n%s  passed=%d failed=%d\n",
                g_failed == 0 ? "[PASS] 全部通过" : "[FAIL] 存在失败", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
