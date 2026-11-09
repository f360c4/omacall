# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- The bar widget failed to load on current Omarchy with "Composite Singleton
  Type I18n is not creatable": Omarchy now ships an `I18n` singleton in
  `qs.Commons`, which shadowed the plugin's own `I18n.qml`. The plugin's
  dictionary is renamed to `OmacallI18n.qml`.

## [0.1.2] - 2026-09-23

### Fixed

- A client that sent requests without reading the replies could make the
  daemon buffer them without limit. Replies waiting on a client are now capped
  at 1 MiB (2 MiB across all clients); past 256 KiB the daemon stops reading
  from that client until it drains, and past the cap it drops it.
  `make test` floods a non-reading client and checks that memory stays flat.

## [0.1.1] - 2026-09-20

### Changed

- The package pins its source to a published release tarball verified by
  sha256, instead of fetching a Git tag with the checksum skipped. A tag can be
  moved; a checksummed archive of a known commit cannot.

## [0.1.0] - 2026-09-18

### Added

- Omarchy plugin skeleton: manifest, service and bar widget, with the phone
  icon and a panel header that reports the daemon's state.
- Translations in English, Brazilian Portuguese, Spanish, Italian, French,
  German and Japanese, checked for key parity by `make i18n-check`.
- `omacalld`, a SIP daemon on pjproject 2.17 with G.729, Opus, PCMA and PCMU,
  speaking NDJSON over a Unix socket. Codec priority follows the order of the
  `codecs` array in the configuration, and codecs left out of it are disabled.
- `omacall`, a CLI for status, history, contacts, configuration and
  registration.
- An Arch package that builds pjproject statically and refuses to finish if
  G.729 did not make it into the archive.
- Registration retries every 20 seconds after a failure instead of pjsua's
  default of 300, so the phone comes back a few seconds after the network does
  rather than five minutes later.
- An Omarchy panel: dialer with keypad, call screen with timer, mute and DTMF,
  contacts with incremental search and editing, call history that dials back,
  and a settings form for the SIP account. One daemon per session no matter how
  many monitors the bar is drawn on.
- Several accounts can be configured and switched from the panel; one registers
  at a time. A configuration written before this still works.
- SIP over TLS, selected with `account.transport`. Signalling only; the media
  is still plain RTP.
- Outgoing calls, with local ringback from a tone generator, mute, and DTMF over
  RFC 4733 or SIP INFO. Every call is written to the history with its direction,
  duration and outcome.
