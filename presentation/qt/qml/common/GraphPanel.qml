pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"
import "../surface3d"

Pane {
    id: root
    property var presentation: graphModel
    property var appearance: quickUi
    padding: 6
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
        id: scroller
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: scroller.availableWidth
            spacing: 4
            Label { text: root.tr("Display mode"); Layout.fillWidth: true }
            CompactComboBox {
                objectName: "numericDisplayMode"
                Layout.fillWidth: true
                model: root.presentation.modes
                currentIndex: root.presentation.modes.indexOf(root.presentation.displayMode)
                enabled: count > 1
                displayText: { let revision = root.appearance.revision; return root.presentation.modeLabel(root.presentation.displayMode) }
                labelForValue: function(value, index) { let revision = root.appearance.revision; return root.presentation.modeLabel(value) }
                onActivated: (index) => root.presentation.selectMode(model[index])
            }
            ColumnLayout {
                visible: root.presentation.modes.indexOf("image2d") >= 0
                Layout.fillWidth: true
                spacing: 4
                Label { text: root.tr("Frequency axis") }
                CompactComboBox {
                    objectName: "frequencyAxisMode"
                    Layout.fillWidth: true
                    model: [root.tr(root.presentation.physicalFrequency ? "Hertz (source coordinates)" : "Hertz unavailable: source has no coordinates"), root.tr("Positions")]
                    currentIndex: root.presentation.preferences.frequency_axis_mode === "positions" ? 1 : 0
                    itemEnabled: function(index) { return index > 0 || root.presentation.physicalFrequency }
                    onActivated: (index) => root.presentation.changePreference("frequency_axis_mode", index === 0 ? "coordinates" : "positions")
                }
                Label { text: root.tr("Time axis") }
                CompactComboBox {
                    objectName: "timeAxisMode"
                    Layout.fillWidth: true
                    model: [root.tr(root.presentation.physicalTime ? "Delay in seconds (source coordinates)" : "Seconds unavailable: source has no timestamps"), root.tr("Temporal frames")]
                    currentIndex: root.presentation.preferences.time_axis_mode === "positions" ? 1 : 0
                    itemEnabled: function(index) { return index > 0 || root.presentation.physicalTime }
                    onActivated: (index) => root.presentation.changePreference("time_axis_mode", index === 0 ? "coordinates" : "positions")
                }
            }
            Label { visible: root.presentation.planeCount > 1; text: root.tr("Channels") }
            CompactComboBox {
                objectName: "numericPlaneSelector"
                visible: root.presentation.planeCount > 1
                Layout.fillWidth: true
                model: root.presentation.planeCount
                currentIndex: root.presentation.selectedPlane
                displayText: root.tr("Channel %1").arg(currentIndex)
                labelForValue: function(value, index) { return root.tr("Channel %1").arg(index) }
                onActivated: (index) => root.presentation.selectPlane(index)
            }
            CompactButton { objectName: "graphRename"; text: root.tr("Rename graph"); Layout.fillWidth: true; onClicked: root.presentation.rename() }
            Loader {
                Layout.fillWidth: true
                active: root.presentation.displayMode === "surface3d" && root.presentation.surfaceModel !== null
                visible: active
                sourceComponent: Component {
                    SurfaceControls { surfaceModel: root.presentation.surfaceModel }
                }
            }
            Repeater {
                model: root.presentation.displayMode === "line" ? [{key:"line_width",label:"Line width",low:0.5,high:20}] :
                       root.presentation.displayMode === "points" ? [{key:"point_size",label:"Point size",low:1,high:64}] :
                       root.presentation.displayMode === "bars" ? [{key:"bar_width_percent",label:"Bar width (%)",low:1,high:100}] : []
                delegate: RowLayout {
                    id: seriesEditor
                    required property var modelData
                    Layout.fillWidth: true
                    Label { text: root.tr(seriesEditor.modelData.label); Layout.fillWidth: true; wrapMode: Text.Wrap }
                    CompactField {
                        objectName: seriesEditor.modelData.key
                        Layout.preferredWidth: 64
                        text: String(root.presentation.preferences[seriesEditor.modelData.key])
                        validator: DoubleValidator { bottom: seriesEditor.modelData.low; top: seriesEditor.modelData.high; locale: "C" }
                        onEditingFinished: { root.presentation.changePreference(seriesEditor.modelData.key, Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences[seriesEditor.modelData.key]) }) }
                    }
                }
            }
            CompactCheckBox {
                objectName: "pieSliceLabels"
                visible: root.presentation.displayMode === "pie"
                text: root.tr("Show slice labels")
                checked: Boolean(root.presentation.preferences.pie_labels_visible)
                onClicked: root.presentation.changePreference("pie_labels_visible", checked)
            }
            Repeater {
                objectName: "cartesianAxisEditors"
                model: (root.presentation.displayMode === "metrics" || root.presentation.displayMode === "signedbar") || root.presentation.displayMode === "surface3d" || root.presentation.displayMode === "pie" ? [] : ["x", "y"]
                delegate: CompactGroupBox {
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
                        spacing: 4
                        RowLayout {
                            spacing: 8
                            CompactCheckBox {
                                text: root.tr("Auto")
                                checked: Boolean(root.presentation.preferences[axisEditor.autoKey])
                                onClicked: root.presentation.changePreference(axisEditor.autoKey, checked)
                            }
                            CompactCheckBox {
                                text: root.tr("Reverse")
                                checked: Boolean(root.presentation.preferences[axisEditor.xAxis ? "reverse_x" : "reverse_y"])
                                onClicked: root.presentation.changePreference(axisEditor.xAxis ? "reverse_x" : "reverse_y", checked)
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Label { text: root.tr("Min") }
                            CompactField {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 32
                                enabled: !root.presentation.preferences[axisEditor.autoKey]
                                text: String(root.presentation.preferences[axisEditor.lowKey])
                                Accessible.name: root.tr("Minimum")
                                validator: DoubleValidator { locale: "C" }
                                onEditingFinished: { root.presentation.changePreference(axisEditor.lowKey, Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences[axisEditor.lowKey]) }) }
                            }
                            Label { text: root.tr("Max") }
                            CompactField {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 32
                                enabled: !root.presentation.preferences[axisEditor.autoKey]
                                text: String(root.presentation.preferences[axisEditor.highKey])
                                Accessible.name: root.tr("Maximum")
                                validator: DoubleValidator { locale: "C" }
                                onEditingFinished: { root.presentation.changePreference(axisEditor.highKey, Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences[axisEditor.highKey]) }) }
                            }
                        }
                        RowLayout {
                            visible: !axisEditor.xAxis
                            Layout.fillWidth: true
                            spacing: 4
                            Label { text: root.tr("Auto range interval (s)"); Layout.fillWidth: true; wrapMode: Text.Wrap }
                            CompactField {
                                Layout.preferredWidth: 48
                                Layout.maximumWidth: 64
                                enabled: root.presentation.preferences.auto_range
                                text: String(root.presentation.preferences.auto_range_interval_seconds)
                                Accessible.name: root.tr("Auto range interval (s)")
                                validator: DoubleValidator { bottom: 1; top: 60; locale: "C" }
                                onEditingFinished: { root.presentation.changePreference("auto_range_interval_seconds", Number(text)); text = Qt.binding(function() { return String(root.presentation.preferences.auto_range_interval_seconds) }) }
                            }
                        }
                    }
                }
            }
            Label { visible: (root.presentation.displayMode !== "metrics" && root.presentation.displayMode !== "signedbar") && root.presentation.displayMode !== "surface3d"; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.presentation.information }
            TextArea {
                objectName: "numericExactValue"
                visible: (root.presentation.displayMode !== "metrics" && root.presentation.displayMode !== "signedbar") && text.length > 0
                Layout.fillWidth: true
                readOnly: true
                wrapMode: Text.Wrap
                text: root.presentation.diagnostic
                selectByMouse: true
                padding: 0
                background: null
            }
            CompactGroupBox {
                visible: (root.presentation.displayMode === "metrics" || root.presentation.displayMode === "signedbar")
                title: root.tr("Inputs")
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Repeater {
                        model: root.presentation.scalarInputs
                        delegate: ColumnLayout {
                            id: metricRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 4
                            CompactField {
                                Layout.fillWidth: true
                                text: metricRow.modelData.name
                                placeholderText: root.tr("Value %1").arg(metricRow.modelData.port + 1)
                                onEditingFinished: { root.presentation.nameScalarInput(metricRow.modelData.port, text); text = Qt.binding(function() { return metricRow.modelData.name }) }
                            }
                            RowLayout {
                                spacing: 4
                                CompactCheckBox {
                                    text: root.tr("Show value")
                                    checked: metricRow.modelData.enabled
                                    onClicked: root.presentation.enableScalarInput(metricRow.modelData.port,checked)
                                }
                                CompactButton {
                                    text: root.tr("Remove input")
                                    enabled: metricRow.modelData.removeEnabled
                                    onClicked: root.presentation.removeScalarInput(metricRow.modelData.port)
                                }
                            }
                        }
                    }
                    CompactButton { text: root.tr("Add input"); enabled: root.presentation.canAddScalarInput; onClicked: root.presentation.addScalarInput() }
                }
            }
        }
    }
}
