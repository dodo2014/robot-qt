#include "ConfigManager.h"

#include <QFile>
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
        spdlog::info("[Config] Cannot open: {}", filePath.toStdString());
        return false;
    }
    try {
        root_ = nlohmann::json::parse(file.readAll().toStdString());
        if (!root_.is_object()) {
            spdlog::info("[Config] Root is not an object, resetting");
            root_ = nlohmann::json::object();
        }
        spdlog::info("[Config] Loaded: {}", filePath.toStdString());
        return true;
    } catch (const std::exception& e) {
        spdlog::info("[Config] Parse error: {}", e.what());
        root_ = nlohmann::json::object();
        return false;
    }
}

bool ConfigManager::save()
{
    if (filePath_.isEmpty()) {
        spdlog::info("[Config] No file path, cannot save");
        return false;
    }
    QFile file(filePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        spdlog::info("[Config] Cannot write: {}", filePath_.toStdString());
        return false;
    }
    try {
        auto str = root_.dump(4);
        file.write(str.data(), static_cast<qint64>(str.size()));
        file.close();
        spdlog::info("[Config] Saved: {}", filePath_.toStdString());
        return true;
    } catch (const std::exception& e) {
        spdlog::info("[Config] Save error: {}", e.what());
        return false;
    }
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
            spdlog::info("[Config] get() error at token '{}': {}", token, e.what());
            return root_;
        }
    }
    return *j;
}

void ConfigManager::set(const std::string& path, nlohmann::json value)
{
    if (path.empty()) {
        spdlog::info("[Config] Ignored set() with empty path");
        return;
    }
    try {
        get(path) = std::move(value);
        scheduleSave();
    } catch (const std::exception& e) {
        spdlog::info("[Config] set() error at '{}': {}", path, e.what());
    }
}

void ConfigManager::scheduleSave()
{
    if (!saveTimer_->isActive())
        saveTimer_->start();
}
