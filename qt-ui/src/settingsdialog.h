#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QHash>
class HostClient;
class QLabel;
class QPushButton;
class QTabWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(HostClient *host, const QStringList &profiles, QWidget *parent = nullptr);
    static bool decodeSettings(const QJsonValue &value, QJsonObject &settings, QString &error);
    void setSettings(const QJsonObject &settings);
    QJsonObject settings() const;
signals:
    void settingsSaved(const QJsonObject &settings);
private:
    void load();
    void save();
    HostClient *m_host;
    QString m_context;
    QJsonObject m_original;
    QHash<QString, QWidget *> m_fields;
    QTabWidget *m_tabs;
    QLabel *m_status;
    QPushButton *m_save;
    QPushButton *m_retry;
};
