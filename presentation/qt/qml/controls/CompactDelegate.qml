import QtQuick
import QtQuick.Controls

ItemDelegate {
    id: control
    FontMetrics { id: metrics; font: control.font }
    implicitHeight: Math.max(24, metrics.height + 8)
    leftPadding: 6
    rightPadding: 6
    topPadding: 2
    bottomPadding: 2
    contentItem: Text {
        text: control.text
        font: control.font
        textFormat: Text.PlainText
        color: control.highlighted ? control.palette.highlightedText : control.palette.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    background: Rectangle {
        color: control.highlighted ? control.palette.highlight : control.palette.base
    }
}
