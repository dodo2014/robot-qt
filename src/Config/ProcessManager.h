#pragma once

#include <QVector>
#include <QString>

enum class ActionType { Move = 0, Vision, Extrude, Delay, Gripper };

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

struct ActionData {
    QString name;
    ActionType type = ActionType::Move;
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
};

struct SchemeData {
    QString schemeName;
    QVector<ActionData> actions;
};

class ProcessManager
{
public:
    static ProcessManager& instance();

    QVector<SchemeData>& schemes() { return m_schemes; }
    const QVector<SchemeData>& schemes() const { return m_schemes; }

    void load();
    void save();

    static QString actionTypeName(ActionType t);
    static QString generateUniqueSchemeName(const QVector<SchemeData>& existing);

private:
    ProcessManager() = default;

    QVector<SchemeData> m_schemes;
};
