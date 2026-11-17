import QtQuick 2.0
import Sailfish.Silica 1.0

// Pick which installed app backs navigation and which backs media. The choices
// are remembered (controller.navApp / mediaApp) and the apps are started
// automatically when projection begins.
Page {
    id: page

    // index in controller.apps for a stored id, or -1
    function indexOfApp(id) {
        var apps = controller.apps
        for (var i = 0; i < apps.length; ++i)
            if (apps[i].id === id)
                return i
        return -1
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                text: qsTr("Rescan apps")
                onClicked: controller.refreshApps()
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: qsTr("Apps") }

            ComboBox {
                id: navBox
                width: parent.width
                label: qsTr("Navigation app (optional — map is built-in)")
                // index 0 is "None"; apps follow
                currentIndex: page.indexOfApp(controller.navApp) + 1
                menu: ContextMenu {
                    MenuItem { text: qsTr("None") ; onClicked: controller.navApp = "" }
                    Repeater {
                        model: controller.apps
                        MenuItem {
                            text: modelData.name + (modelData.android ? "  ·  Android" : "")
                            onClicked: controller.navApp = modelData.id
                        }
                    }
                }
            }

            ComboBox {
                id: mediaBox
                width: parent.width
                label: qsTr("Media app")
                currentIndex: page.indexOfApp(controller.mediaApp) + 1
                menu: ContextMenu {
                    MenuItem { text: qsTr("None") ; onClicked: controller.mediaApp = "" }
                    Repeater {
                        model: controller.apps
                        MenuItem {
                            text: modelData.name + (modelData.android ? "  ·  Android" : "")
                            onClicked: controller.mediaApp = modelData.id
                        }
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: qsTr("Navigation is built in — the map pane renders OpenStreetMap "
                           + "natively with your GPS, so no navigation app is required "
                           + "(leave it on None). A navigation app is optional, for future "
                           + "routing.\n\n"
                           + "The media app is your player: its track and controls appear in "
                           + "the now-playing pane via MPRIS, so it must be running and "
                           + "playing. Chosen apps are launched automatically when the car "
                           + "connects.")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Test-launch both")
                enabled: controller.navApp !== "" || controller.mediaApp !== ""
                onClicked: {
                    if (controller.mediaApp !== "") controller.launchApp(controller.mediaApp)
                    if (controller.navApp !== "" && controller.navApp !== controller.mediaApp)
                        controller.launchApp(controller.navApp)
                }
            }
        }

        VerticalScrollDecorator { }
    }
}
