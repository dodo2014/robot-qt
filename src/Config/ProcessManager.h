#pragma once

#include <QVector>
#include <QString>
#include <QStringList>

// ⚠ 枚举值**只能追加在末尾**：`type` 在 `process.json` 里是**整数持久化**的（现有 0~4），
// 插在中间会让所有已保存方案的动作类型静默错位（详见 doc/vision_guide_action_plan.md §三）。
// 新增类型须同步三处：本枚举 / ProcessManager.cpp 的 actionTypeName / ProcessPage.cpp 的
// 类型下拉项（**下拉框索引直接就是枚举值**，两处硬编码列表都要改）。
enum class ActionType { Move = 0, Vision, Extrude, Delay, Gripper, VisionGuide };

// 节点类型（P6，doc/action_tree_concurrency_plan.md §7.1）。
// **刻意不进 `ActionType` 枚举**：① `type` 是整数持久化的，插值会让既有方案错位；
// ② `VisionGuide` 已占 5，`Group` 再取 5 会冲突；③ 类型下拉框的索引**直接就是枚举值**，
// 把 `Group` 塞进去会立即污染类型映射。故用独立字段表达，与动作类型正交。
// `process.json` 里以字符串 `"action"` / `"group"` 持久化（人可读，且与整数 `type` 不混淆）。
enum class NodeType { Action = 0, Group = 1 };

struct PointData {
    QString name;
    double x = 0.0, y = 0.0, z = 0.0, r = 0.0;
    QString posture;
    // 示教关节角记录：j1/j2=逻辑关节角（GetPosition 值，MoveAbs 直接可用）；
    // j3=Z 电机坐标（回零后=0、往下为负，与手动页 Z 轴 Go 同坐标系，≠coord.z 末端高度，
    // 两者差固定偏移 Z0−h1−tcpDown）；j4=R 角（与 coord.r 同值冗余记录）。
    // hasJoints=false = 无有效记录（坐标被手改/手工点/旧数据），执行回退 IK。
    bool hasJoints = false;
    double j1 = 0.0, j2 = 0.0, j3 = 0.0, j4 = 0.0;
};

// 方案内的**树节点**（P6 前是纯动作；分组与动作共用本结构，由 `nodeType` 区分）。
struct ActionData {
    // 方案内唯一的节点编号（正整数）。规则见 doc/vision_guide_action_plan.md §四：
    // 新增时由 ProcessManager::allocateActionId 分配；插入 / 删除 / 上移 / 下移
    // **都不重排、不回收**（重排会让 sourceActionId 引用静默错位）。
    // 0 = 未分配（旧数据），加载时由 normalizeActionIds 按遍历顺序迁移为 1..N。
    // **分组同样占用编号**（§7.2：UI 要显示、也要能被引用定位）。
    int id = 0;
    QString name;
    // ---- P6（doc/action_tree_concurrency_plan.md §七）----
    NodeType nodeType = NodeType::Action;
    // 分组节点的子节点。**不允许嵌套**（§Q5，写死两级）：子节点一律为 `NodeType::Action`。
    // 双保险：加载期 `normalizeActionIds` 会把非法嵌套拉平，UI 侧在新建/编辑时拦截。
    // 仅 `nodeType == Group` 时非空；动作节点恒为空。
    QVector<ActionData> children;

    ActionType type = ActionType::Move;   // 仅 nodeType == Action 有意义
    int speedPercent = 100;
    QVector<PointData> points;
    QString visionType;
    double exposure = 0.0;
    QString templateName;
    double threshold = 0.0;
    double extrudeAmount = 0.0;
    double extrudeSpeed = 0.0;
    double suckBackAmount = 0.0;
    double suckBackSpeed = 0.0;
    int delayMs = 0;
    bool isGripperOpen = false;
    // 夹爪动作行程 = 轴5（LogicalAxis::Gripper）绝对目标坐标，单位 mm。
    // 物理语义（用户 2026-09-02 确认）：0 = 夹紧，负值 = 松开，受轴5 软限位约束（-5.00 ~ 0.00）。
    double gripperTarget = 0.0;

    // ---- 视觉引导（P5，doc/vision_guide_action_plan.md §5.1）----
    // 引用的**识别动作编号**（必填）。**不允许跨方案引用**（决策 2）——编号只在同一
    // SchemeData 内解析。0 = 未设置：编辑期拒绝保存，执行期报错且**不产生任何运动**。
    int sourceActionId = 0;
    // 「目标上方」相对目标的 Z 偏移，单位 mm，**正值向上**。
    // ⚠ 按**末端高度（Pose.z）**算，与 `PointData.j3`（Z 电机坐标）严格区分、不可混用（决策 3）。
    double offsetZ = 0.0;
    // 构造两个路径点的**轴 4(R) 固定值**（决策 5 / §七）。
    // **不使用 `PuffResult.yaw`**——yaw 是目标在相机图像平面内的偏航角，本项目目标为泡芙，
    // yaw 无工艺意义，不参与坐标构造。
    // 默认 179.9 而非 180：R 物理停在 180° 附近时读数会在 {179.9, 180, -179.9} 之间跳动，
    // 179.9 离 ±180 回绕奇点稍远，读数更稳（§七）。
    double guideR = 179.9;

    bool isGroup() const { return nodeType == NodeType::Group; }
};

struct SchemeData {
    QString schemeName;
    // 一级节点列表。P6 由 `actions` 改名而来（§7.3 第二步必须完成的一次性改名）；
    // 旧文件键 `actions` 仍可读（读取兼容），首次保存写回 `nodes`。
    QVector<ActionData> nodes;
    // 下一个可用节点编号（**单调递增，删除节点后不回退**）。
    // 这是「删除不复用编号」唯一能成立的保证：若按 max(id)+1 现算，删掉最大编号后
    // 新节点会复用该编号，而 sourceActionId 引用（P5）会静默指向新节点。
    // 旧文件无此键时由 normalizeActionIds 兜底为「方案内最大编号 + 1」。
    int nextActionId = 1;
};

class ProcessManager
{
public:
    static ProcessManager& instance();

    QVector<SchemeData>& schemes() { return m_schemes; }
    const QVector<SchemeData>& schemes() const { return m_schemes; }

    /// 从 `config/process.json` 载入全部方案。
    ///
    /// **幂等**：已载入过则直接返回，不重复读盘 —— 该函数有两个调用点
    /// （`MainWindow.cpp` 构造期、`ProcessPage.cpp` 构造期），重复读盘会覆盖内存中
    /// 尚未保存的修改。需要强制重读时显式传 `force = true`。
    void load(bool force = false);
    void save();

    /// 为方案内的新节点分配编号：取 `scheme.nextActionId` 并推进计数器。
    ///
    /// **不按 `max(id)+1` 现算**——那样在「删掉最大编号后新增」时会复用刚被删除的编号，
    /// 使既有的 `sourceActionId` 引用（P5）静默指向新节点。计数器只增不减。
    /// 兜底遍历**整棵树**（分组也占编号，§7.2）。
    static int allocateActionId(SchemeData& scheme);

    /// 规范化方案内的节点树，返回**被改写的节点数**（幂等，0 表示无需改动）。
    ///
    /// 三件事：① 把非法的「分组内嵌分组」拉平（§Q5 写死两级）；② 保证全树编号唯一且为正
    /// （缺失 / 非正 / 与前面重复的编号按「方案内最大编号 + 1」依次补发；显式且不冲突的编号
    /// 一律保留）；③ 把 `nextActionId` 抬到不冲突的位置。
    /// 旧 `process.json` 无 `id` 字段 → 按「一级节点顺序 + 分组内子节点顺序」补成 1..N
    /// （自动升级，不需用户手工迁移）。
    static int normalizeActionIds(SchemeData& scheme);

    /// 按编号在全树（一级节点 + 分组子节点）里查找节点。找不到返回 nullptr。
    static ActionData* findNode(QVector<ActionData>& nodes, int id);
    static const ActionData* findNode(const QVector<ActionData>& nodes, int id);

    /// 查找某编号节点**所属的分组**（顶层节点返回 nullptr）。
    /// 用途：UI 判断插入位置 / 同级移动边界 / 组内资源冲突校验。
    static ActionData* findParentGroup(QVector<ActionData>& nodes, int id);
    static const ActionData* findParentGroup(const QVector<ActionData>& nodes, int id);

    /// 动作类型占用的**硬件资源标识集合**（§六：冲突校验按资源，不按类型）。
    /// 空列表 = 不占硬件资源（`Delay`），同组内可任意重复。
    static QStringList resourceKeysOf(ActionType t);

    /// 校验一个分组的子节点集合是否合法（§6.2 资源互斥 + §6.4 数据依赖 + §Q5 不嵌套）。
    /// 返回空字符串 = 合法；否则返回可直接展示给用户的中文原因。
    static QString validateGroupChildren(const QVector<ActionData>& children);

    static QString actionTypeName(ActionType t);
    static QString generateUniqueSchemeName(const QVector<SchemeData>& existing);

private:
    ProcessManager() = default;

    QVector<SchemeData> m_schemes;
    bool m_loaded = false;
};
