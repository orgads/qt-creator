import QtQuick %{QtQuickVersion}
import QtQuick.Controls %{QtQuickVersion}
import %{ImportModuleName} %{ImportModuleVersion}

Rectangle {
    width: Constants.width
    height: Constants.height

    color: Constants.backgroundColor

    Text {
        text: qsTr("Hello %{ProjectName}")
        anchors.centerIn: parent
        font.family: Constants.font.family
    }
}