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
    // Disclosure state belongs to the sidebar, not the processing graph.
    property bool axisXOpen: false
    property bool axisYOpen: false
    property bool scaleOpen: false
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
            Label { visible: root.presentation.modes.length > 1; text: root.tr("Display mode"); Layout.fillWidth: true }
            CompactComboBox {
                objectName: "numericDisplayMode"
                Layout.fillWidth: true
                visible: root.presentation.modes.length > 1
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
                Label { text: root.tr("Column axis") }
                CompactComboBox {
                    objectName: "frequencyAxisMode"
                    Layout.fillWidth: true
                    model: [root.tr(root.presentation.physicalFrequency ? "Source coordinates" : "Coordinates unavailable"), root.tr("Positions")]
                    currentIndex: root.presentation.preferences.frequency_axis_mode === "positions" ? 1 : 0
                    itemEnabled: function(index) { return index > 0 || root.presentation.physicalFrequency }
                    onActivated: (index) => root.presentation.changePreference("frequency_axis_mode", index === 0 ? "coordinates" : "positions")
                }
                Label { text: root.tr("Row axis") }
                CompactComboBox {
                    objectName: "timeAxisMode"
                    Layout.fillWidth: true
                    model: [root.tr(root.presentation.physicalTime ? "Source coordinates" : "Coordinates unavailable"), root.tr("Rows")]
                    currentIndex: root.presentation.preferences.time_axis_mode === "positions" ? 1 : 0
                    itemEnabled: function(index) { return index > 0 || root.presentation.physicalTime }
                    onActivated: (index) => root.presentation.changePreference("time_axis_mode", index === 0 ? "coordinates" : "positions")
                }
            }
            ColumnLayout {
                visible: root.presentation.audioScales && root.presentation.displayMode !== "piano" && root.presentation.displayMode !== "metrics" && root.presentation.displayMode !== "signedbar" && root.presentation.displayMode !== "pie"
                Layout.fillWidth: true
                CompactButton {
                    objectName: "valueScaleMenu"
                    Layout.fillWidth: true
                    text: (root.scaleOpen ? "▾ " : "▸ ") + root.tr("Value scale")
                    onClicked: root.scaleOpen = !root.scaleOpen
                }
                ColumnLayout {
                    visible: root.scaleOpen
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                CompactComboBox {
                    objectName:"valueScale";Layout.fillWidth:true
                    model:[root.tr("Source values"),root.tr("Linear values"),root.tr("Decibels")]
                    currentIndex:["source","linear","db"].indexOf(root.presentation.preferences.value_scale)
                    onActivated: (index) => root.presentation.changePreference("value_scale",["source","linear","db"][index])
                }
                ColumnLayout {
                    visible:root.presentation.preferences.value_scale !== "source"
                    Layout.fillWidth:true
                    Label {text:root.tr("dB conversion");Layout.fillWidth:true}
                    CompactComboBox {
                        objectName:"dbFactor";Layout.fillWidth:true
                        model:[root.tr("Auto (source quantity)"),root.tr("Amplitude (20 log10)"),root.tr("Power / PSD (10 log10)")]
                        currentIndex:[0,20,10].indexOf(root.presentation.preferences.db_factor)
                        onActivated:(index)=>root.presentation.changePreference("db_factor",[0,20,10][index])
                    }
                    Repeater {
                        model:[{key:"db_reference",label:"dB reference",low:1e-30,high:1e30},{key:"db_floor",label:"dB floor",low:-300,high:-0.001}]
                        delegate:RowLayout {
                            id:dbEditor;required property var modelData;Layout.fillWidth:true
                            Label {text:root.tr(dbEditor.modelData.label);Layout.fillWidth:true;wrapMode:Text.Wrap}
                            CompactField {
                                objectName:dbEditor.modelData.key;Layout.preferredWidth:64
                                text:String(root.presentation.preferences[dbEditor.modelData.key])
                                validator:DoubleValidator {bottom:dbEditor.modelData.low;top:dbEditor.modelData.high;locale:"C"}
                                onEditingFinished:{root.presentation.changePreference(dbEditor.modelData.key,Number(text));text=Qt.binding(function(){return String(root.presentation.preferences[dbEditor.modelData.key])})}
                            }
                        }
                    }
                    Label {text:root.tr("Input dB is not logged again. For conversion to linear, set the reference used by the source.");Layout.fillWidth:true;wrapMode:Text.Wrap}
                }
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
                       root.presentation.displayMode === "bars" ? [{key:"bar_width_percent",label:"Bar width (%)",low:1,high:100}] :
                       root.presentation.displayMode === "piano" ? [{key:"piano_activity_floor",label:"Activity floor (input units)",low:0,high:1e30}] : []
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
                model: (root.presentation.displayMode === "metrics" || root.presentation.displayMode === "signedbar") || root.presentation.displayMode === "surface3d" || root.presentation.displayMode === "pie" || root.presentation.displayMode === "piano" ? [] : ["x", "y"]
                delegate: ColumnLayout {
                    id: axisEditor
                    required property string modelData
                    property bool xAxis: modelData === "x"
                    property string lowKey: xAxis ? "minimum_x" : "minimum"
                    property string highKey: xAxis ? "maximum_x" : "maximum"
                    property string autoKey: xAxis ? "auto_range_x" : "auto_range"
                    Layout.fillWidth: true
                    CompactButton {
                        objectName: axisEditor.xAxis ? "axisMenuX" : "axisMenuY"
                        Layout.fillWidth: true
                        text: ((axisEditor.xAxis ? root.axisXOpen : root.axisYOpen) ? "▾ " : "▸ ") + root.tr(axisEditor.xAxis ? "Axis X" : "Axis Y")
                        onClicked: {
                            if(axisEditor.xAxis) root.axisXOpen = !root.axisXOpen
                            else root.axisYOpen = !root.axisYOpen
                        }
                    }
                    ColumnLayout {
                        objectName: axisEditor.xAxis ? "axisSettingsX" : "axisSettingsY"
                        visible: axisEditor.xAxis ? root.axisXOpen : root.axisYOpen
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
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
                        Repeater {
                            model:[{key:axisEditor.xAxis?"grid_lines_x":"grid_lines_y",label:axisEditor.xAxis?"Vertical grid lines":"Horizontal grid lines"},{key:axisEditor.xAxis?"axis_labels_x":"axis_labels_y",label:axisEditor.xAxis?"X labels":"Y labels"}]
                            delegate:RowLayout {
                                id:densityEditor;required property var modelData;Layout.fillWidth:true
                                Label {text:root.tr(densityEditor.modelData.label);Layout.fillWidth:true;wrapMode:Text.Wrap}
                                CompactField {
                                    objectName:densityEditor.modelData.key;Layout.preferredWidth:48
                                    text:String(root.presentation.preferences[densityEditor.modelData.key])
                                    validator:IntValidator {bottom:-1;top:128}
                                    onEditingFinished:{root.presentation.changePreference(densityEditor.modelData.key,Number(text));text=Qt.binding(function(){return String(root.presentation.preferences[densityEditor.modelData.key])})}
                                }
                            }
                        }
                        Label {text:root.tr("0 = auto; -1 = hidden; 1–128 = count (including edges)");Layout.fillWidth:true;wrapMode:Text.Wrap}
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
            Label { visible: root.presentation.valueLayout.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; text: root.presentation.valueLayout }
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
