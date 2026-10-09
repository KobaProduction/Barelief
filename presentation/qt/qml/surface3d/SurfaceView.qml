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
        if (side && split && !split.resizing)
            side.SplitView.preferredWidth = split.width * panelRatio
    }
    onPanelRatioChanged: updatePanelWidth()
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

    component NumberField: TextField {
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
    component AxisEditor: GroupBox {
        id: axis
        property int axisIndex: 0
        property real minimum: 0
        property real maximum: 100
        property bool automatic: true
        property string reverseKey: "reverse_time"
        Layout.fillWidth: true
        function apply() {graphModel.quick_range(axisIndex, Number.fromLocaleString(Qt.locale(),minimumField.text), Number.fromLocaleString(Qt.locale(),maximumField.text), automaticBox.checked)}
        ColumnLayout {
            anchors.fill: parent
            Label { text: axis.axisIndex === 1 ? root.tr("Amplitude (Y)") : root.tr("Range (%)"); Layout.fillWidth: true; wrapMode: Text.Wrap }
            CheckBox {
                id: automaticBox; text: root.tr("Auto"); checked: axis.automatic
                onClicked: axis.apply()
            }
            RowLayout {
                NumberField {
                    id: minimumField; Layout.fillWidth: true; number: axis.minimum; enabled: !automaticBox.checked
                    onAcceptedNumber: value => axis.apply()
                    Accessible.name: root.tr("Min")
                }
                NumberField {
                    id: maximumField; Layout.fillWidth: true; number: axis.maximum; enabled: !automaticBox.checked
                    onAcceptedNumber: value => axis.apply()
                    Accessible.name: root.tr("Max")
                }
            }
            CheckBox {
                text: root.tr("Reverse"); checked: !!root.state.surface[axis.reverseKey]
                onClicked: graphModel.quick_action(axis.reverseKey, checked)
            }
        }
    }
    SplitView {
        id: split
        anchors.fill: parent
        orientation: Qt.Horizontal
        onWidthChanged: root.updatePanelWidth()
        onResizingChanged: if (!resizing && width > 0) graphModel.quick_action("panelRatio", side.width / width)
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
            Canvas {
                id: overlay
                anchors.fill: parent
                property var commands: graphModel.overlay.commands || []
                onCommandsChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                function color(value) {
                    if (value && value.length === 9 && value[0] === '#')
                        return 'rgba('+parseInt(value.slice(3,5),16)+','+parseInt(value.slice(5,7),16)+','+parseInt(value.slice(7,9),16)+','+(parseInt(value.slice(1,3),16)/255)+')'
                    return value
                }
                onPaint: {
                    const ctx = getContext('2d'); ctx.clearRect(0,0,width,height)
                    for (const command of commands) {
                        const c=command, b=c.brush, t=c.transform
                        ctx.save()
                        if (c.clip.width > 0 && c.clip.height > 0) {ctx.beginPath();ctx.rect(c.clip.x,c.clip.y,c.clip.width,c.clip.height);ctx.clip()}
                        ctx.transform(t[0],t[1],t[2],t[3],t[4],t[5])
                        ctx.strokeStyle=color(c.pen);ctx.lineWidth=c.penWidth > 0 ? c.penWidth : 1
                        if (b.stops) {
                            const gradient=ctx.createLinearGradient(b.start.x,b.start.y,b.end.x,b.end.y)
                            for (const stop of b.stops) gradient.addColorStop(stop.position,color(stop.color))
                            ctx.fillStyle=gradient
                        } else ctx.fillStyle=color(b.color)
                        ctx.beginPath()
                        if(c.kind === 'line'){ctx.moveTo(c.points[0],c.points[1]);ctx.lineTo(c.points[2],c.points[3])}
                        else if(c.kind === 'rect')ctx.rect(c.x,c.y,c.width,c.height)
                        else if(c.kind === 'roundedRect')ctx.roundedRect(c.x,c.y,c.width,c.height,c.radiusX,c.radiusY)
                        else if(c.kind === 'ellipse')ctx.ellipse(c.x,c.y,c.width,c.height)
                        else if(c.kind === 'polygon'){
                            if(c.points.length){ctx.moveTo(c.points[0],c.points[1]);for(let i=2;i<c.points.length;i+=2)ctx.lineTo(c.points[i],c.points[i+1]);ctx.closePath()}
                        } else if(c.kind === 'text'){
                            ctx.fillStyle=color(c.pen);ctx.font=(c.fontBold ? 'bold ' : '')+c.fontPixels+'px "'+c.font+'"'
                            ctx.textBaseline=c.baseline ? 'alphabetic' : 'top';ctx.textAlign='left'
                            let x=c.x,y=c.y
                            if(!c.baseline){
                                if(c.flags & Qt.AlignHCenter){x+=c.width/2;ctx.textAlign='center'}
                                else if(c.flags & Qt.AlignRight){x+=c.width;ctx.textAlign='right'}
                                if(c.flags & Qt.AlignVCenter){y+=c.height/2;ctx.textBaseline='middle'}
                                else if(c.flags & Qt.AlignBottom){y+=c.height;ctx.textBaseline='bottom'}
                            }
                            if(c.flags & Qt.TextWordWrap){
                                const words=c.text.split(' ');let line='',lineY=y
                                for(const word of words){const next=line ? line+' '+word : word;if(line && ctx.measureText(next).width>c.width){ctx.fillText(line,x,lineY);lineY+=c.fontPixels*1.2;line=word}else line=next}
                                if(line)ctx.fillText(line,x,lineY)
                            }else ctx.fillText(c.text,x,y)
                        }
                        if(c.kind !== 'text'){if(!b.none)ctx.fill();if(!c.noPen)ctx.stroke()}
                        ctx.restore()
                    }
                }
            }
        }
        ScrollView {
            id: side
            SplitView.preferredWidth: split.width * root.panelRatio
            SplitView.minimumWidth: 180
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: side.availableWidth; spacing: 8
                GroupBox {
                    title: root.tr("Playback"); Layout.fillWidth: true
                    CheckBox {text: root.tr("Pause"); checked: !!root.state.pause; onClicked: graphModel.quick_action("pause",checked)}
                }
                GroupBox {
                    title: root.tr("Selection"); Layout.fillWidth: true
                    ColumnLayout {
                        Repeater {
                            model: [root.tr("No selection"),root.tr("Item"),root.tr("Time Slice"),root.tr("Frequency Slice")]
                            RadioButton {required property int index;required property string modelData;text: modelData;checked: index === root.state.selection;onClicked: graphModel.quick_action("selection",index)}
                        }
                    }
                }
                GroupBox {
                    title: root.tr("Scene geometry"); Layout.fillWidth: true
                    ColumnLayout {
                        anchors.fill: parent
                        ComboBox {
                            Layout.fillWidth: true
                            model: [root.tr("Fixed scene proportions"),root.tr("Proportions from sample counts")]
                            currentIndex: root.state.surface.scene_aspect_mode === "sample_counts" ? 1 : 0
                            onActivated: graphModel.quick_action("scene_aspect_mode",currentIndex ? "sample_counts" : "fixed")
                        }
                        Label {text: root.tr("Time width / height")}
                        NumberField {Layout.fillWidth: true;number: root.state.surface.scene_time_aspect;enabled: root.state.surface.scene_aspect_mode === 'fixed';onAcceptedNumber: value => graphModel.quick_action("scene_time_aspect",value)}
                        Label {text: root.tr("Band depth / height")}
                        NumberField {Layout.fillWidth: true;number: root.state.surface.scene_band_aspect;enabled: root.state.surface.scene_aspect_mode === 'fixed';onAcceptedNumber: value => graphModel.quick_action("scene_band_aspect",value)}
                    }
                }
                Label {Layout.fillWidth: true;wrapMode: Text.Wrap;text: root.tr("History: %1 frames").replace("%1",root.state.historyFrames + " / " + root.state.historyCapacity)}
                AxisEditor {title: root.state.timeTitle;axisIndex: 0;minimum: root.state.ranges ? root.state.ranges[0] : 0;maximum: root.state.ranges ? root.state.ranges[1] : 100;automatic: root.state.horizontalAutomatic[0];reverseKey: 'reverse_time'}
                AxisEditor {title: root.state.valueTitle;axisIndex: 1;minimum: root.state.yMinimum;maximum: root.state.yMaximum;automatic: root.state.surface.y_range_mode !== 'manual';reverseKey: 'reverse_amplitude'}
                GroupBox {
                    title: root.tr("Auto range interval (s)"); Layout.fillWidth: true
                    NumberField {width: parent.width;number: root.state.surface.auto_range_interval_seconds;enabled: root.state.surface.y_range_mode !== 'manual';onAcceptedNumber: value => graphModel.quick_action("auto_range_interval_seconds",value)}
                }
                AxisEditor {title: root.state.bandTitle;axisIndex: 2;minimum: root.state.ranges ? root.state.ranges[2] : 0;maximum: root.state.ranges ? root.state.ranges[3] : 100;automatic: root.state.horizontalAutomatic[1];reverseKey: 'reverse_bands'}
                GroupBox {
                    title: root.tr("Color scale"); Layout.fillWidth: true
                    ComboBox {
                        width: parent.width
                        property var codes: ['green_red','black_yellow','viridis','ice_fire','rainbow','custom']
                        model: [root.tr("Green / Red"),root.tr("Black / Yellow"),root.tr("Viridis"),root.tr("Ice / Fire"),root.tr("Rainbow"),root.tr("Custom")]
                        currentIndex: root.state.surface.gradient_stops.length ? 5 : Math.max(0,codes.indexOf(root.state.surface.gradient))
                        onActivated: graphModel.quick_action("gradient",codes[currentIndex])
                    }
                }
                GroupBox {
                    title: root.tr("Camera"); Layout.fillWidth: true
                    Button {text: root.tr("Reset (Alt+R)");onClicked: graphModel.quick_action("resetCamera")}
                }
            }
        }
    }
}
