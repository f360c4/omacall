# The Omacall control protocol

`omacalld` listens on a Unix stream socket at `$XDG_RUNTIME_DIR/omacall.sock`,
mode 0600. If `XDG_RUNTIME_DIR` is unset it falls back to `/run/user/<uid>`.

The wire format is **NDJSON**: one JSON object per line, terminated by `\n`,
UTF-8, with no newlines inside an object. Several clients can be connected at
once — the panel and the CLI usually both are. Replies go only to the client
that asked; events go to everyone.

This document is the contract. The panel, the CLI and anything else you write
against the socket can rely on it.

## Envelope

A request:

```json
{"id": 7, "cmd": "call", "args": {"dest": "1002"}}
```

A reply, to that client only:

```json
{"id": 7, "ok": true, "result": {"call_id": 3}}
{"id": 7, "ok": false, "error": {"code": "not_registered", "message": "..."}}
```

An event, to every client:

```json
{"event": "call.state", "data": {"call_id": 3, "state": "connected"}}
```

`id` is an integer the client picks and the daemon echoes back. An unknown
command answers `ok: false` with code `unknown_cmd`. A line that is not valid
JSON answers `bad_json` without dropping the connection — one bad line costs
one bad line.

## Commands

| cmd | args | result |
|---|---|---|
| `hello` | `{"client":"plugin\|cli","version":"x.y.z"}` | full snapshot: `daemon_version`, `protocol`, `state`, `registered`, `account`, `call`, `history_tail` (20 newest), `contacts` |
| `status` | — | `{"state","registered","account":{"uri","status_code","status_text"},"call": object or null}` |
| `call` | `{"dest":"1002"}` or `{"dest":"sip:1002@host"}` | `{"call_id":n}`; errors: `no_account`, `not_registered`, `busy` |
| `answer` | `{"call_id":n}` optional | `{}`; error `no_incoming` |
| `hangup` | `{"call_id":n}` optional | `{}`; error `no_call` |
| `dtmf` | `{"digits":"123#"}` | `{}`; error `not_connected`. Valid digits: `0-9 * # A-D` |
| `tone` | `{"digits":"5"}` | `{}`; plays the key tone on this machine's speaker only, for feedback while dialling. No call required |
| `mute` | `{"enabled":true}` | `{"muted":bool}` |
| `history.list` | `{"limit":50,"offset":0}` | `{"items":[…],"total":n}`, newest first |
| `history.clear` | — | `{}` |
| `contacts.list` | — | `{"contacts":[…]}`, sorted by name |
| `contacts.upsert` | `{"id":"optional","name":"…","number":"…"}` | `{"contact":{…}}` |
| `contacts.delete` | `{"id":"…"}` | `{}` |
| `config.get` | — | the configuration **without** `password`, which is replaced by `"password_set": true\|false` |
| `config.set` | a partial config object, merged per section | `{}`; writes the file 0600 and re-registers if the account moved |
| `accounts.list` | — | `{"accounts":[{"name","username","domain","transport","password_set","active"}],"active":"…"}`; never returns a password |
| `accounts.select` | `{"name":"…"}` | `{}`; drops the current registration and registers this one |
| `accounts.upsert` | `{"name":"…", …account fields…}` | `{}`; creates or edits a saved account |
| `accounts.delete` | `{"name":"…"}` | `{}`; refuses to delete the last one |
| `register` | `{"enabled":true\|false}` | `{}`; forces a REGISTER or un-REGISTER |
| `quit` | — | `{}`; un-REGISTERs and stops the daemon |

A `dest` without a scheme becomes `sip:<dest>@<account.domain>`.

## Events

| event | data |
|---|---|
| `reg.state` | `{"registered":bool,"status_code":200,"status_text":"OK","expires":300}` |
| `call.incoming` | `{"call_id","remote","display","number","contact_name","ts"}` |
| `call.state` | `{"call_id","state":"calling\|early\|connected\|disconnecting","remote","number","display","started_at": unix or null}` |
| `call.ended` | `{"call_id","direction":"in\|out","number","display","reason","status_code","duration_sec","ts"}` |
| `call.mute` | `{"muted":bool}` |
| `media.state` | `{"call_id","active":bool,"codec":"G729"}` |
| `history.changed` | `{}` — the client re-reads with `history.list` |
| `contacts.changed` | `{}` |
| `config.changed` | `{}` |
| `error` | `{"code","message"}` for asynchronous failures |

`reason` on `call.ended` is one of `completed`, `missed`, `rejected`, `busy`,
`failed`, `cancelled`.

## Stored objects

A history entry, one JSON object per line in
`~/.local/share/omacall/history.jsonl`:

```json
{"ts":1758200000,"direction":"in","number":"1002","display":"Fulano","reason":"completed","duration_sec":83,"status_code":200}
```

A contact, in `~/.local/share/omacall/contacts.json`:

```json
{"id":"c_006aad851e85fc33c5","name":"Fulano","number":"1002","created_at":1758200000}
```

The daemon generates `id` as `c_` followed by the creation time and four random
bytes, all hex.

## Error codes

`bad_json`, `unknown_cmd`, `bad_args`, `no_account`, `not_registered`, `busy`,
`no_call`, `no_incoming`, `not_connected`, `io`, `internal`.

Messages are always English: they are for a log, not for a user. A client that
shows errors to people translates them from `code`.

## Configuration

`~/.config/omacall/config.json`, mode 0600. `config.json.example` in the
repository root is a complete, commented copy.

A file written before accounts were a list — one `account` object and no
`accounts` — is read as a single account called `Default`, so nothing has to be
edited by hand.

The daemon also reports the active account under `account` and `tls`, resolved,
so a client that only cares about "the account" does not have to walk the list.

| field | meaning |
|---|---|
| `accounts[]` | the saved accounts, each with its own `name`, SIP fields and `tls` block. **Only one registers at a time** — this is a way to keep several servers configured, not multi-account |
| `active_account` | the `name` of the account to register. An unknown name falls back to the first, with a warning |
| `account.username`, `account.password`, `account.domain` | the SIP account. `auth_username` only when it differs from `username` |
| `account.proxy` | outbound proxy, when the provider needs one |
| `account.port` | local SIP port; 0 picks an ephemeral one. With TLS, 5060 is read as "the default" and becomes 5061 |
| `account.transport` | `udp` (default) or `tls`. TLS encrypts the signalling only — the audio is still plain RTP |
| `tls.verify_server` | whether the server's certificate has to check out. `false` is for a lab with a self-signed certificate and is logged as a warning |
| `tls.ca_file` | a CA bundle, when the certificate is not signed by anything the system store knows |
| `account.register_expires` | requested registration lifetime; the registrar may shorten it |
| `codecs` | negotiation order, highest first. A codec left out of this list is disabled outright |
| `audio.capture`, `audio.playback` | ALSA device names; `default` is PipeWire |
| `audio.echo_cancel` | the WebRTC echo canceller |
| `audio.ring_volume` | 0.0–1.0, for the generated ring and ringback |
| `nat.stun_server` | needed when the registrar does not return `rport` (see the README) |
| `nat.keepalive_sec` | how often to poke the NAT binding open |
| `dtmf_method` | `rfc4733` (default), `rfc2833` (a synonym), `info` or `inband` |
| `ring_timeout_sec` | how long an unanswered call rings before it becomes a missed call |
| `log_level` | pjsip log verbosity, 0–6, written to `~/.local/state/omacall/pjsip.log` |

## Versioning

`hello` returns `daemon_version` and `protocol`. This document describes
protocol **1**. An incompatible change increments it, and a client should say
so rather than misbehave.
