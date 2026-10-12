#pragma once
#include <QApplication>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QThread>

// Call on the rendering thread with its actual OpenGL context current. This
// identifies the renderer in use, rather than guessing from installed adapters.
class RendererProbeTelemetry {
public:
    void capture() {
        QMutexLocker lock(&mutex_);
        if(!identity_.isEmpty())return;
        auto* context=QOpenGLContext::currentContext();
        if(!context)return;
        auto* functions=context->functions();
        const auto string=[&](GLenum name){const auto* bytes=functions->glGetString(name);
            return bytes?QString::fromLatin1(reinterpret_cast<const char*>(bytes)):QString{};};
        identity_={{"gl_vendor",string(GL_VENDOR)},{"gl_renderer",string(GL_RENDERER)},
            {"gl_version",string(GL_VERSION)},{"context_samples",context->format().samples()},
            {"context_swap_interval",context->format().swapInterval()},
            {"render_thread_id",QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()),16)},
            {"render_thread_is_gui",QThread::currentThread()==qApp->thread()}};
    }
    QJsonObject snapshot() const {QMutexLocker lock(&mutex_);return identity_;}
private:
    mutable QMutex mutex_;
    QJsonObject identity_;
};
