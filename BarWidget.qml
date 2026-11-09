pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Quickshell.Io
import qs.Commons
import qs.Ui
import "Model.js" as Model
import "components"

// Bar widget: the phone icon, and the panel behind it.
//
// Every bit of state comes from the service, which is mounted once for the
// whole shell; this file only draws it and forwards intent. Colors, spacing
// and type come from the active Omarchy theme through Color.* and Style.*, so
// `omarchy theme set` restyles the panel live without it knowing.
Panel {
  id: root

  moduleName: "f360c4.omacall"
  ipcTarget: "f360c4.omacall"
  manageIpc: false

  implicitWidth: barRow.implicitWidth
  implicitHeight: barRow.implicitHeight

  // `bar` is typed QtObject, so its members are only known at runtime. Reading
  // them through a var keeps the accesses honest for qmllint.
  readonly property var host: bar
  readonly property var fontTokens: Style.font

  readonly property var phone: host && host.shell ? host.shell.serviceFor("f360c4.omacall") : null
  readonly property string phoneState: phone ? phone.phoneState : "daemon_missing"
  readonly property bool daemonConnected: phone ? phone.connected : false
  readonly property string statusKey: Model.statusKey(phoneState, daemonConnected)
  readonly property var call: phone ? phone.activeCall : null
  readonly property bool live: statusKey === "registered" || statusKey === "inCall"
       || statusKey === "incoming"

  readonly property color foreground: host ? host.foreground : Color.foreground
  readonly property string fontFamily: host ? host.fontFamily : fontTokens.family
  readonly property color dim: Qt.darker(barForeground, 1.55)
  readonly property color barIconColor: {
    if (statusKey === "incoming") return host ? host.urgent : Color.urgent
    if (statusKey === "inCall") return Color.accent
    return live ? barForeground : dim
  }

  // dialer | contacts | history | settings — ignored while a call exists,
  // because the call screen takes the whole panel.
  property string tab: "dialer"
  property string dialed: ""
  property int callElapsed: 0

  OmacallI18n {
    id: i18n
    language: root.setting("language", "auto")
  }

  function placeCall(destination) {
    var target = String(destination || "").trim()
    if (target === "" || !root.phone)
      return
    root.phone.placeCall(target)
    root.dialed = ""
  }

  // An incoming call pops the panel open, if the user left that on.
  Connections {
    target: root.phone
    function onIncomingCall(incoming) {
      if (root.setting("openOnIncoming", true) && !root.opened)
        root.open()
    }
  }

  // Keybind and script surface: `omarchy-shell f360c4.omacall <method>`.
  IpcHandler {
    target: root.ipcTarget

    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function answer(): string { return root.phone && root.phone.answer() ? "ok" : "unavailable" }
    function hangup(): string { return root.phone && root.phone.hangup() ? "ok" : "unavailable" }
    function call(dest: string): string { return root.phone && root.phone.placeCall(dest) ? "ok" : "unavailable" }
    function dtmf(digits: string): string { return root.phone && root.phone.dtmf(digits) ? "ok" : "unavailable" }
    function mute(): string {
      if (!root.phone) return "unavailable"
      return root.phone.setMuted(!root.phone.muted) ? "ok" : "unavailable"
    }
    function status(): string { return root.phoneState }

    // Open straight to a section, so a keybind can go to the dialer or the
    // history without two steps. An unknown name is refused rather than
    // silently leaving the panel wherever it was.
    function tab(name: string): string {
      var known = ["dialer", "contacts", "history", "settings"]
      if (known.indexOf(name) === -1)
        return "unknown tab: " + name
      root.tab = name
      root.open()
      return "ok"
    }
  }

  // ------------------------------------------------------------- bar icon

  // Duration next to the icon, when the user asked for it.
  Timer {
    interval: 1000
    running: root.call && root.call.started_at && root.setting("showLabel", false)
    repeat: true
    triggeredOnStart: true
    onTriggered: root.callElapsed = Math.max(0, Math.floor(Date.now() / 1000) - root.call.started_at)
  }

  // BarIconButton owns a fixed optical slot, so the timer sits beside it in a
  // row rather than inside it — the same shape omarchy.media uses.
  Row {
    id: barRow
    anchors.centerIn: parent
    spacing: 0

    BarIconButton {
      id: button
      bar: root.bar
      text: Model.statusGlyph(root.statusKey)
      foreground: root.barIconColor
      tooltipText: i18n.t("bar.tooltip." + root.statusKey)
      onPressed: function(buttonCode) {
      // Right click answers a ringing call and hangs up a live one, so the
      // most urgent action never needs the panel.
        if (buttonCode === Qt.RightButton && root.phone) {
          if (root.statusKey === "incoming") root.phone.answer()
          else if (root.statusKey === "inCall") root.phone.hangup()
          else root.toggle()
        } else {
          root.toggle()
        }
      }

      // An incoming call blinks; a live one simply sits in the accent colour.
      SequentialAnimation on opacity {
        running: root.statusKey === "incoming"
        loops: Animation.Infinite
        NumberAnimation { to: 0.35; duration: 500; easing.type: Easing.InOutQuad }
        NumberAnimation { to: 1.0; duration: 500; easing.type: Easing.InOutQuad }
        onRunningChanged: if (!running) button.opacity = 1.0
      }
    }

    Text {
      anchors.verticalCenter: parent.verticalCenter
      visible: root.setting("showLabel", false) && root.statusKey === "inCall"
               && !(root.host && root.host.vertical)
      textFormat: Text.PlainText
      text: Model.duration(root.callElapsed)
      color: root.barIconColor
      font.family: root.host ? root.host.fontFamily : root.fontTokens.family
      font.pixelSize: root.fontTokens.body
      rightPadding: Style.space(6)
    }
  }

  // ---------------------------------------------------------------- panel

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(360))
    contentHeight: panel.fittedContentHeight(column.implicitHeight, Style.space(660))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      // Typing a digit anywhere in the panel starts dialling, the way a desk
      // phone does — no need to find the field with the mouse first. Anything
      // that is not part of a number is left alone for the sections that have
      // their own text fields.
      onTextKey: function(typed) {
        if (root.call || root.tab !== "dialer" || dialField.activeFocus)
          return
        if (/^[0-9*#+]$/.test(typed)) {
          root.dialed += typed
          // "+" is a dialling convenience, not a DTMF digit; the daemon would
          // refuse it and the click is not worth an error either way.
          if (root.phone && typed !== "+")
            root.phone.keyTone(typed)
          dialField.forceActiveFocus()
          dialField.cursorPosition = dialField.text.length
        }
      }

      // Enter dials whatever is in the field, from wherever the focus is.
      onReturnRequested: {
        if (!root.call && root.tab === "dialer")
          root.placeCall(root.dialed)
      }

      Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
          id: column
          width: flick.width
          spacing: Style.space(10)

          PanelHero {
            width: parent.width
            title: i18n.t("app.name")
            meta: i18n.t("status." + (root.statusKey === "inCall" || root.statusKey === "incoming"
                                      ? "registered" : root.statusKey))
            detail: root.phone ? root.phone.accountUri : ""
            foreground: root.foreground
            fontFamily: root.fontFamily
            iconOpacity: root.live ? 1.0 : 0.5
            iconComponent: Component {
              Text {
                textFormat: Text.PlainText
                text: Model.statusGlyph(root.statusKey)
                color: root.live ? root.foreground : Qt.darker(root.foreground, 1.4)
                font.family: root.fontFamily
                font.pixelSize: root.fontTokens.display
              }
            }
          }

          InstallHelp {
            width: parent.width
            visible: root.statusKey === "daemonMissing"
            i18n: i18n
            foreground: root.foreground
            fontFamily: root.fontFamily
          }

          Text {
            width: parent.width
            visible: root.daemonConnected && root.statusKey === "notRegistered"
                     && (!root.phone || !root.phone.accountUri)
            textFormat: Text.PlainText
            text: i18n.t("panel.hint.noAccount")
            color: Qt.darker(root.foreground, 1.4)
            font.family: root.fontFamily
            font.pixelSize: root.fontTokens.body
            wrapMode: Text.WordWrap
          }

          // ------------------------------------------------- call screen

          CallView {
            width: parent.width
            visible: root.call !== null && root.call !== undefined
            phone: root.phone
            i18n: i18n
            call: root.call
            foreground: root.foreground
            fontFamily: root.fontFamily
          }

          // ------------------------------------------------------- tabs

          Row {
            width: parent.width
            visible: !root.call && root.daemonConnected
            spacing: Style.space(4)

            Repeater {
              model: [
                { key: "dialer",   label: "tab.dialer" },
                { key: "contacts", label: "tab.contacts" },
                { key: "history",  label: "tab.history" },
                { key: "settings", label: "tab.settings" }
              ]

              delegate: Button {
                id: tabButton
                required property var modelData

                text: i18n.t(tabButton.modelData.label)
                foreground: root.tab === tabButton.modelData.key ? Color.accent : root.foreground
                fontFamily: root.fontFamily
                selected: root.tab === tabButton.modelData.key
                fontSize: root.fontTokens.caption
                onClicked: root.tab = tabButton.modelData.key
              }
            }
          }

          // ----------------------------------------------------- dialer

          Column {
            width: parent.width
            visible: !root.call && root.daemonConnected && root.tab === "dialer"
            spacing: Style.space(8)

            Row {
              width: parent.width
              spacing: Style.space(6)

              TextField {
                id: dialField
                width: parent.width - dialButton.width - Style.space(6)
                placeholderText: i18n.t("dialer.placeholder")
                text: root.dialed
                foreground: root.foreground
                onTextChanged: root.dialed = text
                Keys.onReturnPressed: root.placeCall(root.dialed)
              }

              Button {
                id: dialButton
                anchors.verticalCenter: parent.verticalCenter
                text: i18n.t("dialer.call")
                iconText: Model.Glyph.phone
                foreground: Color.accent
                fontFamily: root.fontFamily
                bordered: true
                enabled: root.dialed.trim() !== ""
                onClicked: root.placeCall(root.dialed)
              }
            }

            Dialpad {
              anchors.horizontalCenter: parent.horizontalCenter
              foreground: root.foreground
              fontFamily: root.fontFamily
              onKeyPressed: function(digit) {
                root.dialed += digit
                if (root.phone)
                  root.phone.keyTone(digit)
              }
            }
          }

          // --------------------------------------------------- contacts

          ContactsView {
            width: parent.width
            visible: !root.call && root.daemonConnected && root.tab === "contacts"
            phone: root.phone
            i18n: i18n
            foreground: root.foreground
            fontFamily: root.fontFamily
            onCallRequested: function(number) { root.placeCall(number) }
          }

          // ---------------------------------------------------- history

          HistoryView {
            width: parent.width
            visible: !root.call && root.daemonConnected && root.tab === "history"
            phone: root.phone
            i18n: i18n
            foreground: root.foreground
            fontFamily: root.fontFamily
            onCallRequested: function(number) { root.placeCall(number) }
          }

          // --------------------------------------------------- settings

          SettingsView {
            width: parent.width
            visible: !root.call && root.daemonConnected && root.tab === "settings"
            phone: root.phone
            i18n: i18n
            host: root.host
            moduleName: root.moduleName
            language: root.setting("language", "auto")
            foreground: root.foreground
            fontFamily: root.fontFamily
          }

          PanelSeparator {
            width: parent.width
            foreground: root.foreground
          }

          Text {
            width: parent.width
            textFormat: Text.PlainText
            text: root.phone && root.phone.lastError !== ""
                  ? root.phone.lastError
                  : i18n.t("panel.footer.version", { version: root.phone ? root.phone.version : "0.1.0" })
            color: root.phone && root.phone.lastError !== ""
                   ? Color.urgent : Qt.darker(root.foreground, 1.6)
            font.family: root.fontFamily
            font.pixelSize: root.fontTokens.caption
            horizontalAlignment: Text.AlignRight
            wrapMode: Text.WordWrap
          }
        }
      }
    }
  }
}
