#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

class ConfigManager : public QObject
{
    Q_OBJECT

public:
    static ConfigManager& instance();

    bool load(const QString& filePath);
    bool save();

    nlohmann::json& root() { return root_; }
    const nlohmann::json& root() const { return root_; }

    QString filePath() const { return filePath_; }

    void markDirty() { if (!saveTimer_->isActive()) saveTimer_->start(); }

    /// 瀵艰埅 dot-path锛堝惈 `key[i]`锛夊苟杩斿洖寮曠敤锛沺ath 绌烘椂杩斿洖 root_
    nlohmann::json& get(const std::string& path);

    /// 鍐欏叆鍊硷紙path 绌烘椂涓嶅仛浠讳綍浜嬶級
    void set(const std::string& path, nlohmann::json value);

    template<typename T>
    T getValue(const std::string& path, T defaultVal = T()) const
    {
        if (path.empty()) return defaultVal;
        try {
            const auto* j = &root_;
            if (j->is_null()) return defaultVal;
            std::string remaining = path;
            while (!remaining.empty()) {
                auto dot = remaining.find('.');
                std::string key = (dot == std::string::npos) ? remaining : remaining.substr(0, dot);
                remaining = (dot == std::string::npos) ? "" : remaining.substr(dot + 1);
                auto bracket = key.find('[');
                if (bracket != std::string::npos) {
                    auto close = key.find(']', bracket);
                    if (close == std::string::npos) return defaultVal;
                    std::string arrName = key.substr(0, bracket);
                    int idx = std::stoi(key.substr(bracket + 1, close - bracket - 1));
                    if (!j->is_object() || !j->contains(arrName)) return defaultVal;
                    j = &(*j)[arrName];
                    if (!j->is_array() || idx < 0 || idx >= static_cast<int>(j->size())) return defaultVal;
                    j = &(*j)[idx];
                } else {
                    if (!j->is_object() || !j->contains(key)) return defaultVal;
                    j = &(*j)[key];
                }
            }
            return j->get<T>();
        } catch (const std::exception& e) {
            SPDLOG_INFO("[Config] getValue error: {} - {}", path, e.what());
            return defaultVal;
        }
    }

private:
    ConfigManager();
    ~ConfigManager() override = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void scheduleSave();

    nlohmann::json root_ = nlohmann::json::object();
    QString filePath_;
    QTimer* saveTimer_ = nullptr;
};
