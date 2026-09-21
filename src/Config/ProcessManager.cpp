#include "ProcessManager.h"

#include "ConfigPaths.h"

#include <QFile>
#include <QSaveFile>
#include <QSet>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <random>
#include <utility>

namespace {

// 分组内嵌分组的**拉平**（§Q5：分组不能嵌套，两级写死）。
// 递归把内层分组的子节点上提到同一层，内层分组节点本身丢弃。
// 这是加载期兜底修复——正常数据不可能产生嵌套（UI 防呆 + validateGroupChildren 两处拦）。
void flattenInto(QVector<ActionData>& dst, const QVector<ActionData>& src)
{
    for (const auto& c : src) {
        if (c.isGroup())
            flattenInto(dst, c.children);     // 上提一层，丢弃该分组节点
        else
            dst.push_back(c);
    }
}

// 按「一级节点顺序 + 分组内子节点顺序」收集整棵树的节点指针。
// 编号分配与计数器兜底共用同一遍历顺序，保证两处一致。
void collectOrdered(QVector<ActionData*>& out, QVector<ActionData>& nodes)
{
    for (auto& n : nodes) {
        out.push_back(&n);
        if (n.isGroup())
            for (auto& c : n.children) out.push_back(&c);
    }
}

void collectOrdered(QVector<const ActionData*>& out, const QVector<ActionData>& nodes)
{
    for (const auto& n : nodes) {
        out.push_back(&n);
        if (n.isGroup())
            for (const auto& c : n.children) out.push_back(&c);
    }
}

// ---- process.json 节点读写（P6：树形 + nodeType）----

ActionData readNode(const nlohmann::json& nj)
{
    ActionData a;
    // 旧文件无 id → 0，由 normalizeActionIds 按遍历顺序补发
    a.id = nj.value("id", 0);
    a.name = QString::fromStdString(nj.value("name", ""));
    // 旧文件无 nodeType → 一律视为动作（§7.3 读取兼容，不需用户手工迁移）
    a.nodeType = (nj.value("nodeType", std::string("action")) == "group")
                     ? NodeType::Group : NodeType::Action;
    a.type = static_cast<ActionType>(nj.value("type", 0));
    a.speedPercent = nj.value("speedPercent", 100);
    a.visionType = QString::fromStdString(nj.value("visionType", ""));
    a.exposure = nj.value("exposure", 0.0);
    a.templateName = QString::fromStdString(nj.value("templateName", ""));
    a.threshold = nj.value("threshold", 0.0);
    a.extrudeAmount = nj.value("extrudeAmount", 0.0);
    a.extrudeSpeed = nj.value("extrudeSpeed", 0.0);
    a.suckBackAmount = nj.value("suckBackAmount", 0.0);
    a.suckBackSpeed = nj.value("suckBackSpeed", 0.0);
    a.delayMs = nj.value("delayMs", 0);
    a.isGripperOpen = nj.value("isGripperOpen", false);
    // 旧 json 无 gripperTarget：按方向给默认（张开=-3.00 松开3mm / 闭合=0.00 夹紧）
    a.gripperTarget = nj.value("gripperTarget", a.isGripperOpen ? -3.0 : 0.0);
    // 视觉引导（P5）：旧 json 无这三个键 → 0 / 0.0 / 179.9（默认值即"未设置引用"）
    a.sourceActionId = nj.value("sourceActionId", 0);
    a.offsetZ = nj.value("offsetZ", 0.0);
    a.guideR = nj.value("guideR", 179.9);

    if (nj.contains("points") && nj["points"].is_array()) {
        for (const auto& pj : nj["points"]) {
            PointData p;
            p.name = QString::fromStdString(pj.value("name", ""));
            if (pj.contains("coord") && pj["coord"].is_object()) {
                const auto& cj = pj["coord"];
                p.x = cj.value("x", 0.0);
                p.y = cj.value("y", 0.0);
                p.z = cj.value("z", 0.0);
                p.r = cj.value("r", 0.0);
            }
            p.posture = QString::fromStdString(pj.value("posture", "elbow_up"));
            // joints 语义校验（FK 一致性）在执行层 SequenceWorker 做，
            // 此处纯读：有字段即记录，无字段/缺项 → hasJoints=false 走 IK
            if (pj.contains("joints") && pj["joints"].is_object()) {
                const auto& jj = pj["joints"];
                if (jj.contains("j1") && jj.contains("j2") &&
                    jj.contains("j3") && jj.contains("j4")) {
                    p.hasJoints = true;
                    p.j1 = jj.value("j1", 0.0);
                    p.j2 = jj.value("j2", 0.0);
                    p.j3 = jj.value("j3", 0.0);
                    p.j4 = jj.value("j4", 0.0);
                }
            }
            a.points.push_back(p);
        }
    }

    if (nj.contains("children") && nj["children"].is_array())
        for (const auto& cj : nj["children"]) a.children.push_back(readNode(cj));

    return a;
}

nlohmann::json writeNode(const ActionData& a)
{
    nlohmann::json aj;
    aj["id"] = a.id;                       // 方案内唯一节点编号（P2；分组同样占编号）
    aj["name"] = a.name.toStdString();
    aj["nodeType"] = a.isGroup() ? "group" : "action";

    if (a.isGroup()) {
        aj["children"] = nlohmann::json::array();
        for (const auto& c : a.children) aj["children"].push_back(writeNode(c));
        return aj;                         // 分组不带动作参数
    }

    aj["type"] = static_cast<int>(a.type);
    aj["speedPercent"] = a.speedPercent;
    aj["visionType"] = a.visionType.toStdString();
    aj["exposure"] = a.exposure;
    aj["templateName"] = a.templateName.toStdString();
    aj["threshold"] = a.threshold;
    aj["extrudeAmount"] = a.extrudeAmount;
    aj["extrudeSpeed"] = a.extrudeSpeed;
    aj["suckBackAmount"] = a.suckBackAmount;
    aj["suckBackSpeed"] = a.suckBackSpeed;
    aj["delayMs"] = a.delayMs;
    aj["isGripperOpen"] = a.isGripperOpen;
    aj["gripperTarget"] = a.gripperTarget;
    // 视觉引导（P5）：引用编号 + 上方偏移 + 轴4 固定 R 角。
    // 只写"动作定义"，**识别坐标绝不写进来**（决策 5：运行结果不污染方案定义）。
    aj["sourceActionId"] = a.sourceActionId;
    aj["offsetZ"] = a.offsetZ;
    aj["guideR"] = a.guideR;
    aj["points"] = nlohmann::json::array();

    for (const auto& pt : a.points) {
        nlohmann::json pj;
        pj["name"] = pt.name.toStdString();
        pj["posture"] = pt.posture.toStdString();
        pj["coord"]["x"] = pt.x;
        pj["coord"]["y"] = pt.y;
        pj["coord"]["z"] = pt.z;
        pj["coord"]["r"] = pt.r;
        // 仅示教记录有效时写 joints（缺失字段 = 无记录，执行回退 IK）。
        // 不写全 0 占位：0 是合法关节角，无法与"作废/无记录"区分
        if (pt.hasJoints) {
            pj["joints"]["j1"] = pt.j1;
            pj["joints"]["j2"] = pt.j2;
            pj["joints"]["j3"] = pt.j3;
            pj["joints"]["j4"] = pt.j4;
        }
        aj["points"].push_back(pj);
    }
    return aj;
}

} // namespace

// ---------------------------------------------------------------------------
ProcessManager& ProcessManager::instance()
{
    static ProcessManager inst;
    return inst;
}

// ---------------------------------------------------------------------------
QString ProcessManager::actionTypeName(ActionType t)
{
    switch (t) {
    case ActionType::Move:        return QStringLiteral("移动");
    case ActionType::Vision:      return QStringLiteral("识别");
    case ActionType::Extrude:     return QStringLiteral("挤压");
    case ActionType::Delay:       return QStringLiteral("延时");
    case ActionType::Gripper:     return QStringLiteral("夹爪");
    case ActionType::VisionGuide: return QStringLiteral("视觉引导");
    }
    return QStringLiteral("未知");
}

// ---------------------------------------------------------------------------
int ProcessManager::allocateActionId(SchemeData& scheme)
{
    // 计数器兜底：旧内存数据（或外部直接构造的 SchemeData）可能 nextActionId 落后于实际最大值。
    // 遍历**整棵树**——分组同样占编号（§7.2）。
    QVector<const ActionData*> ordered;
    collectOrdered(ordered, scheme.nodes);
    for (auto* p : ordered)
        if (p->id >= scheme.nextActionId) scheme.nextActionId = p->id + 1;
    if (scheme.nextActionId < 1) scheme.nextActionId = 1;
    return scheme.nextActionId++;
}

int ProcessManager::normalizeActionIds(SchemeData& scheme)
{
    int changed = 0;

    // ① 拉平非法的「分组内嵌分组」（§Q5 写死两级）。正常数据不会命中。
    for (auto& n : scheme.nodes) {
        if (!n.isGroup() || n.children.isEmpty()) continue;
        bool hasGroupChild = false;
        for (const auto& c : n.children)
            if (c.isGroup()) { hasGroupChild = true; break; }
        if (!hasGroupChild) continue;

        QVector<ActionData> flat;
        flattenInto(flat, n.children);
        SPDLOG_WARN("[Process] Group '{}' had nested group(s): flattened {} child node(s) -> {}",
                    n.name.toStdString(), n.children.size(), flat.size());
        changed += n.children.size() - flat.size();
        n.children = std::move(flat);
    }

    // ② 编号：按「一级节点顺序 + 分组内子节点顺序」遍历，保证全树唯一且为正
    QVector<ActionData*> ordered;
    collectOrdered(ordered, scheme.nodes);

    int maxId = 0;
    for (auto* p : ordered)
        if (p->id > maxId) maxId = p->id;

    // 计数器不得低于「方案内最大编号 + 1」，否则新节点会撞上已有编号
    int next = scheme.nextActionId > maxId + 1 ? scheme.nextActionId : maxId + 1;
    if (next < 1) next = 1;

    QSet<int> claimed;
    for (auto* p : ordered) {
        if (p->id > 0 && !claimed.contains(p->id)) {
            claimed.insert(p->id);           // 显式且不冲突 → 保留（避免每次加载都改编号）
            continue;
        }
        p->id = next++;
        claimed.insert(p->id);
        ++changed;
    }
    scheme.nextActionId = next;
    return changed;
}

ActionData* ProcessManager::findNode(QVector<ActionData>& nodes, int id)
{
    if (id <= 0) return nullptr;
    for (auto& n : nodes) {
        if (n.id == id) return &n;
        if (n.isGroup())
            for (auto& c : n.children)
                if (c.id == id) return &c;
    }
    return nullptr;
}

const ActionData* ProcessManager::findNode(const QVector<ActionData>& nodes, int id)
{
    if (id <= 0) return nullptr;
    for (const auto& n : nodes) {
        if (n.id == id) return &n;
        if (n.isGroup())
            for (const auto& c : n.children)
                if (c.id == id) return &c;
    }
    return nullptr;
}

ActionData* ProcessManager::findParentGroup(QVector<ActionData>& nodes, int id)
{
    if (id <= 0) return nullptr;
    for (auto& n : nodes) {
        if (!n.isGroup()) continue;
        for (auto& c : n.children)
            if (c.id == id) return &n;
    }
    return nullptr;
}

const ActionData* ProcessManager::findParentGroup(const QVector<ActionData>& nodes, int id)
{
    if (id <= 0) return nullptr;
    for (const auto& n : nodes) {
        if (!n.isGroup()) continue;
        for (const auto& c : n.children)
            if (c.id == id) return &n;
    }
    return nullptr;
}

QStringList ProcessManager::resourceKeysOf(ActionType t)
{
    switch (t) {
    case ActionType::Move:
    case ActionType::VisionGuide:
        // ⚠ 视觉引导内部就是两段移动动作（P5）——与「移动」占**同一组轴资源**，
        // 同组内必须互斥。否则两个绝对目标会互相打架（§6.1）。
        return { QStringLiteral("Axis_J1"), QStringLiteral("Axis_J2"),
                 QStringLiteral("Axis_Z"),  QStringLiteral("Axis_R") };
    case ActionType::Gripper: return { QStringLiteral("Axis_Gripper") };
    case ActionType::Extrude: return { QStringLiteral("Axis_Extruder") };
    case ActionType::Vision:  return { QStringLiteral("Camera") };
    case ActionType::Delay:   return {};   // §6.3：不占硬件资源，同组多个等价于取最大值
    }
    return {};
}

QString ProcessManager::validateGroupChildren(const QVector<ActionData>& children)
{
    // ① 不嵌套（§Q5）：分组内只允许动作节点
    for (const auto& c : children)
        if (c.isGroup())
            return QStringLiteral("分组不能嵌套：「%1」本身是分组。请先把它的子动作移出。").arg(c.name);

    // ② 数据依赖（§6.4）：识别动作不能与「引用它编号的视觉引导」同组——
    //    并发时移动的目标尚未算出，逻辑上自相矛盾
    for (const auto& g : children) {
        if (g.type != ActionType::VisionGuide || g.sourceActionId <= 0) continue;
        for (const auto& v : children) {
            if (v.type == ActionType::Vision && v.id == g.sourceActionId)
                return QStringLiteral("「%1」引用了同组的识别动作「%2」：并发执行时移动目标尚未算出"
                                      "（数据依赖冲突）。请把两者分到不同分组或改为顺序执行。")
                    .arg(g.name, v.name);
        }
    }

    // ③ 资源互斥（§6.2：按资源，不按类型）：两两求交，命中即拒
    for (int i = 0; i < children.size(); ++i) {
        const QStringList ri = resourceKeysOf(children[i].type);
        if (ri.isEmpty()) continue;
        for (int j = i + 1; j < children.size(); ++j) {
            const QStringList rj = resourceKeysOf(children[j].type);
            for (const auto& k : rj) {
                if (ri.contains(k))
                    return QStringLiteral("「%1」与「%2」并发时争用同一资源（%3），无法同时执行。")
                        .arg(children[i].name, children[j].name, k);
            }
        }
    }
    return QString();
}

QString ProcessManager::generateUniqueSchemeName(const QVector<SchemeData>& existing)
{
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(100, 999);
    QString name;
    do {
        name = QStringLiteral("方案_%1").arg(dis(gen), 3, 10, QChar('0'));
    } while (std::any_of(existing.begin(), existing.end(),
        [&](const SchemeData& s) { return s.schemeName == name; }));
    return name;
}

// ---------------------------------------------------------------------------
void ProcessManager::load(bool force)
{
    // 幂等：本函数有 MainWindow 与 ProcessPage 两个构造期调用点，重复读盘会覆盖
    // 内存中尚未保存的修改（见 doc/config_ownership_plan.md §3.5）。
    if (m_loaded && !force)
        return;

    m_schemes.clear();
    m_loaded = true;

    const QString path = ConfigPaths::processJsonPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        SPDLOG_INFO("[Process] No process.json found, generating default test scheme");

        SchemeData def;
        def.schemeName = QStringLiteral("方案_100");

        ActionData a1; a1.name = QStringLiteral("移动到：安全上方点"); a1.type = ActionType::Move;
        PointData p1; p1.name = QStringLiteral("safe_top"); p1.x = 0; p1.y = 200; p1.z = 50; p1.r = 0; p1.posture = QStringLiteral("elbow_up");
        a1.points.push_back(p1);
        def.nodes.push_back(a1);

        ActionData a2; a2.name = QStringLiteral("打开夹爪"); a2.type = ActionType::Gripper; a2.isGripperOpen = true; a2.gripperTarget = -3.0;
        def.nodes.push_back(a2);

        ActionData a3; a3.name = QStringLiteral("等待视觉结果"); a3.type = ActionType::Vision;
        a3.visionType = QStringLiteral("CCD"); a3.exposure = 30.0; a3.templateName = QStringLiteral("template_a"); a3.threshold = 0.85;
        def.nodes.push_back(a3);

        ActionData a4; a4.name = QStringLiteral("直线插补：插入灌装口"); a4.type = ActionType::Move;
        PointData p2; p2.name = QStringLiteral("fill_start"); p2.x = 85; p2.y = 92; p2.z = 22; p2.r = 2; p2.posture = QStringLiteral("elbow_up");
        PointData p3; p3.name = QStringLiteral("fill_end");   p3.x = 85; p3.y = 92; p3.z = 10; p3.r = 2; p3.posture = QStringLiteral("elbow_down");
        a4.points.push_back(p2); a4.points.push_back(p3);
        def.nodes.push_back(a4);

        ActionData a5; a5.name = QStringLiteral("挤出奶油"); a5.type = ActionType::Extrude;
        a5.extrudeAmount = 5.0; a5.extrudeSpeed = 2.0; a5.suckBackAmount = 1.0; a5.suckBackSpeed = 3.0;
        def.nodes.push_back(a5);

        ActionData a6; a6.name = QStringLiteral("关闭夹爪"); a6.type = ActionType::Gripper; a6.isGripperOpen = false; a6.gripperTarget = 0.0;
        def.nodes.push_back(a6);

        ActionData a7; a7.name = QStringLiteral("移动到放置点"); a7.type = ActionType::Move;
        PointData p4; p4.name = QStringLiteral("place"); p4.x = 150; p4.y = 0; p4.z = 30; p4.r = 0; p4.posture = QStringLiteral("elbow_up");
        a7.points.push_back(p4);
        def.nodes.push_back(a7);

        // 出厂方案同样走编号规范化（补 1..N），否则首次新增动作会拿到 0/重复编号
        normalizeActionIds(def);
        m_schemes.push_back(def);
        return;
    }

    try {
        auto j = nlohmann::json::parse(file.readAll().toStdString());
        m_schemes.clear();

        if (j.contains("schemes") && j["schemes"].is_array()) {
            for (const auto& sj : j["schemes"]) {
                SchemeData s;
                s.schemeName = QString::fromStdString(sj.value("schemeName", ""));
                // 旧文件无 nextActionId → 默认 1，随后由 normalizeActionIds 抬到不冲突的位置
                s.nextActionId = sj.value("nextActionId", 1);

                // §7.3 读取兼容：新键 `nodes` 优先，旧键 `actions` 照收（首次保存写回 `nodes`）。
                const nlohmann::json* arr = nullptr;
                if (sj.contains("nodes") && sj["nodes"].is_array())        arr = &sj["nodes"];
                else if (sj.contains("actions") && sj["actions"].is_array()) arr = &sj["actions"];
                if (arr)
                    for (const auto& nj : *arr) s.nodes.push_back(readNode(nj));

                // 读取兼容与自动升级（P2 / P6）：旧文件无 id → 按遍历顺序补 1..N；编号缺失/非正/
                // 重复、以及非法的分组嵌套，都在此统一修复。**不要求用户手工迁移**。
                const int fixed = normalizeActionIds(s);
                if (fixed > 0)
                    SPDLOG_INFO("[Process] Scheme '{}': normalized {} of {} node(s), nextActionId={}",
                                s.schemeName.toStdString(), fixed, s.nodes.size(), s.nextActionId);
                m_schemes.push_back(s);
            }
        }

        SPDLOG_INFO("[Process] Loaded {} schemes from {}", m_schemes.size(), path.toStdString());
    } catch (const std::exception& e) {
        SPDLOG_INFO("[Process] Parse error: {}", e.what());
        m_schemes.clear();
    }
}

void ProcessManager::save()
{
    nlohmann::json j;
    j["schemes"] = nlohmann::json::array();

    for (auto& scheme : m_schemes) {
        // 落盘前兜底规范化：保证写出的文件里不存在 0 / 重复编号（幂等，正常情况零改动）
        normalizeActionIds(scheme);

        nlohmann::json sj;
        sj["schemeName"] = scheme.schemeName.toStdString();
        // 编号计数器必须落盘——它是「删除不复用编号」的唯一依据（§四）
        sj["nextActionId"] = scheme.nextActionId;
        // P6：键名 `actions` → `nodes`（§7.3 第二步必须完成的一次性改名）
        sj["nodes"] = nlohmann::json::array();
        for (const auto& node : scheme.nodes)
            sj["nodes"].push_back(writeNode(node));
        j["schemes"].push_back(sj);
    }

    // 原子写（QSaveFile：写临时文件，commit() 时原子替换目标）。
    // 原实现用 std::ofstream 直接截断目标文件后写入，中途崩溃/掉电会丢掉**全部方案**——
    // 与 ConfigManager::save() 同一缺陷（doc/config_ownership_plan.md §七 只点了后者）。
    const QString target = ConfigPaths::processJsonPath();
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        SPDLOG_WARN("[Process] Cannot write: {}", target.toStdString());
        return;
    }
    const std::string dumped = j.dump(4);
    if (file.write(dumped.data(), static_cast<qint64>(dumped.size())) != static_cast<qint64>(dumped.size())) {
        SPDLOG_WARN("[Process] Short write, aborting: {}", target.toStdString());
        file.cancelWriting();
        return;
    }
    if (!file.commit()) {
        SPDLOG_WARN("[Process] Commit failed, original kept: {}", target.toStdString());
        return;
    }
    SPDLOG_INFO("[Process] Saved schemes to {}", target.toStdString());
}
