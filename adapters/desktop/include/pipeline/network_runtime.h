#pragma once
#include "pipeline/audio_pipeline.h"
#include <QObject>
#include <QJsonObject>
#include <memory>
namespace pipeline {
// Configures direct transport only for graph network nodes. No sockets for local graphs.
class NetworkRuntime final : public QObject {
  public:
    explicit NetworkRuntime(AudioPipeline &engine, QJsonObject config = {}, QObject *parent = nullptr);
    ~NetworkRuntime();
    bool refresh(QString &error);
    QJsonObject capabilities() const;
    QJsonObject diagnostics() const;

  private:
    class Impl;
    std::unique_ptr<Impl> d;
};
} // namespace pipeline
