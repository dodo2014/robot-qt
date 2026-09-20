#include "ConfigManager.h"

#include <QFile>
#include <QSaveFile>
#include <spdlog/spdlog.h>

namespace {

struct KeyInfo {
    std::string name;
    int index = -1;
};

KeyInfo parseKey(const std::string& key)
{
    auto b = key.find('[');
    if (b == std::string::npos) return { key, -1 };
    auto c = key.find(']', b);
    if (c == std::string::npos) return { key, -1 };
    return { key.substr(0, b), std::stoi(key.substr(b + 1, c - b - 1)) };
}

} // namespace

ConfigManager& ConfigManager::instance()
{
    static ConfigManager inst;
    return inst;
}

ConfigManager::ConfigManager()
    : QObject(nullptr)
{
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(300);
    connect(saveTimer_, &QTimer::timeout, this, &ConfigManager::save);
}

bool ConfigManager::load(const QString& filePath)
{
    filePath_ = filePath;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        SPDLOG_INFO("[Config] Cannot open: {}", filePath.toStdString());
        return false;
    }
    try {
        root_ = nlohmann::json::parse(file.readAll().toStdString());
        if (!root_.is_object()) {
            SPDLOG_INFO("[Config] Root is not an object, resetting");
            root_ = nlohmann::json::object();
        }
        SPDLOG_INFO("[Config] Loaded: {}", filePath.toStdString());
        return true;
    } catch (const std::exception& e) {
        SPDLOG_INFO("[Config] Parse error: {}", e.what());
        root_ = nlohmann::json::object();
        return false;
    }
}

bool ConfigManager::save()
{
    if (filePath_.isEmpty()) {
        SPDLOG_INFO("[Config] No file path, cannot save");
        return false;
    }

    std::string str;
    try {
        str = root_.dump(4);
    } catch (const std::exception& e) {
        SPDLOG_INFO("[Config] Serialize error: {}", e.what());
        return false;
    }

    // 原子写：QSaveFile 写同目录临时文件，commit() 时原子替换目标文件。
    //
    // 原实现以 WriteOnly|Truncate 直接打开目标文件再写入，写入途中崩溃/掉电会留下
    // 「已截断但未写完」的 config.json；load() 解析失败即把 root_ 重置为空对象
    // → 全部配置回落到默认值。见 doc/config_ownership_plan.md §七。
    QSaveFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly)) {
        SPDLOG_INFO("[Config] Cannot write: {}", filePath_.toStdString());
        return false;
    }
    if (file.write(str.data(), static_cast<qint64>(str.size())) != static_cast<qint64>(str.size())) {
        SPDLOG_INFO("[Config] Short write, aborting: {}", filePath_.toStdString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        SPDLOG_INFO("[Config] Commit failed, original kept: {}", filePath_.toStdString());
        return false;
    }
    SPDLOG_INFO("[Config] Saved: {}", filePath_.toStdString());
    return true;
}

nlohmann::json& ConfigManager::get(const std::string& path)
{
    if (path.empty()) return root_;

    auto* j = &root_;
    std::string remaining = path;
    while (!remaining.empty()) {
        auto dot = remaining.find('.');
        std::string token = (dot == std::string::npos) ? remaining : remaining.substr(0, dot);
        remaining = (dot == std::string::npos) ? "" : remaining.substr(dot + 1);
        auto ki = parseKey(token);
        try {
            if (ki.index >= 0) {
                if (!j->is_object()) *j = nlohmann::json::object();
                if (!j->contains(ki.name)) (*j)[ki.name] = nlohmann::json::array();
                j = &(*j)[ki.name];
                if (!j->is_array()) *j = nlohmann::json::array();
                if (ki.index >= static_cast<int>(j->size()))
                    j->push_back(nlohmann::json::object());
                j = &(*j)[ki.index];
            } else {
                if (!j->is_object()) *j = nlohmann::json::object();
                if (!j->contains(ki.name)) (*j)[ki.name] = nlohmann::json::object();
                j = &(*j)[ki.name];
            }
        } catch (const std::exception& e) {
            SPDLOG_INFO("[Config] get() error at token '{}': {}", token, e.what());
            return root_;
        }
    }
    return *j;
}

void ConfigManager::set(const std::string& path, nlohmann::json value)
{
    if (path.empty()) {
        SPDLOG_INFO("[Config] Ignored set() with empty path");
        return;
    }
    try {
        get(path) = std::move(value);
        scheduleSave();
    } catch (const std::exception& e) {
        SPDLOG_INFO("[Config] set() error at '{}': {}", path, e.what());
    }
}

void ConfigManager::scheduleSave()
{
    if (!saveTimer_->isActive())
        saveTimer_->start();
}
