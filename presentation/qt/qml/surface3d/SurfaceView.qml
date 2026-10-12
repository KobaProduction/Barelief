pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Visualization.Surface 1.0

Control {
    id: root
    width: 900; height: 600
    property var state: graphModel.quickState
    property real panelRatio: state.panelRatio
    function updatePanelWidth() {
        if (side && split && !graphModel.controlsEmbedded && !split.resizing)
            side.SplitView.preferredWidth = split.width * panelRatio
    }
    onPanelRatioChanged: updatePanelWidth()
    property bool controlsEmbedded: graphModel.controlsEmbedded
    onControlsEmbeddedChanged: if (!controlsEmbedded) updatePanelWidth()
    Component.onCompleted: updatePanelWidth()
    function tr(s) { const revision = quickUi.revision; return quickUi.text(s) }
    palette.window: quickUi.palette.window
    palette.base: quickUi.palette.base
    palette.text: quickUi.palette.text
    palette.windowText: quickUi.palette.windowText
    palette.button: quickUi.palette.button
    palette.buttonText: quickUi.palette.buttonText
    palette.highlight: quickUi.palette.highlight
    palette.highlightedText: quickUi.palette.highlightedText
    palette.mid: quickUi.palette.mid
    palette.disabled.text: quickUi.palette.disabled_text
    palette.disabled.buttonText: quickUi.palette.disabled_buttonText
    palette.disabled.windowText: quickUi.palette.disabled_windowText
    palette.inactive.text: quickUi.palette.inactive_text
    palette.inactive.buttonText: quickUi.palette.inactive_buttonText
    palette.inactive.windowText: quickUi.palette.inactive_windowText
    palette.inactive.window: quickUi.palette.inactive_window
    palette.inactive.base: quickUi.palette.inactive_base
    palette.inactive.button: quickUi.palette.inactive_button
    palette.inactive.highlight: quickUi.palette.inactive_highlight
    palette.inactive.highlightedText: quickUi.palette.inactive_highlightedText
    palette.inactive.mid: quickUi.palette.inactive_mid
    palette.disabled.window: quickUi.palette.disabled_window
    palette.disabled.base: quickUi.palette.disabled_base
    palette.disabled.button: quickUi.palette.disabled_button
    palette.disabled.highlight: quickUi.palette.disabled_highlight
    palette.disabled.highlightedText: quickUi.palette.disabled_highlightedText
    palette.disabled.mid: quickUi.palette.disabled_mid
    background: Rectangle { color: quickUi.palette.window }

    SplitView {
        id: split
        anchors.fill: parent
        orientation: Qt.Horizontal
        onWidthChanged: root.updatePanelWidth()
        onResizingChanged: if (!root.controlsEmbedded && !resizing && width > 0) graphModel.quick_action("panelRatio", side.width / width)
        SurfaceCanvas {
            id: surface
            objectName: "spectrogramContainer"
            SplitView.fillWidth: true
            SplitView.minimumWidth: 120
            focus: true
            onWidthChanged: graphModel.quick_viewport(width, height)
            onHeightChanged: graphModel.quick_viewport(width, height)
            onActiveFocusChanged: if (!activeFocus) graphModel.quick_cancel_input()
            MouseArea {
                anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                hoverEnabled: true
                onPressed: mouse => {surface.forceActiveFocus(); graphModel.quick_pointer(0,mouse.x,mouse.y,mouse.button,mouse.buttons,mouse.modifiers)}
                onPositionChanged: mouse => graphModel.quick_pointer(1,mouse.x,mouse.y,mouse.button,mouse.buttons,mouse.modifiers)
                onReleased: mouse => graphModel.quick_pointer(2,mouse.x,mouse.y,mouse.button,mouse.buttons,mouse.modifiers)
                onCanceled: graphModel.quick_cancel_input()
                onExited: graphModel.quick_action("cursorReset")
                onWheel: wheel => {
                    const pixels = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0
                    graphModel.quick_wheel(pixels ? wheel.pixelDelta.x : wheel.angleDelta.x, pixels ? wheel.pixelDelta.y : wheel.angleDelta.y, pixels)
                    wheel.accepted = true
                }
            }
            SurfaceOverlay {
                objectName: "spectrogramOverlay"
                anchors.fill: parent
                sourceModel: graphModel
            }
        }
        ScrollView {
            id: side
            visible: !root.controlsEmbedded
            SplitView.preferredWidth: split.width * root.panelRatio
            SplitView.minimumWidth: 180
            clip: true
            contentWidth: availableWidth
            SurfaceControls {
                width: side.availableWidth
                surfaceModel: graphModel
            }
        }
    }
}
