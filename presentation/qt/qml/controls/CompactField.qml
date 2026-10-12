import QtQuick
import QtQuick.Controls

TextField {
    id: control
    FontMetrics { id: metrics; font: control.font }
    implicitHeight: Math.max(24, metrics.height + 8)
    leftPadding: 5
    rightPadding: 5
    topPadding: 2
    bottomPadding: 2
    selectByMouse: true
}
