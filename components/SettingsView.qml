pragma ComponentBehavior: Bound

import QtQuick
import qs.Commons
import qs.Ui

// The SIP account, and the two NAT fields people actually need. Everything
// else in config.json stays in config.json: this form is for the things you
// cannot make a call without.
//
// The password is never read back from the daemon — config.get answers with
// password_set instead — so an empty field means "keep what is there".
Column {
  id: root

  required property var phone
  required property var i18n
  // The widget's own shell.json entry, for the settings that are the panel's
  // rather than the daemon's — the language is one of the bar widget's
  // settings, not something the daemon knows about.
  required property var host
  required property string moduleName
  required property string language
  property color foreground: Color.foreground
  property color accent: Color.accent
  property color urgent: Color.urgent
  property string fontFamily: fontTokens.family
  readonly property var fontTokens: Style.font

  property string displayName: ""
  property string username: ""
  property string password: ""
  property string domain: ""
  property string proxy: ""
  property string stun: ""
  property string transport: "udp"
  property bool tlsVerify: true
  property bool showAdvanced: false
  property bool creating: false
  property string newName: ""
  property bool passwordSet: false
  property bool saved: false

  // Every codec the daemon can offer, in the order they are proposed. The
  // config keeps only the enabled ones, and the order of that array is the
  // preference order, so this list is the full menu and `enabled` is the mark.
  readonly property var knownCodecs: [
    { id: "G729/8000",  label: "G.729" },
    { id: "opus/48000", label: "Opus" },
    { id: "PCMA/8000",  label: "G.711 A-law" },
    { id: "PCMU/8000",  label: "G.711 µ-law" }
  ]
  property var enabledCodecs: []

  readonly property var languages: [
    { code: "auto",  label: "" },
    { code: "en",    label: "English" },
    { code: "pt-BR", label: "Português" },
    { code: "es",    label: "Español" },
    { code: "it",    label: "Italiano" },
    { code: "fr",    label: "Français" },
    { code: "de",    label: "Deutsch" },
    { code: "ja",    label: "日本語" }
  ]

  readonly property var accountNames: {
    var out = []
    var list = phone ? phone.accounts : []
    for (var i = 0; i < list.length; i++)
      out.push(list[i].name)
    return out
  }

  function saveNewAccount() {
    var name = newName.trim()
    if (name === "")
      return
    // Create it empty and switch to it; the form below then edits it like any
    // other, which keeps one path for "fill in an account" instead of two.
    phone.upsertAccount({ name: name }, function(result, error) {
      if (!error) {
        phone.selectAccount(name)
        root.creating = false
        root.newName = ""
      }
    })
  }

  readonly property var languageLabels: {
    var out = []
    for (var i = 0; i < languages.length; i++)
      out.push(labelForLanguage(languages[i].code))
    return out
  }

  function labelForLanguage(code) {
    for (var i = 0; i < languages.length; i++) {
      if (languages[i].code === code)
        return languages[i].code === "auto"
               ? i18n.t("settings.languageAuto") : languages[i].label
    }
    return i18n.t("settings.languageAuto")
  }

  function languageForLabel(label) {
    for (var i = 0; i < languages.length; i++) {
      if (labelForLanguage(languages[i].code) === label)
        return languages[i].code
    }
    return "auto"
  }

  function setLanguage(code) {
    if (host && host.shell && typeof host.shell.updateEntryInline === "function")
      host.shell.updateEntryInline(root.moduleName, { language: code })
  }

  function toggleCodec(id) {
    var next = []
    var found = false
    for (var i = 0; i < enabledCodecs.length; i++) {
      if (enabledCodecs[i] === id)
        found = true
      else
        next.push(enabledCodecs[i])
    }
    if (!found) {
      // Re-enabling puts it back in the canonical order rather than at the
      // end, so the preference order stays predictable.
      next = []
      for (var k = 0; k < knownCodecs.length; k++) {
        var candidate = knownCodecs[k].id
        if (candidate === id || enabledCodecs.indexOf(candidate) !== -1)
          next.push(candidate)
      }
    }
    if (next.length === 0)
      return          // the daemon would have nothing to negotiate with
    enabledCodecs = next
    phone.setConfig({ codecs: next })
    saved = true
  }

  function load() {
    var config = phone ? phone.config : null
    if (!config || !config.account)
      return
    displayName = config.account.display_name || ""
    username = config.account.username || ""
    domain = config.account.domain || ""
    proxy = config.account.proxy || ""
    passwordSet = !!config.account.password_set
    password = ""
    stun = config.nat ? (config.nat.stun_server || "") : ""
    transport = config.account.transport || "udp"
    tlsVerify = config.tls ? config.tls.verify_server !== false : true
    enabledCodecs = config.codecs instanceof Array ? config.codecs.slice() : []
    saved = false
  }

  function save() {
    var patch = {
      account: {
        display_name: displayName.trim(),
        username: username.trim(),
        domain: domain.trim(),
        proxy: proxy.trim(),
        transport: transport
      },
      tls: { verify_server: tlsVerify },
      nat: { stun_server: stun.trim() }
    }
    // Only send a password when one was typed, so saving the form without
    // touching it does not blank the stored one.
    if (password !== "")
      patch.account.password = password

    phone.setConfig(patch, function(result, error) {
      root.saved = !error
      if (!error) {
        root.password = ""
        root.passwordSet = true
      }
    })
  }

  spacing: Style.space(6)

  Component.onCompleted: load()

  // The view is built before `phone` resolves, so the load above finds nothing
  // and leaves the form blank. By the time the service arrives its config is
  // usually already set, which means no configChanged is coming either — the
  // arrival of the service is the signal.
  onPhoneChanged: load()

  // ------------------------------------------------------------- account
  //
  // Several accounts can be configured; one registers at a time. Switching is
  // the point of the list, so it sits above everything it changes.

  Row {
    width: parent.width
    visible: !root.creating
    spacing: Style.space(6)

    Dropdown {
      width: parent.width - newAccountButton.width - deleteAccountButton.width
             - Style.space(12)
      label: root.i18n.t("settings.account")
      fontFamily: root.fontFamily
      foreground: root.foreground
      accent: root.accent
      options: root.accountNames
      value: root.phone ? root.phone.activeAccount : ""
      onChanged: function(picked) {
        if (picked !== root.phone.activeAccount)
          root.phone.selectAccount(picked)
      }
    }

    PanelActionButton {
      id: newAccountButton
      anchors.verticalCenter: parent.verticalCenter
      iconText: "\uf067"
      tooltipText: root.i18n.t("settings.accountNew")
      foreground: root.accent
      fontFamily: root.fontFamily
      onClicked: root.creating = true
    }

    PanelActionButton {
      id: deleteAccountButton
      anchors.verticalCenter: parent.verticalCenter
      iconText: "\uf1f8"
      tooltipText: root.accountNames.length > 1
                   ? root.i18n.t("settings.accountDelete")
                   : root.i18n.t("settings.accountLast")
      foreground: root.accountNames.length > 1
                  ? Qt.darker(root.foreground, 1.4) : Qt.darker(root.foreground, 2.2)
      fontFamily: root.fontFamily
      onClicked: {
        if (root.accountNames.length > 1)
          root.phone.deleteAccount(root.phone.activeAccount)
      }
    }
  }

  Text {
    width: parent.width
    visible: !root.creating && root.accountNames.length > 1
    textFormat: Text.PlainText
    text: root.i18n.t("settings.accountHint")
    color: Qt.darker(root.foreground, 1.8)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
    wrapMode: Text.WordWrap
  }

  Row {
    width: parent.width
    visible: root.creating
    spacing: Style.space(6)

    TextField {
      width: parent.width - Style.space(150)
      placeholderText: root.i18n.t("settings.accountName")
      text: root.newName
      foreground: root.foreground
      onTextChanged: root.newName = text
      Keys.onReturnPressed: root.saveNewAccount()
    }

    Button {
      anchors.verticalCenter: parent.verticalCenter
      text: root.i18n.t("settings.save")
      foreground: root.accent
      fontFamily: root.fontFamily
      fontSize: root.fontTokens.caption
      bordered: true
      onClicked: root.saveNewAccount()
    }

    Button {
      anchors.verticalCenter: parent.verticalCenter
      text: root.i18n.t("common.cancel")
      foreground: root.foreground
      fontFamily: root.fontFamily
      fontSize: root.fontTokens.caption
      onClicked: { root.creating = false; root.newName = "" }
    }
  }

  PanelSeparator { width: parent.width; foreground: root.foreground }

  // ----------------------------------------------------------- transport
  //
  // Below the account, because it belongs to whichever one is selected.

  Text {
    textFormat: Text.PlainText
    text: root.i18n.t("settings.transport")
    color: Qt.darker(root.foreground, 1.5)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
  }

  Row {
    spacing: Style.space(6)

    Repeater {
      model: ["udp", "tls"]

      delegate: Button {
        id: transportButton
        required property string modelData
        readonly property bool on: root.transport === transportButton.modelData

        text: transportButton.modelData.toUpperCase()
        foreground: transportButton.on ? root.accent : Qt.darker(root.foreground, 1.9)
        fontFamily: root.fontFamily
        fontSize: root.fontTokens.caption
        bordered: transportButton.on
        selected: transportButton.on
        onClicked: {
          root.transport = transportButton.modelData
          root.saved = false
        }
      }
    }
  }

  Text {
    width: parent.width
    visible: root.transport === "tls"
    textFormat: Text.PlainText
    text: root.i18n.t("settings.transportHint")
    color: Qt.darker(root.foreground, 1.8)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
    wrapMode: Text.WordWrap
  }

  Row {
    width: parent.width
    visible: root.transport === "tls"
    spacing: Style.space(6)

    ToggleSwitch {
      anchors.verticalCenter: parent.verticalCenter
      checked: root.tlsVerify
      foreground: root.foreground
      accent: root.accent
      onToggled: {
        root.tlsVerify = !root.tlsVerify
        root.saved = false
      }
    }

    Column {
      anchors.verticalCenter: parent.verticalCenter
      // The warning is a full sentence and has to wrap, so the column needs a
      // width rather than taking one from its widest line.
      width: parent.width - Style.space(56)
      spacing: 0

      Text {
        width: parent.width
        textFormat: Text.PlainText
        text: root.i18n.t("settings.tlsVerify")
        color: root.foreground
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.caption
        wrapMode: Text.WordWrap
      }

      Text {
        width: parent.width
        visible: !root.tlsVerify
        textFormat: Text.PlainText
        text: root.i18n.t("settings.tlsVerifyOff")
        color: root.urgent
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.caption
        wrapMode: Text.WordWrap
      }
    }
  }

  // Language sits up here with the transport rather than at the bottom: both
  // are set once and then forgotten, and a dropdown keeps it to one row so
  // neither has to be scrolled to.
  Dropdown {
    width: parent.width
    label: root.i18n.t("settings.language")
    fontFamily: root.fontFamily
    foreground: root.foreground
    accent: root.accent
    options: root.languageLabels
    value: root.labelForLanguage(root.language)
    onChanged: function(picked) { root.setLanguage(root.languageForLabel(picked)) }
  }

  PanelSeparator { width: parent.width; foreground: root.foreground }

  Connections {
    target: root.phone
    function onConfigChanged() { root.load() }
  }

  Repeater {
    model: [
      { key: "displayName", label: "settings.displayName", secret: false },
      { key: "username",    label: "settings.username",    secret: false },
      { key: "password",    label: "settings.password",    secret: true },
      { key: "domain",      label: "settings.domain",      secret: false }
    ]

    delegate: Column {
      id: field
      required property var modelData

      width: root.width
      spacing: Style.space(2)

      Text {
        textFormat: Text.PlainText
        text: root.i18n.t(field.modelData.label)
        color: Qt.darker(root.foreground, 1.5)
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.caption
      }

      TextField {
        width: parent.width
        foreground: root.foreground
        password: field.modelData.secret
        placeholderText: field.modelData.secret && root.passwordSet
                         ? root.i18n.t("settings.passwordKept") : ""
        text: root[field.modelData.key]
        onTextChanged: {
          root[field.modelData.key] = text
          root.saved = false
        }
        Keys.onReturnPressed: root.save()
      }
    }
  }

  // ----------------------------------------------------------- advanced

  Button {
    text: (root.showAdvanced ? "\u25be  " : "\u25b8  ") + root.i18n.t("settings.advanced")
    foreground: Qt.darker(root.foreground, 1.4)
    fontFamily: root.fontFamily
    fontSize: root.fontTokens.caption
    onClicked: root.showAdvanced = !root.showAdvanced
  }

  Repeater {
    model: root.showAdvanced ? [
      { key: "proxy", label: "settings.proxy" },
      { key: "stun",  label: "settings.stun" }
    ] : []

    delegate: Column {
      id: advField
      required property var modelData

      width: root.width
      spacing: Style.space(2)

      Text {
        textFormat: Text.PlainText
        text: root.i18n.t(advField.modelData.label)
        color: Qt.darker(root.foreground, 1.5)
        font.family: root.fontFamily
        font.pixelSize: root.fontTokens.caption
      }

      TextField {
        width: parent.width
        foreground: root.foreground
        text: root[advField.modelData.key]
        onTextChanged: {
          root[advField.modelData.key] = text
          root.saved = false
        }
        Keys.onReturnPressed: root.save()
      }
    }
  }

  PanelSeparator { width: parent.width; foreground: root.foreground }

  // ------------------------------------------------------------- codecs

  Text {
    textFormat: Text.PlainText
    text: root.i18n.t("settings.codecs")
    color: Qt.darker(root.foreground, 1.5)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
  }

  Text {
    width: parent.width
    textFormat: Text.PlainText
    text: root.i18n.t("settings.codecsHint")
    color: Qt.darker(root.foreground, 1.8)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
    wrapMode: Text.WordWrap
  }

  Flow {
    width: parent.width
    spacing: Style.space(6)

    Repeater {
      model: root.knownCodecs

      delegate: Button {
        id: codecButton
        required property var modelData
        readonly property bool on: root.enabledCodecs.indexOf(codecButton.modelData.id) !== -1
        readonly property bool last: root.enabledCodecs.length === 1 && codecButton.on

        text: codecButton.modelData.label
        foreground: codecButton.on ? root.accent : Qt.darker(root.foreground, 1.9)
        fontFamily: root.fontFamily
        fontSize: root.fontTokens.caption
        bordered: codecButton.on
        selected: codecButton.on
        // The last one standing cannot be turned off: with an empty list the
        // daemon has nothing to put in an SDP.
        tooltipText: codecButton.last ? root.i18n.t("settings.codecsEmpty") : ""
        onClicked: if (!codecButton.last) root.toggleCodec(codecButton.modelData.id)
      }
    }
  }

  PanelSeparator { width: parent.width; foreground: root.foreground }

  Text {
    width: parent.width
    textFormat: Text.PlainText
    text: root.i18n.t("settings.moreInFile")
    color: Qt.darker(root.foreground, 1.9)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.caption
    wrapMode: Text.WordWrap
  }

  Row {
    spacing: Style.space(8)

    Button {
      text: root.i18n.t("settings.save")
      foreground: root.accent
      fontFamily: root.fontFamily
      bordered: true
      onClicked: root.save()
    }

    Text {
      anchors.verticalCenter: parent.verticalCenter
      visible: root.saved
      textFormat: Text.PlainText
      text: root.i18n.t("settings.saved")
      color: Qt.darker(root.foreground, 1.5)
      font.family: root.fontFamily
      font.pixelSize: root.fontTokens.caption
    }
  }
}
