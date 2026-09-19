pragma ComponentBehavior: Bound

import QtQuick
import Quickshell
import Quickshell.Io

// Headless singleton that owns the phone for the whole shell.
//
// The bar exists once per monitor, so the widget cannot be the thing that
// starts the daemon — that would give you one daemon per screen. This service
// is mounted once, supervises `omacalld`, holds the socket, and Panel.qml only
// reads from it.
//
// Everything crossing the socket is NDJSON, one object per line, as described
// in docs/protocol.md. Replies are matched to requests by id; events arrive
// unsolicited and update state in place.
Item {
  id: root

  // Injected by the host when it mounts the plugin.
  property var shell: null
  property var manifest: null

  readonly property string version: "0.1.0"
  readonly property string socketPath: (Quickshell.env("XDG_RUNTIME_DIR") || "/tmp") + "/omacall.sock"

  // idle | registering | registered | reg_failed | incoming | calling | early |
  // connected | disconnecting, plus daemon_missing when omacalld is not there.
  // Named phoneState, not state: every QML Item already has a `state`.
  property string phoneState: "daemon_missing"

  property bool daemonRunning: false
  // A binding, not a signal handler: Socket does not emit
  // connectionStateChanged for the initial connection when `connected: true`
  // is set declaratively, so a handler alone never learns it worked — and the
  // retry timer then tears down a socket that was already live.
  // Read through `sock`, which is a var, so qmllint does not have to know the
  // members of a type the Loader only knows at runtime.
  readonly property var sock: socketLoader.item
  readonly property bool connected: sock && sock.connected ? true : false
  property bool registered: false
  property string accountUri: ""
  property string regStatusText: ""
  property int regStatusCode: 0

  // The live call as an object, or null.
  property var activeCall: null
  property bool muted: false

  property var history: []
  property var contacts: []
  // Saved accounts, without passwords. Only one is registered at a time.
  property var accounts: []
  property string activeAccount: ""
  property var config: null
  property string lastError: ""

  // True once the daemon has failed to start enough times that the binary is
  // probably not installed, which is a different message than "starting".
  property bool daemonMissing: false

  signal incomingCall(var call)

  property I18n i18n: I18n {}

  // ------------------------------------------------------------ requests

  property var pendingCallbacks: ({})
  property int nextRequestId: 1

  function send(cmd, args, callback) {
    if (!sock || !sock.connected) {
      root.lastError = "not connected to the daemon"
      if (callback)
        callback(null, { code: "no_daemon", message: root.lastError })
      return -1
    }

    var id = root.nextRequestId++
    var request = { id: id, cmd: cmd }
    if (args)
      request.args = args
    if (callback)
      root.pendingCallbacks[id] = callback

    sock.write(JSON.stringify(request) + "\n")
    sock.flush()
    return id
  }

  function placeCall(dest) { return send("call", { dest: String(dest) }) >= 0 }
  function answer() { return send("answer", null) >= 0 }
  function hangup() { return send("hangup", null) >= 0 }
  function dtmf(digits) { return send("dtmf", { digits: String(digits) }) >= 0 }
  // The click while dialling, before there is a call to send a digit to.
  function keyTone(digit) { return send("tone", { digits: String(digit) }) >= 0 }
  function setMuted(enabled) { return send("mute", { enabled: !!enabled }) >= 0 }
  function refreshHistory() { send("history.list", { limit: 100 }) }
  function refreshContacts() { send("contacts.list", null) }
  function clearHistory() { send("history.clear", null) }
  function upsertContact(contact, callback) { send("contacts.upsert", contact, callback) }
  function deleteContact(id) { send("contacts.delete", { id: String(id) }) }
  function getConfig(callback) { send("config.get", null, callback) }
  function refreshAccounts() {
    send("accounts.list", null, function(result) {
      if (!result)
        return
      root.accounts = result.accounts || []
      root.activeAccount = result.active || ""
    })
  }
  function selectAccount(name) { send("accounts.select", { name: String(name) }) }
  function upsertAccount(account, callback) { send("accounts.upsert", account, callback) }
  function deleteAccount(name) { send("accounts.delete", { name: String(name) }) }
  function setConfig(patch, callback) { send("config.set", patch, callback) }

  // ------------------------------------------------------------- incoming

  function handleLine(line) {
    var message
    try {
      message = JSON.parse(line)
    } catch (e) {
      console.warn("Omacall: unparseable line from the daemon:", line)
      return
    }

    if (message.event !== undefined) {
      handleEvent(message.event, message.data || {})
      return
    }

    var callback = root.pendingCallbacks[message.id]
    if (callback) {
      delete root.pendingCallbacks[message.id]
      callback(message.ok ? message.result : null, message.ok ? null : message.error)
    }
    if (!message.ok && message.error)
      root.lastError = message.error.message || message.error.code || ""
    else if (message.ok)
      applyResult(message.result)
  }

  // hello and status share a shape, so one function absorbs both.
  function applyResult(result) {
    if (!result || typeof result !== "object")
      return

    if (result.state !== undefined)
      root.phoneState = result.state
    if (result.registered !== undefined)
      root.registered = !!result.registered
    if (result.account) {
      root.accountUri = result.account.uri || ""
      root.regStatusText = result.account.status_text || ""
      root.regStatusCode = result.account.status_code || 0
    }
    if (result.call !== undefined) {
      root.activeCall = result.call
      root.muted = result.call ? !!result.call.muted : false
    }
    if (result.history_tail !== undefined)
      root.history = result.history_tail
    if (result.items !== undefined)
      root.history = result.items
    if (result.contacts !== undefined)
      root.contacts = result.contacts
    // config.get answers with the config object itself, which has no other
    // field we watch for; the account block is the tell.
    if (result.codecs !== undefined && result.account !== undefined)
      root.config = result
  }

  function handleEvent(name, data) {
    switch (name) {
    case "reg.state":
      root.registered = !!data.registered
      root.regStatusCode = data.status_code || 0
      root.regStatusText = data.status_text || ""
      root.phoneState = data.registered ? "registered" : (root.phoneState === "idle" ? "idle" : "reg_failed")
      break

    case "call.incoming":
      root.activeCall = {
        call_id: data.call_id,
        direction: "in",
        state: "incoming",
        number: data.number || "",
        display: data.display || data.contact_name || "",
        remote: data.remote || "",
        muted: false,
        started_at: null
      }
      root.phoneState = "incoming"
      root.incomingCall(root.activeCall)
      notifyIncoming(root.activeCall)
      break

    case "call.state":
      if (!root.activeCall || root.activeCall.call_id !== data.call_id) {
        root.activeCall = {
          call_id: data.call_id,
          direction: "out",
          number: data.number || "",
          display: data.display || "",
          remote: data.remote || "",
          muted: false
        }
      }
      // Rebuild the object rather than mutating it: QML only re-evaluates
      // bindings on a property write, not on a field changing underneath.
      root.activeCall = Object.assign({}, root.activeCall, {
        state: data.state,
        started_at: data.started_at || root.activeCall.started_at || null
      })
      root.phoneState = data.state
      break

    case "media.state":
      if (root.activeCall && root.activeCall.call_id === data.call_id)
        root.activeCall = Object.assign({}, root.activeCall, { codec: data.codec || "" })
      break

    case "call.mute":
      root.muted = !!data.muted
      if (root.activeCall)
        root.activeCall = Object.assign({}, root.activeCall, { muted: root.muted })
      break

    case "call.ended":
      root.activeCall = null
      root.muted = false
      root.phoneState = root.registered ? "registered" : "idle"
      refreshHistory()
      break

    case "history.changed":
      refreshHistory()
      break

    case "contacts.changed":
      refreshContacts()
      break

    case "config.changed":
      getConfig(function(result) { if (result) root.config = result })
      refreshAccounts()
      break

    case "error":
      root.lastError = data.message || data.code || ""
      console.warn("Omacall daemon error:", root.lastError)
      break
    }
  }

  function notifyIncoming(call) {
    var who = call.display && call.display !== "" ? call.display : call.number
    Quickshell.execDetached([
      "notify-send", "-a", "Omacall", "-i", "call-start",
      root.i18n.t("notify.incoming.title"),
      root.i18n.t("notify.incoming.body", { who: who })
    ])
  }

  // ------------------------------------------------------------ the daemon

  property int restartDelay: 1000

  Process {
    id: daemon
    command: ["omacalld"]
    running: true

    stderr: SplitParser {
      onRead: function(line) {
        // The daemon's own log. Worth keeping in the shell's journal, since a
        // registration failure explains an icon that never lights up.
        if (line && line.length > 0)
          console.log("omacalld:", line)
      }
    }

    onRunningChanged: root.daemonRunning = daemon.running

    onExited: function(exitCode) {
      root.daemonRunning = false

      // Exit 3 is "another omacalld already owns the socket", which happens
      // during a shell restart while the old one is still shutting down.
      // Retrying is right; treating it as a missing binary is not.
      if (exitCode === 3) {
        console.log("Omacall: another daemon holds the socket, retrying")
      } else if (exitCode !== 0) {
        console.warn("Omacall: omacalld exited with", exitCode)
      }

      restartTimer.interval = root.restartDelay
      restartTimer.restart()
      // 1s, 2s, 4s … capped at 30s, so a missing binary does not spin.
      root.restartDelay = Math.min(root.restartDelay * 2, 30000)
      if (root.restartDelay >= 8000)
        root.daemonMissing = true
    }
  }

  Timer {
    id: restartTimer
    repeat: false
    onTriggered: if (!daemon.running) daemon.running = true
  }

  // --------------------------------------------------------------- socket
  //
  // A Socket starts connecting the moment it has a path, which on a shell
  // restart is a second or two before the daemon has created it. That first
  // attempt fails with ConnectionRefused, and after it the object stays dead:
  // writing `connected` again does nothing, whatever order the writes are in.
  // So reconnecting means building a new Socket, which is what the Loader is
  // for — dropping and reloading it is a genuinely fresh attempt.

  onConnectedChanged: {
    if (root.connected) {
      root.daemonMissing = false
      root.restartDelay = 1000
      root.lastError = ""
      // hello returns the whole world in one round trip.
      root.send("hello", { client: "plugin", version: root.version }, function(result) {
        if (result)
          root.applyResult(result)
      })
      root.refreshContacts()
      root.refreshAccounts()
      root.getConfig(function(result) { if (result) root.config = result })
    } else {
      root.phoneState = "daemon_missing"
      root.registered = false
      root.activeCall = null
      root.pendingCallbacks = ({})
    }
  }

  // Disabling the plugin destroys this service, and Quickshell tears the
  // Process down without giving the daemon time to handle a signal: the socket
  // is left behind and, worse, the account stays registered, so the registrar
  // keeps sending calls to a phone that is gone until the registration expires.
  // Asking it to quit over the socket first gives it the chance to un-REGISTER,
  // hang up anything live and clean up after itself.
  Component.onDestruction: {
    if (root.sock && root.sock.connected) {
      root.sock.write(JSON.stringify({ id: 0, cmd: "quit" }) + "\n")
      root.sock.flush()
    }
  }

  Loader {
    id: socketLoader
    active: true

    sourceComponent: Socket {
      path: root.socketPath
      connected: true

      parser: SplitParser {
        onRead: function(line) { root.handleLine(line) }
      }
    }
  }

  // One second is fast enough that the panel feels live on a shell restart,
  // and slow enough to be invisible while the daemon is simply absent.
  Timer {
    interval: 1000
    running: !root.connected
    repeat: true
    onTriggered: {
      socketLoader.active = false
      Qt.callLater(function() { socketLoader.active = true })
    }
  }
}
