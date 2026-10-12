import QtQuick
import QtQuick.Controls

Button {
    id: control
    FontMetrics { id: metrics; font: control.font }
    implicitHeight: Math.max(24, metrics.height + 8)
    leftPadding: 6
    rightPadding: 6
    topPadding: 2
    bottomPadding: 2
}
