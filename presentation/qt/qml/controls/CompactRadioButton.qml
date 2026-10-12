import QtQuick
import QtQuick.Controls

RadioButton {
    id: control
    FontMetrics { id: metrics; font: control.font }
    implicitHeight: Math.max(24, metrics.height + 6)
    padding: 0
    spacing: 4
}
