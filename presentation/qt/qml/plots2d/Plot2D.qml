import QtQuick
import Numeric.Graphs 1.0

Rectangle {
    id: root
    property var s: graphModel.state
    property var axes: graphModel.axes
    property var colors: quickUi.palette
    FontMetrics { id: labelMetrics; font: caption.font }
    color: colors.base
    clip: true
    function requestVisiblePaints() {
        if(vectorCanvas.visible)vectorCanvas.requestPaint()
        if(pieCanvas.visible)pieCanvas.requestPaint()
    }
    // All labels arrive as C++ strings. Exact scalar values never enter JS arithmetic.
    Text {
        id: caption
        x: 8; y: 2; width: parent.width - 16; height: 22
        text: root.s.title || ""
        textFormat: Text.PlainText; color: root.colors.text
        elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
    }
    TextEdit {
        id: scalar
        x: 8; y: 27; width: parent.width - 16; height: 34
        visible: root.s.scalar === true && root.s.scalarAvailable === true
        text: root.s.exact || ""
        color: root.colors.text; font.bold: true; font.pixelSize: 24
        textFormat: TextEdit.PlainText; readOnly: true; selectByMouse: true
        horizontalAlignment: TextEdit.AlignHCenter; wrapMode: TextEdit.Wrap
    }
    Item {
        id: plot
        x: root.axes.plotLeft || 82; y: root.s.scalar === true ? 70 : 28
        width: Math.max(1, root.width - x - 22)
        height: Math.max(1, root.height - y - 58)
        visible: root.s.kind === "vector" || root.s.kind === "image"
        Rectangle {anchors.fill: parent; color: root.colors.base; border.color: root.colors.mid}
        VectorGeometry {
            id: vectorCanvas
            objectName: "numericVectorPlot"
            anchors.fill: parent
            visible: root.s.kind === "vector"
            sourceModel: visible ? graphModel : null
            colors: root.colors
        }
        Item {
            anchors.fill: parent; clip: true
            visible: root.s.kind === "image" && root.s.available === true
            Image {
                objectName: "numericRaster"
                anchors.fill: parent
                source: root.s.available === true ? (root.s.imageSource || "") : ""
                cache: false; smooth: false; asynchronous: false
                transform: Scale {origin.x: plot.width/2;origin.y: plot.height/2;xScale: root.s.reverseX ? -1 : 1;yScale: root.s.reverseY ? -1 : 1}
            }
        }
        Text {
            anchors.centerIn: parent; width: parent.width
            horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
            color: root.colors.text
            visible: root.s.kind === "image" && root.s.available !== true
            text: {const revision=quickUi.revision; return quickUi.text("Renderer projection unavailable")}
        }
        Repeater {
            model: root.axes.xTicks || []
            Text {
                required property var modelData
                x: modelData.position*plot.width-width/2; y: plot.height+5
                text: modelData.text; color: root.colors.text; textFormat: Text.PlainText
            }
        }
        Repeater {
            model: root.axes.yTicks || []
            Text {
                required property var modelData
                x: -width-8; y: modelData.position*plot.height-height/2
                text: modelData.text; color: root.colors.text; textFormat: Text.PlainText
            }
        }
        Text {
            x:0;y:plot.height+28;width:plot.width
            text:root.axes.xTitle || "";color:root.colors.text;horizontalAlignment:Text.AlignHCenter;elide:Text.ElideRight;textFormat:Text.PlainText
        }
        Text {
            x:-plot.height/2-plot.x+16;y:plot.height/2-height/2
            width:plot.height;rotation:-90
            text:root.axes.yTitle || "";color:root.colors.text;horizontalAlignment:Text.AlignHCenter;elide:Text.ElideRight;textFormat:Text.PlainText
        }
    }
    VectorPlot {
        id: pieCanvas
        objectName: "numericPiePlot"
        x:8;y:28;width:Math.max(1,parent.width-16);height:Math.max(1,parent.height-36)
        visible:root.s.kind === "pie"
        onVisibleChanged: if(visible)requestPaint()
        readonly property real labelHeight: labelMetrics.height + 4
        readonly property real labelWidth: Math.min(width*0.26, longestLabel()+4)
        readonly property real radius: Math.max(1,Math.min(height*0.34,(width-2*(labelWidth+24))/2))
        readonly property var labelLayout: layoutLabels()
        function longestLabel() {
            const slices=root.s.slices || []
            let longest=0
            for(let i=0;i<slices.length;i++)longest=Math.max(longest,labelMetrics.advanceWidth(slices[i].label))
            return longest
        }
        function layoutLabels() {
            const slices=root.s.slices || [], left=[],right=[]
            // As before, dense pies retain their slices without an unreadable
            // label cloud. The input snapshot itself remains unchanged.
            if(root.s.showSliceLabels === false || slices.length>12)return []
            // Reserve both label columns and the leader gaps before placing
            // anything. Tiny viewports still draw the pie, without clipped text.
            if(width<2*(labelWidth+24)+2 || height<labelHeight)return []
            for(let i=0;i<slices.length;i++)if(slices[i].fraction>0){
                const angle=(slices[i].start+slices[i].fraction/2)*2*Math.PI-Math.PI/2
                const isLeft=Math.cos(angle)<0
                const label={text:slices[i].label,left:isLeft,
                    anchorX:width/2+Math.cos(angle)*radius,anchorY:height/2+Math.sin(angle)*radius,
                    y:height/2+Math.sin(angle)*(radius+16),
                    x:isLeft?width/2-radius-18-labelWidth:width/2+radius+18}
                ;(isLeft?left:right).push(label)
            }
            // The collision passes require enough space for every label on a
            // side. Suppress the label layer instead of pushing its top rows
            // above the canvas when the viewport/font budget cannot fit them.
            if(Math.max(left.length,right.length)*labelHeight>height)return []
            for(const side of [left,right]){
                side.sort(function(a,b){return a.y-b.y})
                for(let i=0;i<side.length;i++)side[i].y=Math.max(labelHeight/2,side[i].y,i?side[i-1].y+labelHeight:0)
                for(let i=side.length-1;i>=0;i--)side[i].y=Math.min(height-labelHeight/2,side[i].y,i+1<side.length?side[i+1].y-labelHeight:height)
            }
            return left.concat(right)
        }
        onLabelLayoutChanged: if(visible)requestPaint()
        presentation: visible ? root.s : ({})
        colors: root.colors
        pieRadius: radius
        pieLabelWidth: labelWidth
        pieLabels: labelLayout
        Repeater {
            model:pieCanvas.labelLayout
            Text {
                required property var modelData
                x:modelData.x;y:modelData.y-height/2;width:pieCanvas.labelWidth
                text:modelData.text;font:caption.font;color:root.colors.text
                horizontalAlignment:modelData.left?Text.AlignRight:Text.AlignLeft
                textFormat:Text.PlainText;elide:Text.ElideRight
            }
        }
    }
    Flickable {
        anchors.fill: parent; anchors.margins: 12
        visible: root.s.kind === "metrics"
        clip: true; contentHeight: metricColumn.height; contentWidth: width
        Column {
            id: metricColumn
            width: parent.width; spacing: 12
            Repeater {
                model:root.s.metrics || []
                Row {
                    required property int metricPort
                    required property string metricName
                    required property string metricExact
                    required property real metricBar
                    width:metricColumn.width;spacing:12
                    Text {
                        width:Math.max(30,parent.width*0.4);text:parent.metricName
                        textFormat:Text.PlainText;color:root.colors.text;wrapMode:Text.Wrap
                    }
                    Item {
                        visible:root.s.signedBar === true
                        width:visible?120:0;height:100
                        Rectangle { x:0;y:49;width:parent.width;height:1;color:root.colors.text }
                        Rectangle { x:parent.width*0.3;width:parent.width*0.4;y:parent.parent.metricBar>=0?50-parent.parent.metricBar*50:50;height:Math.abs(parent.parent.metricBar)*50;color:root.colors.accent || root.colors.text }
                    }
                    TextEdit {
                        width:Math.max(30,parent.width*0.6-12-(root.s.signedBar?132:0));text:parent.metricExact
                        objectName:"metricValue_"+parent.metricPort
                        textFormat:TextEdit.PlainText;color:root.colors.text;readOnly:true;selectByMouse:true;wrapMode:TextEdit.Wrap
                    }
                }
            }
        }
    }
    Connections {
        target:graphModel
        function onChanged(){root.requestVisiblePaints()}
    }
    Connections {
        target:quickUi
        function onChanged(){root.requestVisiblePaints()}
    }
    onWidthChanged:requestVisiblePaints()
    onHeightChanged:requestVisiblePaints()
}
