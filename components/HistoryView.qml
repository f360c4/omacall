pragma ComponentBehavior: Bound

import QtQuick
import qs.Commons
import qs.Ui
import "../Model.js" as Model

// Recent calls, newest first. A row is a button: clicking it calls back, which
// is the only thing anyone ever wants from a call list.
Column {
  id: root

  required property var phone
  required property var i18n
  property color foreground: Color.foreground
  property color urgent: Color.urgent
  property string fontFamily: fontTokens.family
  readonly property var fontTokens: Style.font

  signal callRequested(string number)

  property bool confirmingClear: false

  spacing: Style.space(4)

  // Clearing the history is easy to hit by accident and impossible to undo,
  // so the button asks once before doing it.
  Row {
    width: parent.width
    visible: root.phone && root.phone.history.length > 0
    spacing: Style.space(6)

    Button {
      visible: !root.confirmingClear
      text: root.i18n.t("history.clear")
      foreground: Qt.darker(root.foreground, 1.3)
      fontFamily: root.fontFamily
      fontSize: root.fontTokens.caption
      onClicked: root.confirmingClear = true
    }

    Text {
      anchors.verticalCenter: parent.verticalCenter
      visible: root.confirmingClear
      textFormat: Text.PlainText
      text: root.i18n.t("history.clearConfirm")
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: root.fontTokens.caption
      wrapMode: Text.WordWrap
    }

    Button {
      visible: root.confirmingClear
      text: root.i18n.t("common.delete")
      foreground: root.urgent
      fontFamily: root.fontFamily
      fontSize: root.fontTokens.caption
      bordered: true
      onClicked: {
        root.phone.clearHistory()
        root.confirmingClear = false
      }
    }

    Button {
      visible: root.confirmingClear
      text: root.i18n.t("common.cancel")
      foreground: root.foreground
      fontFamily: root.fontFamily
      fontSize: root.fontTokens.caption
      onClicked: root.confirmingClear = false
    }
  }

  Text {
    width: parent.width
    visible: !root.phone || root.phone.history.length === 0
    textFormat: Text.PlainText
    text: root.i18n.t("history.empty")
    color: Qt.darker(root.foreground, 1.6)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.body
  }

  Repeater {
    model: root.phone ? root.phone.history : []

    delegate: Button {
      id: row
      required property var modelData

      readonly property bool missed: row.modelData.reason === "missed"
      readonly property bool outgoing: row.modelData.direction === "out"

      width: root.width
      // A row is two lines of text plus padding. Without an explicit height
      // the button takes none from its anchored content and every row lands on
      // top of the last one.
      implicitHeight: Style.space(38)
      foreground: root.foreground
      fontFamily: root.fontFamily
      onClicked: root.callRequested(String(row.modelData.number))

      Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Style.space(8)
        anchors.rightMargin: Style.space(8)
        spacing: Style.space(8)

        Text {
          anchors.verticalCenter: parent.verticalCenter
          textFormat: Text.PlainText
          text: row.outgoing ? Model.Glyph.outgoing : (row.missed ? Model.Glyph.missed : Model.Glyph.incoming)
          color: row.missed ? root.urgent : Qt.darker(root.foreground, 1.4)
          font.family: root.fontFamily
          font.pixelSize: root.fontTokens.body
        }

        Column {
          anchors.verticalCenter: parent.verticalCenter
          width: parent.width - Style.space(150)
          spacing: 0

          Text {
            width: parent.width
            textFormat: Text.PlainText
            text: row.modelData.display && row.modelData.display !== ""
                  ? row.modelData.display : row.modelData.number
            color: row.missed ? root.urgent : root.foreground
            font.family: root.fontFamily
            font.pixelSize: root.fontTokens.body
            elide: Text.ElideRight
          }

          Text {
            width: parent.width
            textFormat: Text.PlainText
            text: root.i18n.t("history.reason." + row.modelData.reason)
                  + (row.modelData.duration_sec > 0
                     ? " · " + Model.duration(row.modelData.duration_sec) : "")
            color: Qt.darker(root.foreground, 1.7)
            font.family: root.fontFamily
            font.pixelSize: root.fontTokens.caption
            elide: Text.ElideRight
          }
        }

        Text {
          anchors.verticalCenter: parent.verticalCenter
          textFormat: Text.PlainText
          text: Model.historyStamp(row.modelData.ts, root.i18n.code, Locale.ShortFormat)
          color: Qt.darker(root.foreground, 1.7)
          font.family: root.fontFamily
          font.pixelSize: root.fontTokens.caption
        }
      }
    }
  }
}
