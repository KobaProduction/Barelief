#pragma once
#include <QQuaternion>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace CameraGeometry {
inline float heading(const QQuaternion& orientation) {
    const auto forward=orientation.rotatedVector({0,0,-1});
    if(forward.x()*forward.x()+forward.z()*forward.z()>1e-8f)
        return std::atan2(-forward.x(),-forward.z())*180.f/std::numbers::pi_v<float>;
    const auto right=orientation.rotatedVector({1,0,0});
    return std::atan2(-right.z(),right.x())*180.f/std::numbers::pi_v<float>;
}
inline QQuaternion uprightLook(const QQuaternion& orientation,float yawDelta=0,float pitchDelta=0) {
    const auto forward=orientation.rotatedVector({0,0,-1});
    const auto pitch=std::asin(std::clamp(forward.y(),-1.f,1.f))*180.f/std::numbers::pi_v<float>;
    return QQuaternion::fromEulerAngles(std::clamp(pitch+pitchDelta,-89.f,89.f),heading(orientation)+yawDelta,0);
}
inline QVector3D horizontalMovement(const QQuaternion& orientation,const QVector3D& local) {
    return QQuaternion::fromEulerAngles(0,heading(orientation),0).rotatedVector({local.x(),0,local.z()})+
        QVector3D(0,local.y(),0);
}
// Clip the step along its own direction: a horizontal step must never alter height,
// including at the camera's existing distance limit.
inline QVector3D translatedPosition(const QVector3D& position,const QVector3D& step) {
    const auto next=position+step;
    if(next.lengthSquared()<=100.f || step.lengthSquared()==0)return next;
    const auto a=step.lengthSquared(),b=QVector3D::dotProduct(position,step);
    const auto t=std::clamp((-b+std::sqrt(std::max(0.f,b*b+a*(100.f-position.lengthSquared()))))/a,0.f,1.f);
    return position+step*t;
}
}
