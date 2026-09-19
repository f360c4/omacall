# How Omacall is put together

Three pieces: a daemon, a CLI, and a shell plugin.

```
 Hyprland keybind ──► omarchy-shell f360c4.omacall answer ─┐
 omacall (CLI) ─────► $XDG_RUNTIME_DIR/omacall.sock ─┐     │
                                                     ▼     ▼
 ┌──────────────── omarchy-shell (Quickshell) ──────────────┐
 │  plugin f360c4.omacall                                   │
 │   ├─ Service.qml  (kind: service, keepLoaded)            │
 │   │    • Process { omacalld }  supervises the daemon     │
 │   │    • Socket  { omacall.sock } NDJSON ◄──► state      │
 │   └─ Panel.qml    (kind: bar-widget) icon + panel        │
 │        • reads state through shell.serviceFor(id)        │
 │        • dialer, contacts, history, call screen, DTMF    │
 └──────────────────────────────────────────────────────────┘
                              │ Unix socket, NDJSON
                              ▼
 ┌──────────── omacalld (C, pjsua) ─────────────┐
 │  1 SIP account · UDP · G.729/Opus/PCMA/PCMU  │
 │  ALSA "default" (PipeWire) · DTMF            │
 │  ~/.config/omacall/config.json (0600)        │
 │  ~/.local/share/omacall/{history,contacts}   │
 └──────────────────────────────────────────────┘
```

## Why the daemon is a separate process

The Omarchy plugin guide is explicit that plugins run unsandboxed inside the
shell process. A SIP stack with real-time audio threads does not belong there:
a crash in it would take the whole desktop shell with it.

The bar also exists once per monitor. If the widget started the daemon you
would get one daemon per screen. So the `Process` lives in the **service**,
which is mounted once, and the widget only reads state. `keepLoaded: true`
keeps the service alive across plugin hot-reloads, so editing the panel's QML
does not drop a call in progress.

A daemon behind a socket also means keybinds, scripts and tests work without
the UI being open at all.

## Threads

pjsua runs its own threads and calls back from them. The rule the daemon holds
to: **a callback copies what it needs into a flat struct, drops it in a queue,
and writes one byte to a pipe.** Nothing else. JSON, disk writes and client
sockets are only ever touched by the main loop, which is a `poll()` over the
listening socket, the connected clients and that pipe.

The queue is a fixed array with a mutex, so posting an event cannot fail for
lack of memory on the media thread. A full queue drops the event with a warning
rather than blocking audio.

## One account registered at a time

Several accounts can be configured and switched between, which is not the same
as multi-account: exactly one is registered, and selecting another drops the
first. That keeps the daemon's state machine single-valued — one registration,
one call, one set of media — while still letting someone keep a work PBX and a
lab server side by side.

Transports are opened as they are needed and then kept: switching from a UDP
account to a TLS one opens the TLS transport on the spot, because an account
whose URI names a transport that does not exist is refused outright.

## One call at a time

By design. A second INVITE arriving during a call is answered `486 Busy Here`
in the callback — not queued for a decision that has already been made — and
written to the history as a missed call. Call waiting, transfer and conferences
are out of scope for v1, and not stubbed.

## Audio

The conference bridge and the sound device both run at 16 kHz. They do not have
to in general, but the WebRTC echo canceller is created for the bridge's rate
and then handed the device's frames: at a 16k bridge against a 48k device it
aborts the process on the first frame of every call. 16 kHz is the right rate
for telephony anyway — every codec in v1 is 8 or 16 kHz — and PipeWire
resamples to its own 48k without complaint.

Ring and ringback are synthesized by a tone generator rather than shipped as
audio files, so they are never missing and never the wrong sample rate. Local
ringback is suppressed when the far end sends early media of its own.

Mute disconnects the microphone leg only: the sound device stops feeding the
call while the call keeps feeding the speaker.

## Transport

UDP by default, TLS when `account.transport` says so. Both carry the same SIP;
TLS wraps the signalling and nothing else. **The audio is plain RTP either
way** — encrypting media is SRTP, which Omacall does not do, so a PBX
configured to require it will refuse the call.

With TLS the account's URIs carry `;transport=tls`. Without that, pjsip
resolves the destination the usual way and sends over UDP even with a TLS
transport open, which looks like a configuration that is being ignored.

## NAT

The SDP has to carry an address the far end can reach. Omacall asks the
registrar first — most return `rport` and `received` in the REGISTER response
(RFC 3581), and `allow_sdp_nat_rewrite` takes the public address from there
with nothing to configure. Registrars that do not implement it leave no way to
learn the address, and then a STUN server is required. The daemon warns at
startup when it has a private address and no STUN configured.

## Files

| path | owner | contents |
|---|---|---|
| `~/.config/omacall/config.json` | user and daemon, 0600 | account, codecs, audio, NAT |
| `~/.local/share/omacall/history.jsonl` | daemon | one call per line, appended |
| `~/.local/share/omacall/contacts.json` | daemon | rewritten atomically |
| `~/.local/state/omacall/pjsip.log` | daemon | pjsip's own log |
| `$XDG_RUNTIME_DIR/omacall.sock` | daemon, 0600 | the control socket |

Omacall writes nothing else. It does not touch your firewall, your Hyprland
bindings, or anything in `~/.config/omarchy/` beyond the widget entry that
`omarchy plugin enable` creates itself.
