#ifndef CORE_APP_CONFIG_MANAGER_H
#define CORE_APP_CONFIG_MANAGER_H

#include "core_app/types.h"
#include <desktop/numeric_project_document.h>

class ConfigManager {
  public:
    explicit ConfigManager(QString config_path = {});

    bool loadNumeric(desktop::NumericProjectDocument&, domain::NumericProcessingGraph&,
                     QString *error = nullptr) const;
    bool readNumericSettings(const desktop::NumericProjectDocument&, FileSaveApplicationConfig&, QString *error = nullptr) const;
    bool numericSettingsDelta(const desktop::NumericProjectDocument&, const FileSaveApplicationConfig&, QJsonObject&, QString *error = nullptr) const;
    // Persists only UI settings against the existing source-owned document.
    // Does not extract/stage a draft or change the applied graph.
    bool saveNumericPreferences(desktop::NumericProjectDocument&, const FileSaveApplicationConfig&,
                                QString *error = nullptr) const;
    bool saveNumeric(desktop::NumericProjectDocument&, const domain::NumericProcessingGraph&,
                     QString *error = nullptr) const;
    bool saveNumeric(desktop::NumericProjectDocument&, const domain::NumericProcessingGraph&,
                     const desktop::NumericDraftPresentation&, QString *error = nullptr) const;
    bool saveNumericAs(desktop::NumericProjectDocument&, const domain::NumericProcessingGraph&,
                       const QString &path, QString *error = nullptr);
    bool saveNumericAs(desktop::NumericProjectDocument&, const domain::NumericProcessingGraph&,
                       const desktop::NumericDraftPresentation&, const QString &path, QString *error = nullptr);

    [[nodiscard]] QString getConfigPath() const;

    void setConfigPath(const QString &path);

  private:
    QString _config_path;

    static void _parse_core_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config);
    static void _parse_ui_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config);
    static void _parse_wled_ar_server_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config);

    static void _save_aggregator_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config);
    static void _save_ui_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config);
    static void _save_wled_ar_server_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config);
};

#endif // CORE_APP_CONFIG_MANAGER_H
