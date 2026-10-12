import QtQuick
import QtQuick.Controls

GroupBox {
    id: control
    FontMetrics { id: metrics; font: control.font }
    padding: 6
    topPadding: title.length ? metrics.height + 6 : 6
}
