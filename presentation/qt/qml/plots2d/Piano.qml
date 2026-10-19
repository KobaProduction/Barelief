pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: root
    required property var presentation
    required property var colors
    readonly property int keyCount: presentation.keys ? presentation.keys.length : 0
    Text {
        x: 12; y: 8; width: parent.width-24; height: 22
        text: root.presentation.title; font: root.presentation.font
        color: root.colors.windowText; textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    Text {
        x: 12; y: 30; width: parent.width-24; height: root.keyCount ? 20 : parent.height-42
        text: root.presentation.status; font: root.presentation.font
        color: root.colors.windowText; textFormat: Text.PlainText
        wrapMode: Text.Wrap
    }
    // A numeric count retains delegates between frames. Geometry is cached in
    // C++; only levels/colors/text that actually change dirty their scene nodes.
    Repeater {
        model: root.keyCount
        delegate: Item {
            id: note
            required property int index
            readonly property var key: root.presentation.keys[index]
            readonly property real level: root.presentation.levels[index] || 0
            x: key.x; y: key.y; width: key.width; height: key.height
            z: key.black ? 2 : 1
            Rectangle {
                x: (parent.width-Math.max(2,parent.width*.5))/2
                y: -8-height
                width: Math.max(2,parent.width*.5)
                height: Math.max(0,note.key.y-58)*note.level
                color: root.colors.highlight
            }
            Rectangle {
                anchors.fill: parent
                color: note.key.black ? root.presentation.blackKey : root.presentation.whiteKey
                border.color: root.colors.mid; border.width: 1
                Rectangle {
                    anchors.fill: parent; anchors.margins: 1
                    color: root.colors.highlight; opacity: note.level*.9
                }

            }
        }
    }
    Repeater {
        model: root.keyCount
        delegate: Text {
            required property int index
            readonly property var key: root.presentation.keys[index]
            visible: key.label.length > 0
            x: key.x+(key.width-width)/2; y: key.y+key.height*.7
            height: key.height*.3-3; z: 3
            text: key.label; font: root.presentation.font; color: root.presentation.keyText
            textFormat: Text.PlainText; verticalAlignment: Text.AlignVCenter
        }
    }
    Text {
        x: 12; y: parent.height-25; width: parent.width-24; height: 20
        text: root.presentation.peakText; font: root.presentation.font
        color: root.colors.windowText; textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
}
