# Omacall — plugin-side tasks. The daemon has its own Makefile under daemon/.
#
# Everything here is make + POSIX shell + jq. No scripting language is added to
# this project, build included.

PLUGIN_ID   := f360c4.omacall
PLUGIN_DIR  := $(HOME)/.config/omarchy/plugins/$(PLUGIN_ID)
OMARCHY_PATH ?= /usr/share/omarchy
QMLLINT     := /usr/lib/qt6/bin/qmllint
# qmllint resolves `import qs.Commons` by looking for qs/Commons/qmldir on an
# import path. The shell ships those qmldir files but not the `qs` root the
# Quickshell runtime synthesizes, so build one here out of a symlink.
QMLROOT     := .qmllint
# Quickshell's Process and Socket declare signal parameters whose C++ types
# (QProcess::ExitStatus, QLocalSocket::LocalSocketError) are not exported to
# QML, so qmllint cannot resolve them no matter how the handler is written.
# Omarchy's own plugins carry the same warnings. Every other category stays on.
QMLLINT_FLAGS := --signal-handler-parameters disable
REF_LANG    := i18n/en.json

.PHONY: help check i18n-check version-check validate lint dev-install dev-uninstall

help:
	@echo "make check        run every check below"
	@echo "make i18n-check   every language carries exactly the keys of en.json"
	@echo "make version-check manifest, daemon and PKGBUILD agree on the version"
	@echo "make lint         qmllint over the plugin QML"
	@echo "make validate     omarchy plugin validate over the installed copy"
	@echo "make dev-install  copy this working tree to $(PLUGIN_DIR)"
	@echo "make dev-uninstall  remove that copy"

check: i18n-check version-check lint validate

# The version lives in three files that no tool keeps in sync. A release built
# from mismatched ones installs a package whose binary reports a different
# version than the plugin next to it.
version-check:
	@manifest=$$(jq -r '.version' manifest.json); \
	daemon=$$(sed -n 's/^VERSION := //p' daemon/Makefile); \
	pkgbuild=$$(sed -n 's/^pkgver=//p' packaging/PKGBUILD); \
	if [ "$$manifest" = "$$daemon" ] && [ "$$manifest" = "$$pkgbuild" ]; then \
	  echo "version: $$manifest in manifest.json, daemon/Makefile and PKGBUILD"; \
	else \
	  echo "version mismatch: manifest.json=$$manifest daemon/Makefile=$$daemon PKGBUILD=$$pkgbuild" >&2; \
	  exit 1; \
	fi

# English is the fallback, so it is also the reference: any other language that
# adds or drops a key would leave a string untranslatable or silently unused.
i18n-check:
	@status=0; \
	for f in i18n/*.json; do \
	  jq -e . "$$f" >/dev/null 2>&1 || { echo "i18n: not valid JSON: $$f"; status=1; continue; }; \
	  missing=$$(jq -r -n --slurpfile a "$$f" --slurpfile b $(REF_LANG) '(($$b[0]|keys) - ($$a[0]|keys))[]'); \
	  extra=$$(jq -r -n --slurpfile a "$$f" --slurpfile b $(REF_LANG) '(($$a[0]|keys) - ($$b[0]|keys))[]'); \
	  empty=$$(jq -r 'to_entries[] | select((.value | type) != "string" or (.value | length) == 0) | .key' "$$f"); \
	  for k in $$missing; do echo "i18n: $$f is missing key '$$k'"; status=1; done; \
	  for k in $$extra; do echo "i18n: $$f has key '$$k' that $(REF_LANG) does not"; status=1; done; \
	  for k in $$empty; do echo "i18n: $$f has an empty value for '$$k'"; status=1; done; \
	done; \
	[ $$status -eq 0 ] && echo "i18n: $$(ls i18n/*.json | wc -l) languages, $$(jq 'keys|length' $(REF_LANG)) keys, all matching"; \
	exit $$status

lint:
	@mkdir -p $(QMLROOT)
	@ln -sfn "$(OMARCHY_PATH)/shell" $(QMLROOT)/qs
	@$(QMLLINT) $(QMLLINT_FLAGS) -I $(QMLROOT) -I "$(OMARCHY_PATH)/shell" \
	    I18n.qml Service.qml BarWidget.qml components/*.qml
	@echo "qmllint: clean"

validate:
	@omarchy plugin validate "$(PLUGIN_DIR)"
	@echo "manifest: valid"

# Mirror the working tree the way `omarchy plugin add` would clone it: only the
# files git would carry, so an ignored build artifact never reaches the shell.
dev-install:
	@mkdir -p "$(PLUGIN_DIR)"
	@git ls-files -co --exclude-standard -z | tar --null -T - -cf - | tar -xf - -C "$(PLUGIN_DIR)"
	@echo "installed to $(PLUGIN_DIR)"

dev-uninstall:
	@rm -rf "$(PLUGIN_DIR)"
	@echo "removed $(PLUGIN_DIR)"
