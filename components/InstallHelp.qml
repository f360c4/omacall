pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import qs.Commons
import qs.Ui

// What someone sees the first time, before the daemon exists.
//
// Omarchy installs no software when a plugin is added — by design, and the
// plugin guide is explicit about it. So the panel's job here is to say exactly
// what is missing and hand over the two commands that fix it, rather than
// leaving someone to find the README.
//
// There is no "install it for me" button: makepkg wants a terminal of its own,
// and a plugin that installed packages without being watched would be a plugin
// nobody should trust.
Column {
  id: root

  required property var i18n
  property color foreground: Color.foreground
  property color accent: Color.accent
  property string fontFamily: fontTokens.family
  readonly property var fontTokens: Style.font

  readonly property string commands:
      "omarchy pkg aur add bcg729\n" +
      "git clone https://github.com/f360c4/omacall.git\n" +
      "cd omacall/packaging && makepkg -si"

  property bool copied: false

  spacing: Style.space(8)

  Text {
    width: parent.width
    textFormat: Text.PlainText
    text: root.i18n.t("panel.install.intro")
    color: root.foreground
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.body
    wrapMode: Text.WordWrap
  }

  Text {
    width: parent.width
    textFormat: Text.PlainText
    text: root.i18n.t("panel.install.run")
    color: Qt.darker(root.foreground, 1.4)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
    wrapMode: Text.WordWrap
  }

  Rectangle {
    width: parent.width
    implicitHeight: commandText.implicitHeight + Style.space(16)
    color: Style.hoverFillFor(root.foreground, root.accent)
    radius: Style.cornerRadius

    Text {
      id: commandText
      anchors.fill: parent
      anchors.margins: Style.space(8)
      textFormat: Text.PlainText
      text: root.commands
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: root.fontTokens.caption
      wrapMode: Text.WrapAnywhere
    }
  }

  Row {
    spacing: Style.space(8)

    Button {
      text: root.copied ? root.i18n.t("panel.install.copied")
                        : root.i18n.t("panel.install.copy")
      iconText: "\uf0c5"
      foreground: root.copied ? root.accent : root.foreground
      fontFamily: root.fontFamily
      fontSize: root.fontTokens.caption
      bordered: true
      onClicked: {
        Quickshell.execDetached(["wl-copy", "--", root.commands])
        root.copied = true
        copiedReset.restart()
      }
    }
  }

  Timer {
    id: copiedReset
    interval: 2500
    onTriggered: root.copied = false
  }

  Text {
    width: parent.width
    textFormat: Text.PlainText
    text: root.i18n.t("panel.install.why")
    color: Qt.darker(root.foreground, 1.9)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
    wrapMode: Text.WordWrap
  }
}
