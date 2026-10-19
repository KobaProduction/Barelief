#include "product_updates.h"
#include <core_app/product_identity.h>
#include <barelief/ui_text.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVersionNumber>

namespace {
QString release_status(const QJsonObject& object) {
    auto tag=object.value("tag_name").toString();
    if(tag.startsWith('v'))tag.remove(0,1);
    static const QRegularExpression stable(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
    if(!stable.match(tag).hasMatch() || object.value("draft").toBool() || object.value("prerelease").toBool())
        return ui::text("No supported release is available.");
    if(QVersionNumber::fromString(tag)>QVersionNumber::fromString(barelief::version))
        return ui::text("A new version is available: %1").arg(tag);
    return ui::text("You are using the latest version.");
}
void finish_check(QNetworkReply& reply,QLabel& status,QPushButton& release) {
    const auto code=reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if(code==404){status.setText(ui::text("No published releases are available yet."));return;}
    if(reply.error()!=QNetworkReply::NoError || reply.bytesAvailable()>1048576){
        status.setText(ui::text("Could not check for updates. Please try again later."));return;
    }
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(reply.readAll(),&error);
    if(error.error!=QJsonParseError::NoError || !document.isObject()){
        status.setText(ui::text("Could not check for updates. Please try again later."));return;
    }
    status.setText(release_status(document.object()));
    release.setVisible(true);
}
void bound_reply(QNetworkReply& reply) {
    reply.setReadBufferSize(1048577);
    QObject::connect(&reply,&QNetworkReply::readyRead,&reply,[&reply]{
        if(reply.bytesAvailable()>1048576)reply.abort();
    });
    QTimer::singleShot(10000,&reply,[&reply]{if(reply.isRunning())reply.abort();});
}
}
void barelief::check_updates(QPushButton& check,QLabel& status,QPushButton& release) {
    check.setEnabled(false);release.setVisible(false);
    status.setText(ui::text("Checking for updates…"));
    auto* network=new QNetworkAccessManager(&check);
    QNetworkRequest request{QUrl(release_api)};
    request.setRawHeader("User-Agent",QByteArray("Barelief/")+version);
    request.setRawHeader("Accept","application/vnd.github+json");
    request.setTransferTimeout(10000);
    auto* reply=network->get(request);bound_reply(*reply);
    QObject::connect(reply,&QNetworkReply::finished,&check,[reply,network,&check,&status,&release]{
        finish_check(*reply,status,release);check.setEnabled(true);network->deleteLater();
    });
}
