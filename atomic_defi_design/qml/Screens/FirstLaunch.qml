import QtQuick 2.14
import QtQuick.Layouts 1.12
import QtQuick.Controls 2.12

import "../Components"
import "../Constants"
import "../Settings"

SetupPage {
    // Override
    property var onClickedNewUser: () => {}
    property var onClickedRecoverSeed: () => {}
    property var onClickedWallet: () => {}

    // Local
    function updateWallets() {
        wallets = API.get().get_wallets()
    }

    property var wallets: ([])

    image_path: General.image_path + Style.sidebar_atomicdefi_logo
    image_margin: 30
    content: ColumnLayout {
        width: 400
        spacing: Style.rowSpacing
        DefaultText {
            text_value: API.get().settings_pg.empty_string + (qsTr("Welcome"))
        }

        HorizontalLine {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Style.buttonSpacing

            DefaultButton {
                Layout.fillWidth: true
                text: API.get().settings_pg.empty_string + (qsTr("Recover Seed"))
                onClicked: onClickedRecoverSeed()
            }

            DefaultButton {
                Layout.fillWidth: true
                text: API.get().settings_pg.empty_string + (qsTr("New User"))
                onClicked: onClickedNewUser()
            }
        }

        // Wallets
        ColumnLayout {
            spacing: Style.rowSpacing

            visible: wallets.length > 0

            // Name
            DefaultText {
                text_value: API.get().settings_pg.empty_string + (qsTr("Wallets"))
                font.pixelSize: Style.textSizeSmall2
            }

            InnerBackground {
                id: bg
                Layout.fillWidth: true
                readonly property int row_height: 40
                Layout.minimumHeight: row_height
                Layout.preferredHeight: row_height * Math.min(wallets.length, 3)

                content: DefaultListView {
                    id: list
                    implicitHeight: bg.Layout.preferredHeight

                    model: wallets

                    delegate: GradientRectangle {
                        start_color: mouse_area.containsMouse ? Style.colorWalletsHighlightGradient1 : "transparent"
                        end_color: mouse_area.containsMouse ? Style.colorWalletsHighlightGradient2 : "transparent"

                        width: bg.width
                        height: bg.row_height

                        // Click area
                        MouseArea {
                            id: mouse_area
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                selected_wallet_name = model.modelData
                                onClickedWallet()
                            }
                        }

                        // Name
                        DefaultText {
                            anchors.left: parent.left
                            anchors.leftMargin: 40

                            text_value: API.get().settings_pg.empty_string + (model.modelData)
                            anchors.verticalCenter: parent.verticalCenter
                            font.pixelSize: Style.textSizeSmall2
                        }

                        HorizontalLine {
                            visible: index !== wallets.length -1
                            width: parent.width - 4

                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: -height/2
                            light: true
                        }
                    }
                }
            }
        }



        HorizontalLine {
            light: true
            Layout.fillWidth: true
        }

        Languages {
        }
    }
}

/*##^##
Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
##^##*/

