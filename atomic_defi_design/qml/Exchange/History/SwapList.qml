import QtQuick 2.14
import QtQuick.Layouts 1.12
import QtQuick.Controls 2.12

import "../../Components"
import "../../Constants"
import ".."

InnerBackground {
    property string title
    property var items

    // Override
    property var postCancelOrder: () => {}

    // Local
    function onCancelOrder(order_id) {
        API.get().trading_pg.cancel_order(order_id)
        postCancelOrder()
    }

    Layout.fillWidth: true
    Layout.fillHeight: true
    color: Style.colorTheme7
    radius: Style.rectangleCornerRadius

    ColumnLayout {
        width: parent.width
        height: parent.height

        DefaultText {
            text_value: API.get().settings_pg.empty_string + (title + " (" + items.length + ")")

            Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
            Layout.topMargin: 10

            font.pixelSize: Style.textSize2
        }

        HorizontalLine {
            Layout.fillWidth: true
            color: Style.colorWhite8
        }

        // No orders
        DefaultText {
            wrapMode: Text.Wrap
            visible: items.length === 0
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 20
            color: Style.colorWhite5

            text_value: API.get().settings_pg.empty_string + (qsTr("You don't have recent orders."))
        }

        // List
        DefaultListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true

            model: items.orders_proxy_mdl

            // Row
            delegate: OrderLine {
                details: model
            }
        }
    }
}










/*##^##
Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
##^##*/
