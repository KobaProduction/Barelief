import QtQuick

Rectangle {
    id: root
    property var s: graphModel.state
    property var colors: quickUi.palette
    color: colors.base
    clip: true
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
        x: root.s.plotLeft || 82; y: root.s.scalar === true ? 70 : 28
        width: Math.max(1, root.width - x - 22)
        height: Math.max(1, root.height - y - 58)
        visible: root.s.kind === "vector" || root.s.kind === "image"
        Rectangle {anchors.fill: parent; color: root.colors.base; border.color: root.colors.mid}
        Canvas {
            id: vectorCanvas
            anchors.fill: parent
            visible: root.s.kind === "vector"
            renderStrategy: Canvas.Immediate
            onPaint: {
                const c = getContext("2d")
                c.clearRect(0,0,width,height)
                c.save(); c.beginPath(); c.rect(0,0,width,height); c.clip()
                c.strokeStyle = root.colors.mid; c.globalAlpha = 0.3; c.lineWidth = 0.75
                const xt = root.s.xTicks || [], yt = root.s.yTicks || []
                c.beginPath()
                for(let i=0;i<xt.length;i++){const x=xt[i].position*width;c.moveTo(x,0);c.lineTo(x,height)}
                for(let j=0;j<yt.length;j++){const y=yt[j].position*height;c.moveTo(0,y);c.lineTo(width,y)}
                c.stroke(); c.globalAlpha=1
                const points=root.s.points || []
                c.strokeStyle=root.colors.highlight; c.fillStyle=root.colors.highlight; c.lineWidth=1.25
                c.beginPath()
                if(root.s.style === "bars") {
                    const base=root.s.baseline*height
                    for(let k=0;k<points.length;k++){const x=points[k].x*width;c.moveTo(x,base);c.lineTo(x,points[k].y*height)}
                    c.stroke()
                } else if(root.s.style === "points" || points.length===1) {
                    for(let k=0;k<points.length;k++){const x=points[k].x*width,y=points[k].y*height;c.moveTo(x+3,y);c.arc(x,y,3,0,2*Math.PI)}
                    c.fill()
                } else {
                    for(let k=0;k<points.length;k++){const x=points[k].x*width,y=points[k].y*height;if(k===0)c.moveTo(x,y);else c.lineTo(x,y)}
                    c.stroke()
                }
                c.restore()
            }
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
            model: root.s.xTicks || []
            Text {
                required property var modelData
                x: modelData.position*plot.width-width/2; y: plot.height+5
                text: modelData.text; color: root.colors.text; textFormat: Text.PlainText
            }
        }
        Repeater {
            model: root.s.yTicks || []
            Text {
                required property var modelData
                x: -width-8; y: modelData.position*plot.height-height/2
                text: modelData.text; color: root.colors.text; textFormat: Text.PlainText
            }
        }
        Text {
            x:0;y:plot.height+28;width:plot.width
            text:root.s.xTitle || "";color:root.colors.text;horizontalAlignment:Text.AlignHCenter;elide:Text.ElideRight;textFormat:Text.PlainText
        }
        Text {
            x:-plot.height/2-plot.x+16;y:plot.height/2-height/2
            width:plot.height;rotation:-90
            text:root.s.yTitle || "";color:root.colors.text;horizontalAlignment:Text.AlignHCenter;elide:Text.ElideRight;textFormat:Text.PlainText
        }
    }
    Canvas {
        id: pieCanvas
        x:8;y:28;width:Math.max(1,parent.width-16);height:Math.max(1,parent.height-36)
        visible:root.s.kind === "pie"
        onPaint: {
            const c=getContext("2d");c.clearRect(0,0,width,height)
            const slices=root.s.slices || [], cx=width/2,cy=height/2,r=Math.max(1,Math.min(width,height)*0.34)
            for(let i=0;i<slices.length;i++) {
                const start=slices[i].start*2*Math.PI-Math.PI/2, end=start+slices[i].fraction*2*Math.PI
                const mix=0.35+0.65*i/Math.max(1,slices.length-1), a=root.colors.highlight,b=root.colors.base
                c.fillStyle=Qt.rgba(b.r+(a.r-b.r)*mix,b.g+(a.g-b.g)*mix,b.b+(a.b-b.b)*mix,1)
                c.strokeStyle=root.colors.mid;c.lineWidth=1
                c.beginPath();c.moveTo(cx,cy);c.arc(cx,cy,r,start,end);c.closePath();c.fill();c.stroke()
                if(slices.length<=12 && slices[i].fraction>0){
                    const mid=(start+end)/2;c.fillStyle=root.colors.text;c.font="12px sans-serif"
                    c.textAlign=Math.cos(mid)<0?"right":"left";c.textBaseline="middle"
                    c.fillText(slices[i].label,cx+Math.cos(mid)*(r+10),cy+Math.sin(mid)*(r+10))
                }
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
                    required property var modelData
                    width:metricColumn.width;spacing:12
                    Text {
                        width:Math.max(30,parent.width*0.4);text:parent.modelData.name
                        textFormat:Text.PlainText;color:root.colors.text;wrapMode:Text.Wrap
                    }
                    TextEdit {
                        width:Math.max(30,parent.width*0.6-12);text:parent.modelData.exact
                        objectName:"metricValue_"+parent.modelData.port
                        textFormat:TextEdit.PlainText;color:root.colors.text;readOnly:true;selectByMouse:true;wrapMode:TextEdit.Wrap
                    }
                }
            }
        }
    }
    Connections {
        target:graphModel
        function onChanged(){vectorCanvas.requestPaint();pieCanvas.requestPaint()}
    }
    Connections {
        target:quickUi
        function onChanged(){vectorCanvas.requestPaint();pieCanvas.requestPaint()}
    }
    onWidthChanged:{vectorCanvas.requestPaint();pieCanvas.requestPaint()}
    onHeightChanged:{vectorCanvas.requestPaint();pieCanvas.requestPaint()}
}
