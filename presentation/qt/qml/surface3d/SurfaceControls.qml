pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../controls"

ColumnLayout {
    id: root
    required property var surfaceModel
    property var state: surfaceModel.quickState
    spacing: 6
    function tr(s) { const revision = quickUi.revision; return quickUi.text(s) }

    component NumberField: CompactField {
        property real number: 0
        signal acceptedNumber(real value)
        text: Number(number).toLocaleString(Qt.locale(), 'g', 8)
        selectByMouse: true
        validator: DoubleValidator { bottom: -1000000; top: 1000000; notation: DoubleValidator.ScientificNotation }
        onEditingFinished: {
            const value = Number.fromLocaleString(Qt.locale(), text)
            if (isFinite(value)) acceptedNumber(value)
            text = Qt.binding(function() {return Number(number).toLocaleString(Qt.locale(), 'g', 8)})
        }
    }
    component AxisEditor: CompactGroupBox {
        id: axis
        property int axisIndex: 0
        property real minimum: 0
        property real maximum: 100
        property bool automatic: true
        property string reverseKey: "reverse_time"
        Layout.fillWidth: true
        function apply() {root.surfaceModel.quick_range(axisIndex, Number.fromLocaleString(Qt.locale(),minimumField.text), Number.fromLocaleString(Qt.locale(),maximumField.text), automaticBox.checked)}
        ColumnLayout {
            anchors.fill: parent
            spacing: 4
            RowLayout {
                CompactCheckBox {
                    id: automaticBox; text: root.tr("Auto"); checked: axis.automatic
                    onClicked: axis.apply()
                }
                Item {Layout.fillWidth: true}
                CompactCheckBox {
                    text: root.tr("Reverse"); checked: !!root.state.surface[axis.reverseKey]
                    onClicked: root.surfaceModel.quick_action(axis.reverseKey, checked)
                }
            }
            RowLayout {
                Label {text: root.tr("Min")}
                NumberField {
                    id: minimumField; Layout.fillWidth: true; Layout.minimumWidth: 0
                    number: axis.minimum; enabled: !automaticBox.checked
                    onAcceptedNumber: value => axis.apply()
                    Accessible.name: root.tr("Min")
                }
                Label {text: root.tr("Max")}
                NumberField {
                    id: maximumField; Layout.fillWidth: true; Layout.minimumWidth: 0
                    number: axis.maximum; enabled: !automaticBox.checked
                    onAcceptedNumber: value => axis.apply()
                    Accessible.name: root.tr("Max")
                }
            }
        }
    }
    CompactGroupBox {
        title: root.tr("Playback"); Layout.fillWidth: true
        CompactCheckBox {text: root.tr("Pause"); checked: !!root.state.pause; onClicked: root.surfaceModel.quick_action("pause",checked)}
    }
    CompactGroupBox {
        title: root.tr("Selection"); Layout.fillWidth: true
        ColumnLayout {
            spacing: 2
            Repeater {
                model: [root.tr("No selection"),root.tr("Item"),root.tr("Time Slice"),root.tr("Frequency Slice")]
                CompactRadioButton {required property int index;required property string modelData;text: modelData;checked: index === root.state.selection;onClicked: root.surfaceModel.quick_action("selection",index)}
            }
        }
    }
    CompactGroupBox {
        title: root.tr("Scene geometry"); Layout.fillWidth: true
        ColumnLayout {
            anchors.fill: parent; spacing: 4
            CompactComboBox {
                Layout.fillWidth: true
                model: [root.tr("Fixed scene proportions"),root.tr("Proportions from sample counts")]
                currentIndex: root.state.surface.scene_aspect_mode === "sample_counts" ? 1 : 0
                onActivated: root.surfaceModel.quick_action("scene_aspect_mode",currentIndex ? "sample_counts" : "fixed")
            }
            RowLayout {
                Label {text: root.tr("Time width / height");Layout.fillWidth: true;wrapMode: Text.Wrap}
                NumberField {Layout.preferredWidth: 72;Layout.minimumWidth: 0;number: root.state.surface.scene_time_aspect;enabled: root.state.surface.scene_aspect_mode === 'fixed';onAcceptedNumber: value => root.surfaceModel.quick_action("scene_time_aspect",value)}
            }
            RowLayout {
                Label {text: root.tr("Band depth / height");Layout.fillWidth: true;wrapMode: Text.Wrap}
                NumberField {Layout.preferredWidth: 72;Layout.minimumWidth: 0;number: root.state.surface.scene_band_aspect;enabled: root.state.surface.scene_aspect_mode === 'fixed';onAcceptedNumber: value => root.surfaceModel.quick_action("scene_band_aspect",value)}
            }
        }
    }
    Label {Layout.fillWidth: true;wrapMode: Text.Wrap;text: root.tr("History: %1 frames").replace("%1",root.state.historyFrames + " / " + root.state.historyCapacity)}
    AxisEditor {title: root.state.timeTitle;axisIndex: 0;minimum: root.state.ranges ? root.state.ranges[0] : 0;maximum: root.state.ranges ? root.state.ranges[1] : 100;automatic: root.state.horizontalAutomatic[0];reverseKey: 'reverse_time'}
    AxisEditor {title: root.state.valueTitle;axisIndex: 1;minimum: root.state.yMinimum;maximum: root.state.yMaximum;automatic: root.state.surface.y_range_mode !== 'manual';reverseKey: 'reverse_amplitude'}
    CompactGroupBox {
        title: root.tr("Auto range interval (s)"); Layout.fillWidth: true
        NumberField {width: parent.width;number: root.state.surface.auto_range_interval_seconds;enabled: root.state.surface.y_range_mode !== 'manual';onAcceptedNumber: value => root.surfaceModel.quick_action("auto_range_interval_seconds",value)}
    }
    AxisEditor {title: root.state.bandTitle;axisIndex: 2;minimum: root.state.ranges ? root.state.ranges[2] : 0;maximum: root.state.ranges ? root.state.ranges[3] : 100;automatic: root.state.horizontalAutomatic[1];reverseKey: 'reverse_bands'}
    CompactGroupBox {
        title: root.tr("Color scale"); Layout.fillWidth: true
        CompactComboBox {
            width: parent.width
            property var codes: ['green_red','black_yellow','viridis','ice_fire','rainbow','custom']
            model: [root.tr("Green / Red"),root.tr("Black / Yellow"),root.tr("Viridis"),root.tr("Ice / Fire"),root.tr("Rainbow"),root.tr("Custom")]
            currentIndex: root.state.surface.gradient_stops.length ? 5 : Math.max(0,codes.indexOf(root.state.surface.gradient))
            onActivated: root.surfaceModel.quick_action("gradient",codes[currentIndex])
        }
    }
    CompactGroupBox {
        title: root.tr("Camera"); Layout.fillWidth: true
        CompactButton {text: root.tr("Reset (Alt+R)");onClicked: root.surfaceModel.quick_action("resetCamera")}
    }
}
