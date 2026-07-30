#include "ProcessManager.h"

#include <QFile>
#include <QDir>

#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <random>

static QString processJsonPath()
{
    return QStringLiteral(PROJECT_SOURCE_DIR "/config/process.json");
}

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
    case ActionType::Move:    return QStringLiteral("移动");
    case ActionType::Vision:  return QStringLiteral("识别");
    case ActionType::Extrude: return QStringLiteral("挤压");
    case ActionType::Delay:   return QStringLiteral("延时");
    case ActionType::Gripper: return QStringLiteral("夹爪");
    }
    return QStringLiteral("未知");
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
void ProcessManager::load()
{
    m_schemes.clear();

    QFile file(processJsonPath());
    if (!file.open(QIODevice::ReadOnly)) {
        SPDLOG_INFO("[Process] No process.json found, generating default test scheme");

        SchemeData def;
        def.schemeName = QStringLiteral("方案_100");

        ActionData a1; a1.name = QStringLiteral("移动到：安全上方点"); a1.type = ActionType::Move;
        PointData p1; p1.name = QStringLiteral("safe_top"); p1.x = 0; p1.y = 200; p1.z = 50; p1.r = 0; p1.posture = QStringLiteral("elbow_up");
        a1.points.push_back(p1);
        def.actions.push_back(a1);

        ActionData a2; a2.name = QStringLiteral("打开夹爪"); a2.type = ActionType::Gripper; a2.isGripperOpen = true;
        def.actions.push_back(a2);

        ActionData a3; a3.name = QStringLiteral("等待视觉结果"); a3.type = ActionType::Vision;
        a3.visionType = QStringLiteral("CCD"); a3.exposure = 30.0; a3.templateName = QStringLiteral("template_a"); a3.threshold = 0.85;
        def.actions.push_back(a3);

        ActionData a4; a4.name = QStringLiteral("直线插补：插入灌装口"); a4.type = ActionType::Move;
        PointData p2; p2.name = QStringLiteral("fill_start"); p2.x = 85; p2.y = 92; p2.z = 22; p2.r = 2; p2.posture = QStringLiteral("elbow_up");
        PointData p3; p3.name = QStringLiteral("fill_end");   p3.x = 85; p3.y = 92; p3.z = 10; p3.r = 2; p3.posture = QStringLiteral("elbow_down");
        a4.points.push_back(p2); a4.points.push_back(p3);
        def.actions.push_back(a4);

        ActionData a5; a5.name = QStringLiteral("挤出奶油"); a5.type = ActionType::Extrude;
        a5.extrudeAmount = 5.0; a5.extrudeSpeed = 2.0; a5.suckBackAmount = 1.0; a5.suckBackSpeed = 3.0;
        def.actions.push_back(a5);

        ActionData a6; a6.name = QStringLiteral("关闭夹爪"); a6.type = ActionType::Gripper; a6.isGripperOpen = false;
        def.actions.push_back(a6);

        ActionData a7; a7.name = QStringLiteral("移动到放置点"); a7.type = ActionType::Move;
        PointData p4; p4.name = QStringLiteral("place"); p4.x = 150; p4.y = 0; p4.z = 30; p4.r = 0; p4.posture = QStringLiteral("elbow_up");
        a7.points.push_back(p4);
        def.actions.push_back(a7);

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
                if (sj.contains("actions") && sj["actions"].is_array()) {
                    for (const auto& aj : sj["actions"]) {
                        ActionData a;
                        a.name = QString::fromStdString(aj.value("name", ""));
                        a.type = static_cast<ActionType>(aj.value("type", 0));
                        a.visionType = QString::fromStdString(aj.value("visionType", ""));
                        a.exposure = aj.value("exposure", 0.0);
                        a.templateName = QString::fromStdString(aj.value("templateName", ""));
                        a.threshold = aj.value("threshold", 0.0);
                        a.extrudeAmount = aj.value("extrudeAmount", 0.0);
                        a.extrudeSpeed = aj.value("extrudeSpeed", 0.0);
                        a.suckBackAmount = aj.value("suckBackAmount", 0.0);
                        a.suckBackSpeed = aj.value("suckBackSpeed", 0.0);
                        a.delayMs = aj.value("delayMs", 0);
                        a.isGripperOpen = aj.value("isGripperOpen", false);

                        if (aj.contains("points") && aj["points"].is_array()) {
                            for (const auto& pj : aj["points"]) {
                                PointData p;
                                p.name = QString::fromStdString(pj.value("name", ""));
                                p.x = pj.value("x", 0.0);
                                p.y = pj.value("y", 0.0);
                                p.z = pj.value("z", 0.0);
                                p.r = pj.value("r", 0.0);
                                p.posture = QString::fromStdString(pj.value("posture", "elbow_up"));
                                a.points.push_back(p);
                            }
                        }
                        s.actions.push_back(a);
                    }
                }
                m_schemes.push_back(s);
            }
        }

        SPDLOG_INFO("[Process] Loaded {} schemes from {}", m_schemes.size(), processJsonPath().toStdString());
    } catch (const std::exception& e) {
        SPDLOG_INFO("[Process] Parse error: {}", e.what());
        m_schemes.clear();
    }
}

void ProcessManager::save()
{
    nlohmann::json j;
    j["schemes"] = nlohmann::json::array();

    for (const auto& scheme : m_schemes) {
        nlohmann::json sj;
        sj["schemeName"] = scheme.schemeName.toStdString();
        sj["actions"] = nlohmann::json::array();

        for (const auto& action : scheme.actions) {
            nlohmann::json aj;
            aj["name"] = action.name.toStdString();
            aj["type"] = static_cast<int>(action.type);
            aj["visionType"] = action.visionType.toStdString();
            aj["exposure"] = action.exposure;
            aj["templateName"] = action.templateName.toStdString();
            aj["threshold"] = action.threshold;
            aj["extrudeAmount"] = action.extrudeAmount;
            aj["extrudeSpeed"] = action.extrudeSpeed;
            aj["suckBackAmount"] = action.suckBackAmount;
            aj["suckBackSpeed"] = action.suckBackSpeed;
            aj["delayMs"] = action.delayMs;
            aj["isGripperOpen"] = action.isGripperOpen;
            aj["points"] = nlohmann::json::array();

            for (const auto& pt : action.points) {
                nlohmann::json pj;
                pj["name"] = pt.name.toStdString();
                pj["x"] = pt.x;
                pj["y"] = pt.y;
                pj["z"] = pt.z;
                pj["r"] = pt.r;
                pj["posture"] = pt.posture.toStdString();
                aj["points"].push_back(pj);
            }

            sj["actions"].push_back(aj);
        }
        j["schemes"].push_back(sj);
    }

    std::ofstream file(processJsonPath().toStdString());
    if (file.is_open()) {
        file << j.dump(4);
        SPDLOG_INFO("[Process] Saved schemes to {}", processJsonPath().toStdString());
    }
}
