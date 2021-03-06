import QtQuick 2.14
import QtQuick.Layouts 1.12
import QtQuick.Controls 2.12

import "../Components"
import "../Constants"
import "../Wallet"
import "../Exchange"
import "../Sidebar"

SetupPage {
    // Override
    property var onLoaded: () => {}

    readonly property string current_status: API.get().initial_loading_status

    onCurrent_statusChanged: {
        if(current_status === "complete")
            onLoaded()
    }


    image_scale: 0.7
    image_path: General.image_path + "komodo-icon.png"

    content: ColumnLayout {
        DefaultText {
            text_value: API.get().settings_pg.empty_string + (qsTr("Loading, please wait"))
            Layout.bottomMargin: 10
        }

        RowLayout {
            DefaultBusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                Layout.leftMargin: -15
                Layout.rightMargin: Layout.leftMargin*0.75
                scale: 0.5
            }

            DefaultText {
                text_value: API.get().settings_pg.empty_string + ((current_status === "initializing_mm2" ? qsTr("Initializing MM2") :
                       current_status === "enabling_coins" ? qsTr("Enabling coins") : qsTr("Getting ready")) + "...")
            }
        }
    }
}

/*##^##
Designer {
    D{i:0;autoSize:true;height:480;width:1200}
}
##^##*/
