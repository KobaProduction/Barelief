#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
class QWidget;
namespace ui_audit {
// Test-side evidence only. No dependency from product/domain code to this API.
class Recorder {
public:
    explicit Recorder(QString artifactDirectory);
    QJsonObject snapshot(QWidget* root) const;
    void transition(const QString& id,const QString& intent,QWidget* root,
                    const std::function<void()>& action,
                    const std::function<QStringList()>& violations,
                    const QString& dispatch=QStringLiteral("public_api"));
    void finding(const QString& id,const QString& severity,const QString& category,
                 const QString& observed,const QString& expected,const QJsonObject& evidence={});
    void coverage(const QString& area,const QString& status,const QString& reason);
    void evidence(const QString& key,const QJsonObject& value);
    QString artifactDirectory() const {return directory_;}
    bool save(const QString& group) const;
    int executionErrors() const;
private:
    QString directory_;
    QJsonArray transitions_,findings_,coverage_;
    QJsonObject evidence_;
};
void audit_workspace(Recorder&);
void audit_graphs(Recorder&);
void audit_application(Recorder&);
}
