.pragma library

// Pure formatting helpers shared by the panel. No QML types in here, so the
// logic stays testable by reading it and cheap to reuse from any component.

// Nerd Font glyphs (Material Design set) for the phone in its various moods.
var Glyph = {
  phone: "󰏲",        // nf-md-phone
  inTalk: "󰏷",       // nf-md-phone_in_talk
  incoming: "󰏸",     // nf-md-phone_incoming
  hangup: "󰏳",       // nf-md-phone_hangup
  outgoing: "󰏹",     // nf-md-phone_outgoing
  missed: "󰏶"        // nf-md-phone_missed
}

// Seconds -> "mm:ss", or "h:mm:ss" once a call passes the hour.
function duration(seconds) {
  var total = Math.max(0, Math.floor(Number(seconds) || 0))
  var s = total % 60
  var m = Math.floor(total / 60) % 60
  var h = Math.floor(total / 3600)
  var pad = function(n) { return n < 10 ? "0" + n : String(n) }
  return h > 0 ? h + ":" + pad(m) + ":" + pad(s) : pad(m) + ":" + pad(s)
}

// Localised, compact timestamp for a history row: the time for calls from
// today, the weekday for this past week, the date beyond that. `localeName` is
// an i18n code such as "pt-BR", so month and weekday names follow the panel's
// language rather than the system's.
//
// `shortFormat` is Locale.ShortFormat, passed in by the caller: a
// `.pragma library` script has Qt but not the QML enums, and hardcoding the
// number would be a silent breakage the day Qt renumbers them.
function historyStamp(unixSeconds, localeName, shortFormat) {
  var when = new Date(Number(unixSeconds) * 1000)
  var locale = Qt.locale(String(localeName || "en").replace("-", "_"))
  var now = new Date()
  var sameDay = when.toDateString() === now.toDateString()
  var ageDays = (now.getTime() - when.getTime()) / 86400000

  if (sameDay)
    return when.toLocaleTimeString(locale, shortFormat)
  if (ageDays < 7)
    return when.toLocaleDateString(locale, "ddd") + " " + when.toLocaleTimeString(locale, shortFormat)
  return when.toLocaleDateString(locale, shortFormat)
}

// Collapse the daemon's fine-grained state into the handful of situations the
// bar icon and the panel header actually draw differently.
function statusKey(phoneState, connected) {
  if (phoneState === "daemon_missing") return "daemonMissing"
  if (!connected) return "disconnected"
  if (phoneState === "incoming") return "incoming"
  if (phoneState === "calling" || phoneState === "early"
      || phoneState === "connected" || phoneState === "disconnecting") return "inCall"
  if (phoneState === "registered") return "registered"
  return "notRegistered"
}

// The glyph the bar shows for a given situation.
function statusGlyph(key) {
  switch (key) {
  case "incoming": return Glyph.incoming
  case "inCall":   return Glyph.inTalk
  default:         return Glyph.phone
  }
}
