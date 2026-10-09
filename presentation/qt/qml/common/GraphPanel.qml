pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root
    property var presentation: graphModel
    property var appearance: quickUi
    padding: 10
    palette.window: root.appearance.palette.window
    palette.base: root.appearance.palette.base
    palette.text: root.appearance.palette.text
    palette.windowText: root.appearance.palette.windowText
    palette.button: root.appearance.palette.button
    palette.buttonText: root.appearance.palette.buttonText
    palette.highlight: root.appearance.palette.highlight
    palette.highlightedText: root.appearance.palette.highlightedText
    palette.mid: root.appearance.palette.mid
    palette.inactive.window: root.appearance.palette.inactive_window
    palette.inactive.base: root.appearance.palette.inactive_base
    palette.inactive.text: root.appearance.palette.inactive_text
    palette.inactive.windowText: root.appearance.palette.inactive_windowText
    palette.inactive.button: root.appearance.palette.inactive_button
    palette.inactive.buttonText: root.appearance.palette.inactive_buttonText
    palette.inactive.highlight: root.appearance.palette.inactive_highlight
    palette.inactive.highlightedText: root.appearance.palette.inactive_highlightedText
    palette.inactive.mid: root.appearance.palette.inactive_mid
    palette.disabled.window: root.appearance.palette.disabled_window
    palette.disabled.base: root.appearance.palette.disabled_base
    palette.disabled.text: root.appearance.palette.disabled_text
    palette.disabled.windowText: root.appearance.palette.disabled_windowText
    palette.disabled.button: root.appearance.palette.disabled_button
    palette.disabled.buttonText: root.appearance.palette.disabled_buttonText
    palette.disabled.highlight: root.appearance.palette.disabled_highlight
    palette.disabled.highlightedText: root.appearance.palette.disabled_highlightedText
    palette.disabled.mid: root.appearance.palette.disabled_mid
    function tr(source) { let revision = root.appearance.revision; return root.appearance.text(source) }
    background: Rectangle { color: root.appearance.palette.window }
    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 10
            Label { text: root.tr("Display mode"); Layout.fillWidth: true }
            ComboBox {
                objectName: "numericDisplayMode"
                Layout.fillWidth: true
                model: root.presentation.modes
                currentIndex: root.presentation.modes.indexOf(root.presentation.displayMode)
                enabled: count > 1
                displayText: { let revision = root.appearance.revision; return root.presentation.modeLabel(root.presentation.displayMode) }
                delegate: ItemDelegate {
                    required property string modelData
                    width: parent.width
                    text: { let revision = root.appearance.revision; return root.presentation.modeLabel(modelData) }
                    highlighted: root.presentation.displayMode === modelData
                }
                onActivated: (index) => root.presentation.selectMode(model[index])
            }
            ColumnLayout {
                visible: root.presentation.modes.indexOf("image2d") >= 0
                Layout.fillWidth: true
                Label { text: root.tr("Frequency axis") }
                ComboBox {
                    objectName: "frequencyAxisMode"
                    Layout.fillWidth: true
                    model: [root.tr(root.presentation.physicalFrequency ? "Hertz (source coordinates)" : "Hertz unavailable: source has no coordinates"), root.tr("Positions")]
                    currentIndex: root.presentation.preferences.frequency_axis_mode === "positions" ? 1 : 0
                    delegate: ItemDelegate { required property string modelData; required property int index; text: modelData; enabled: index > 0 || root.presentation.physicalFrequency; width: parent.width }
                    onActivated: (index) => root.presentation.changePreference("frequency_axis_mode", index === 0 ? "coordinates" : "positions")
                    ToolTip.text: root.tr("Physical coordinates require source metadata; otherwise positions are shown.")
                    ToolTip.visible: hovered
                }
                Label { text: root.tr("Time axis") }
                ComboBox {
                    objectName: "timeAxisMode"
                    Layout.fillWidth: true
                    model: [root.tr(root.presentation.physicalTime ? "Delay in seconds (source coordinates)" : "Seconds unavailable: source has no timestamps"), root.tr("Temporal frames")]
                    currentIndex: root.presentation.preferences.time_axis_mode === "positions" ? 1 : 0
                    delegate: ItemDelegate { required property string modelData; required property int index; text: modelData; enabled: index > 0 || root.presentation.physicalTime; width: parent.width }
                    onActivated: (index) => root.presentation.changePreference("time_axis_mode", index === 0 ? "coordinates" : "positions")
                }
            }
            Repeater {
                model: root.presentation.displayMode === "metrics" || root.presentation.displayMode === "surface3d" ? [] : ["x", "y"]
                delegate: GroupBox {
                    id: axisEditor
                    required property string modelData
                    property bool xAxis: modelData === "x"
                    property string lowKey: xAxis ? "minimum_x" : "minimum"
                    property string highKey: xAxis ? "maximum_x" : "maximum"
                    property string autoKey: xAxis ? "auto_range_x" : "auto_range"
                    title: root.tr(axisEditor.xAxis ? "Axis range X" : "Axis range Y")
                    Layout.fillWidth: true
                    ColumnLayout {
                        anchors.fill: parent
                        CheckBox {
                            text: root.tr("Auto")
                            checked: Boolean(root.presentation.preferences[axisEditor.autoKey])
                            onClicked: root.presentation.changePreference(axisEditor.autoKey, checked)
                        }
                        Label { text: root.tr("Minimum") }
                        TextField {
                            Layout.fillWidth: true
                            enabled: !root.presentation.preferences[axisEditor.autoKey]
                            text: String(root.presentation.preferences[axisEditor.lowKey])
                            validator: DoubleValidator { locale: "C" }
                            onEditingFinished: { root.presentation.changePreference(axisEditor.lowKey, Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences[axisEditor.lowKey]) }) }
                        }
                        Label { text: root.tr("Maximum") }
                        TextField {
                            Layout.fillWidth: true
                            enabled: !root.presentation.preferences[axisEditor.autoKey]
                            text: String(root.presentation.preferences[axisEditor.highKey])
                            validator: DoubleValidator { locale: "C" }
                            onEditingFinished: { root.presentation.changePreference(axisEditor.highKey, Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences[axisEditor.highKey]) }) }
                        }
                        CheckBox {
                            text: root.tr("Reverse")
                            checked: Boolean(root.presentation.preferences[axisEditor.xAxis ? "reverse_x" : "reverse_y"])
                            onClicked: root.presentation.changePreference(axisEditor.xAxis ? "reverse_x" : "reverse_y", checked)
                        }
                        Label { visible: !axisEditor.xAxis; text: root.tr("Auto range interval (s)") }
                        TextField {
                            visible: !axisEditor.xAxis
                            Layout.fillWidth: true
                            enabled: root.presentation.preferences.auto_range
                            text: String(root.presentation.preferences.auto_range_interval_seconds)
                            validator: DoubleValidator { bottom: 1; top: 60; locale: "C" }
                            onEditingFinished: { root.presentation.changePreference("auto_range_interval_seconds", Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences.auto_range_interval_seconds) }) }
                        }
                    }
                }
            }
            Label { visible: root.presentation.planeCount > 1; text: root.tr("Channels") }
            ComboBox {
                objectName: "numericPlaneSelector"
                visible: root.presentation.planeCount > 1
                Layout.fillWidth: true
                model: root.presentation.planeCount
                currentIndex: root.presentation.selectedPlane
                displayText: root.tr("Channel %1").arg(currentIndex)
                delegate: ItemDelegate { required property int index; text: root.tr("Channel %1").arg(index); width: parent.width }
                onActivated: (index) => root.presentation.selectPlane(index)
            }
            Label { visible: root.presentation.displayMode !== "metrics" && root.presentation.displayMode !== "surface3d"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.presentation.information }
            TextArea {
                objectName: "numericExactValue"
                visible: root.presentation.displayMode !== "metrics" && text.length > 0
                Layout.fillWidth: true
                readOnly: true
                wrapMode: Text.Wrap
                text: root.presentation.diagnostic
                selectByMouse: true
                background: null
            }
            Button { objectName: "graphRename"; text: root.tr("Rename graph"); Layout.fillWidth: true; onClicked: root.presentation.rename() }
            GroupBox {
                visible: root.presentation.displayMode === "metrics"
                title: root.tr("Inputs")
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    Repeater {
                        model: root.presentation.scalarInputs
                        delegate: ColumnLayout {
                            id: metricRow
                            required property var modelData
                            Layout.fillWidth: true
                            TextField {
                                Layout.fillWidth: true
                                text: metricRow.modelData.name
                                placeholderText: root.tr("Value %1").arg(metricRow.modelData.port + 1)
                                onEditingFinished: { root.presentation.nameScalarInput(metricRow.modelData.port, text); text = Qt.binding(function() { return metricRow.modelData.name }) }
                            }
                            RowLayout {
                                CheckBox {
                                    text: root.tr("Show value")
                                    checked: metricRow.modelData.enabled
                                    onClicked: root.presentation.enableScalarInput(metricRow.modelData.port,checked)
                                }
                                Button {
                                    text: root.tr("Remove input")
                                    enabled: metricRow.modelData.removeEnabled
                                    onClicked: root.presentation.removeScalarInput(metricRow.modelData.port)
                                }
                            }
                        }
                    }
                    Button { text: root.tr("Add input"); enabled: root.presentation.canAddScalarInput; onClicked: root.presentation.addScalarInput() }
                }
            }
        }
    }
}
