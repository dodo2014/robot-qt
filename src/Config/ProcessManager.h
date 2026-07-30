#pragma once

#include <QVector>
#include <QString>

enum class ActionType { Move = 0, Vision, Extrude, Delay, Gripper };

struct PointData {
    QString name;
    double x = 0.0, y = 0.0, z = 0.0, r = 0.0;
    QString posture;
};

struct ActionData {
    QString name;
    ActionType type = ActionType::Move;
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
