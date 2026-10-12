import QtQuick
import QtQuick.Controls

// Fusion supplies the single platform-style indicator and editable content.
ComboBox {
    id: control
    property var labelForValue: null
    property var itemEnabled: null
    FontMetrics { id: metrics; font: control.font }
    implicitHeight: Math.max(24, metrics.height + 8)
    topPadding: 2
    bottomPadding: 2
    spacing: 4
    popup.padding: 1
    delegate: CompactDelegate {
        required property var modelData
        required property int index
        width: control.popup.availableWidth
        text: control.labelForValue ? control.labelForValue(modelData, index)
             : control.textRole ? control.textAt(index) : String(modelData)
        enabled: !control.itemEnabled || control.itemEnabled(index)
        highlighted: control.highlightedIndex === index
    }
    ToolTip.visible: hovered && displayText.length > 0
    ToolTip.text: displayText
    ToolTip.delay: 500
}
