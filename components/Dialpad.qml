pragma ComponentBehavior: Bound

import QtQuick
import qs.Commons
import qs.Ui

// The twelve keys, used both to compose a number before a call and to send
// DTMF during one. The letters under the digits are what people read off a
// phone menu ("press 2 for..."), so they earn their pixels.
Grid {
  id: root

  // Style.font is a QObject with dynamic members, so reading it through a var
  // keeps qmllint quiet without giving up the token.
  readonly property var fontTokens: Style.font
  property color foreground: Color.foreground
  property string fontFamily: fontTokens.family
  property real keyWidth: Style.space(64)

  signal keyPressed(string digit)

  columns: 3
  spacing: Style.space(6)

  readonly property var keys: [
    { digit: "1", letters: "" },
    { digit: "2", letters: "ABC" },
    { digit: "3", letters: "DEF" },
    { digit: "4", letters: "GHI" },
    { digit: "5", letters: "JKL" },
    { digit: "6", letters: "MNO" },
    { digit: "7", letters: "PQRS" },
    { digit: "8", letters: "TUV" },
    { digit: "9", letters: "WXYZ" },
    { digit: "*", letters: "" },
    { digit: "0", letters: "+" },
    { digit: "#", letters: "" }
  ]

  Repeater {
    model: root.keys

    delegate: Button {
      id: key
      required property var modelData

      implicitWidth: root.keyWidth
      implicitHeight: Style.space(38)
      foreground: root.foreground
      fontFamily: root.fontFamily
      bordered: true
      focusable: false
      onClicked: root.keyPressed(key.modelData.digit)

      Column {
        anchors.centerIn: parent
        spacing: 0

        Text {
          anchors.horizontalCenter: parent.horizontalCenter
          textFormat: Text.PlainText
          text: key.modelData.digit
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: root.fontTokens.subtitle
        }

        Text {
          anchors.horizontalCenter: parent.horizontalCenter
          visible: key.modelData.letters !== ""
          textFormat: Text.PlainText
          text: key.modelData.letters
          color: Qt.darker(root.foreground, 1.7)
          font.family: root.fontFamily
          font.pixelSize: root.fontTokens.caption
          font.letterSpacing: 1
        }
      }
    }
  }
}
