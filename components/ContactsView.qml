pragma ComponentBehavior: Bound

import QtQuick
import qs.Commons
import qs.Ui

// Contacts, filtered as you type. A row calls; the pencil edits; the editor is
// the same form for a new contact and an existing one.
Column {
  id: root

  required property var phone
  required property var i18n
  property color foreground: Color.foreground
  property color accent: Color.accent
  property color urgent: Color.urgent
  property string fontFamily: fontTokens.family
  readonly property var fontTokens: Style.font

  signal callRequested(string number)

  property string query: ""
  property bool editing: false
  property string editId: ""
  property string editName: ""
  property string editNumber: ""

  readonly property var filtered: {
    var all = phone ? phone.contacts : []
    var needle = query.toLowerCase().trim()
    if (needle === "")
      return all
    var out = []
    for (var i = 0; i < all.length; i++) {
      var c = all[i]
      if (String(c.name).toLowerCase().indexOf(needle) !== -1
          || String(c.number).toLowerCase().indexOf(needle) !== -1)
        out.push(c)
    }
    return out
  }

  function startAdd() {
    editId = ""
    editName = ""
    editNumber = query
    editing = true
  }

  function startEdit(contact) {
    editId = contact.id
    editName = contact.name
    editNumber = contact.number
    editing = true
  }

  function save() {
    if (editName.trim() === "" || editNumber.trim() === "")
      return
    var payload = { name: editName.trim(), number: editNumber.trim() }
    if (editId !== "")
      payload.id = editId
    phone.upsertContact(payload)
    editing = false
  }

  spacing: Style.space(8)

  // ------------------------------------------------------------- editor

  Column {
    width: parent.width
    visible: root.editing
    spacing: Style.space(6)

    TextField {
      id: nameField
      width: parent.width
      placeholderText: root.i18n.t("contacts.name")
      text: root.editName
      foreground: root.foreground
      onTextChanged: root.editName = text
      Keys.onReturnPressed: numberField.forceActiveFocus()
    }

    TextField {
      id: numberField
      width: parent.width
      placeholderText: root.i18n.t("contacts.number")
      text: root.editNumber
      foreground: root.foreground
      onTextChanged: root.editNumber = text
      Keys.onReturnPressed: root.save()
    }

    Row {
      spacing: Style.space(6)

      Button {
        text: root.i18n.t("settings.save")
        foreground: root.accent
        fontFamily: root.fontFamily
        bordered: true
        onClicked: root.save()
      }

      Button {
        text: root.i18n.t("common.cancel")
        foreground: root.foreground
        fontFamily: root.fontFamily
        bordered: true
        onClicked: root.editing = false
      }

      Button {
        visible: root.editId !== ""
        text: root.i18n.t("common.delete")
        foreground: root.urgent
        fontFamily: root.fontFamily
        bordered: true
        onClicked: {
          root.phone.deleteContact(root.editId)
          root.editing = false
        }
      }
    }
  }

  // --------------------------------------------------------------- list

  Row {
    width: parent.width
    visible: !root.editing
    spacing: Style.space(6)

    TextField {
      id: searchField
      width: parent.width - addButton.width - Style.space(6)
      placeholderText: root.i18n.t("contacts.search")
      text: root.query
      foreground: root.foreground
      onTextChanged: root.query = text
    }

    PanelActionButton {
      id: addButton
      anchors.verticalCenter: parent.verticalCenter
      iconText: ""
      tooltipText: root.i18n.t("contacts.add")
      foreground: root.accent
      fontFamily: root.fontFamily
      onClicked: root.startAdd()
    }
  }

  Text {
    width: parent.width
    visible: !root.editing && root.filtered.length === 0
    textFormat: Text.PlainText
    text: root.query === "" ? root.i18n.t("contacts.empty") : root.i18n.t("contacts.noMatch")
    color: Qt.darker(root.foreground, 1.6)
    font.family: root.fontFamily
    font.pixelSize: root.fontTokens.body
  }

  Repeater {
    model: root.editing ? [] : root.filtered

    delegate: Item {
      id: row
      required property var modelData

      width: root.width
      implicitHeight: Style.space(34)

      Button {
        anchors.left: parent.left
        anchors.right: editButton.left
        anchors.rightMargin: Style.space(4)
        height: parent.height
        foreground: root.foreground
        fontFamily: root.fontFamily
        onClicked: root.callRequested(String(row.modelData.number))

        Row {
          anchors.left: parent.left
          anchors.verticalCenter: parent.verticalCenter
          anchors.leftMargin: Style.space(8)
          spacing: Style.space(8)

          Text {
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: row.modelData.name
            color: root.foreground
            font.family: root.fontFamily
            font.pixelSize: root.fontTokens.body
          }

          Text {
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: row.modelData.number
            color: Qt.darker(root.foreground, 1.7)
            font.family: root.fontFamily
            font.pixelSize: root.fontTokens.caption
          }
        }
      }

      PanelActionButton {
        id: editButton
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        iconText: ""
        tooltipText: root.i18n.t("contacts.edit")
        foreground: Qt.darker(root.foreground, 1.4)
        fontFamily: root.fontFamily
        onClicked: root.startEdit(row.modelData)
      }
    }
  }
}
