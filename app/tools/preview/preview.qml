import QtQuick 2.6

// Preview shell: a control bar plus a frame that renders the real car UI
// (carMainUrl -> carqml/CarMain.qml) at a chosen head-unit resolution, scaled
// to fit. carScreen.nightMode is provided by the C++ launcher.
Rectangle {
    id: shell
    color: "#1b1e24"

    // selected head-unit resolution (CarMain reflows off the aspect ratio)
    property int rw: 800
    property int rh: 480

    readonly property var presets: [
        { label: "800 × 480",   w: 800,  h: 480  },
        { label: "1280 × 720",  w: 1280, h: 720  },
        { label: "1920 × 1080", w: 1920, h: 1080 },
        { label: "720 × 1280",  w: 720,  h: 1280 }
    ]

    // ---- control bar -------------------------------------------------------
    Rectangle {
        id: bar
        height: 52
        anchors { left: parent.left; right: parent.right; top: parent.top }
        color: "#11141a"

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 12
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Resolution:"; color: "#8a97a6"; font.pixelSize: 14
            }

            Repeater {
                model: shell.presets
                Rectangle {
                    width: lbl.width + 22; height: 34; radius: 6
                    property bool sel: shell.rw === modelData.w && shell.rh === modelData.h
                    color: sel ? "#4c8dff" : "#232935"
                    Text {
                        id: lbl; anchors.centerIn: parent; text: modelData.label
                        color: parent.sel ? "white" : "#c4cfdb"; font.pixelSize: 14
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: { shell.rw = modelData.w; shell.rh = modelData.h }
                    }
                }
            }
        }

        // day / night toggle
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 12
            width: nl.width + 26; height: 34; radius: 6
            color: carScreen.nightMode ? "#0b0f14" : "#f0c419"
            border.color: "#3a4452"; border.width: 1
            Text {
                id: nl; anchors.centerIn: parent
                text: carScreen.nightMode ? "Night mode" : "Day mode"
                color: carScreen.nightMode ? "#eef3f8" : "#1b1e24"; font.pixelSize: 14
            }
            MouseArea { anchors.fill: parent; onClicked: carScreen.nightMode = !carScreen.nightMode }
        }
    }

    // ---- scaled preview frame ---------------------------------------------
    Item {
        id: stage
        anchors { left: parent.left; right: parent.right; top: bar.bottom; bottom: parent.bottom }
        clip: true

        Rectangle {
            id: frame
            width: shell.rw
            height: shell.rh
            anchors.centerIn: parent
            color: "black"
            scale: Math.min((stage.width - 32) / width, (stage.height - 32) / height)
            antialiasing: true

            Loader {
                id: carLoader
                objectName: "carLoader"
                anchors.fill: parent
                source: carMainUrl
                onLoaded: {
                    if (typeof previewPane !== "undefined" && previewPane.length && item)
                        item.activeApp = previewPane
                }
            }

            // thin bezel so the panel edges are visible against the backdrop
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: "#3a4452"
                border.width: 1 / parent.scale
            }
        }

        Text {
            anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; bottomMargin: 8 }
            text: shell.rw + " × " + shell.rh + "   ·   click the UI to interact"
            color: "#5a6b7b"; font.pixelSize: 12
        }
    }
}
