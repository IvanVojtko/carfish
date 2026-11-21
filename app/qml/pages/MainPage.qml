import QtQuick 2.0
import Sailfish.Silica 1.0

Page {
    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Check certificate")
                enabled: !controller.running
                onClicked: controller.refreshCertStatus()
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: "Carfish" }

            // Primary action: start/stop projection, right on the main screen.
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                preferredWidth: Theme.buttonWidthLarge
                text: controller.running ? qsTr("Stop") : qsTr("Start")
                onClicked: controller.running ? controller.stop()
                                              : controller.start(modeBox.currentIndex)
            }

            ComboBox {
                id: modeBox
                enabled: !controller.running
                label: qsTr("Connection")
                currentIndex: controller.mode
                menu: ContextMenu {
                    MenuItem { text: qsTr("Wireless (Bluetooth + WiFi)") }
                    MenuItem { text: qsTr("USB (Android Open Accessory)") }
                    MenuItem { text: qsTr("TCP server :5277 (testing)") }
                }
            }

            TextField {
                width: parent.width
                enabled: !controller.running
                label: qsTr("Certificate chain (PEM)")
                text: controller.certPath
                onTextChanged: controller.certPath = text
            }

            TextField {
                width: parent.width
                enabled: !controller.running
                label: qsTr("Private key (PEM)")
                text: controller.keyPath
                onTextChanged: controller.keyPath = text
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: controller.certStatus.indexOf("Ready") === 0 ? "#34c759" : "#ff6b5b"
                text: controller.certStatus
            }

            SectionHeader { text: qsTr("Live traffic") }

            TextField {
                width: parent.width
                label: qsTr("TomTom API key")
                placeholderText: qsTr("Paste a free TomTom key")
                text: controller.trafficKey
                onTextChanged: controller.trafficKey = text
            }

            TextSwitch {
                text: qsTr("Show traffic on the map")
                description: qsTr("Colours roads by congestion for the area on screen.")
                enabled: controller.trafficKey !== ""
                checked: controller.trafficEnabled
                onClicked: controller.trafficEnabled = checked
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTr("Live traffic needs a free TomTom API key (developer.tomtom.com) — OpenStreetMap has no traffic data.")
            }

            SectionHeader { text: qsTr("Status") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                color: Theme.highlightColor
                text: controller.state
            }

            SectionHeader { text: qsTr("Log") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: controller.log
            }
        }

        VerticalScrollDecorator { }
    }
}
