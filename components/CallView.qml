pragma ComponentBehavior: Bound

import QtQuick
import qs.Commons
import qs.Ui
import "../Model.js" as Model

// The screen that replaces everything else while a call exists: who it is,
// how long it has been going, and the three or four things you might want to
// do about it. The keypad appears only once the call is connected, because
// DTMF before that has nowhere to go.
Column {
  id: root

  required property var phone
  required property var i18n
  property var call: null
  property color foreground: Color.foreground
  property color accent: Color.accent
  property color urgent: Color.urgent
  property string fontFamily: fontTokens.family
  readonly property var fontTokens: Style.font

  readonly property bool incoming: call && call.state === "incoming"
  readonly property bool connected: call && call.state === "connected"
  readonly property string who: {
    if (!call) return ""
    if (call.display && call.display !== "") return call.display
    if (call.number && call.number !== "") return call.number
    return i18n.t("call.unknown")
  }
  readonly property string subtitle: {
    if (!call) return ""
    switch (call.state) {
    case "incoming": return i18n.t("call.incoming")
    case "calling": return i18n.t("call.calling")
    case "early": return i18n.t("call.ringing")
    case "connected": return i18n.t("call.connected")
    case "disconnecting": return i18n.t("call.ending")
    }
    return ""
  }

  property int elapsed: 0

  // Tick only while there is something to count. started_at is the daemon's
  // clock, so the timer never drifts away from the real duration.
  Timer {
    interval: 1000
    running: root.connected && root.call && root.call.started_at
    repeat: true
    triggeredOnStart: true
    onTriggered: root.elapsed = Math.max(0, Math.floor(Date.now() / 1000) - root.call.started_at)
  }

  spacing: Style.space(10)

  Column {
    width: parent.width
    spacing: Style.space(2)

    Text {
      width: parent.width
      textFormat: Text.PlainText
      text: root.who
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: root.fontTokens.heading
      elide: Text.ElideRight
    }

    Text {
      width: parent.width
      visible: root.call && root.call.display && root.call.display !== "" && root.call.number !== ""
      textFormat: Text.PlainText
      text: root.call ? root.call.number : ""
      color: Qt.darker(root.foreground, 1.5)
      font.family: root.fontFamily
      font.pixelSize: root.fontTokens.body
      elide: Text.ElideRight
    }

    Row {
      spacing: Style.space(8)

      Text {
        textFormat: Text.PlainText
        text: root.subtitle
        color: root.incoming ? root.urgent : root.accent
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.body
      }

      Text {
        visible: root.connected
        textFormat: Text.PlainText
        text: Model.duration(root.elapsed)
        color: Qt.darker(root.foreground, 1.4)
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.body
      }

      Text {
        visible: root.connected && root.call && root.call.codec
        textFormat: Text.PlainText
        text: root.call && root.call.codec ? root.call.codec : ""
        color: Qt.darker(root.foreground, 1.8)
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.caption
      }
    }
  }

  Row {
    spacing: Style.space(8)

    Button {
      visible: root.incoming
      text: root.i18n.t("call.answer")
      iconText: Model.Glyph.phone
      foreground: root.accent
      fontFamily: root.fontFamily
      bordered: true
      onClicked: root.phone.answer()
    }

    Button {
      text: root.incoming ? root.i18n.t("call.reject") : root.i18n.t("call.hangup")
      iconText: Model.Glyph.hangup
      foreground: root.urgent
      fontFamily: root.fontFamily
      bordered: true
      onClicked: root.phone.hangup()
    }

    Button {
      visible: root.connected
      text: root.phone && root.phone.muted ? root.i18n.t("call.unmute") : root.i18n.t("call.mute")
      foreground: root.phone && root.phone.muted ? root.urgent : root.foreground
      fontFamily: root.fontFamily
      bordered: true
      onClicked: root.phone.setMuted(!root.phone.muted)
    }
  }

  PanelSeparator {
    width: parent.width
    visible: root.connected
    foreground: root.foreground
  }

  Dialpad {
    visible: root.connected
    foreground: root.foreground
    fontFamily: root.fontFamily
    keyWidth: Style.space(60)
    onKeyPressed: function(digit) { root.phone.dtmf(digit) }
  }
}
