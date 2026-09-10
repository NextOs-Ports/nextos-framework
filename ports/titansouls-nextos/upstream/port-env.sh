#!/usr/bin/env bash
# Titan Souls opens "LOG.txt" itself.  On case-insensitive ROM filesystems that
# aliases nxbootstrap's "log.txt" and unlinks the launch log while it is still
# open.  Move only the game phase to a distinct filename after NXExtract has
# completed, preserving both setup and runtime diagnostics on every firmware.
if [ -n "${GAMEDIR:-}" ] && [ -d "$GAMEDIR" ]; then
  [ -s "$GAMEDIR/titansouls-bootstrap.log" ] &&
    mv -f "$GAMEDIR/titansouls-bootstrap.log" \
      "$GAMEDIR/titansouls-bootstrap.prev.log" 2>/dev/null || true
  [ -s "$GAMEDIR/log.txt" ] &&
    mv -f "$GAMEDIR/log.txt" "$GAMEDIR/titansouls-bootstrap.log" \
      2>/dev/null || true
  [ -s "$GAMEDIR/titansouls-runtime.log" ] &&
    mv -f "$GAMEDIR/titansouls-runtime.log" \
      "$GAMEDIR/titansouls-runtime.prev.log" 2>/dev/null || true
  exec >"$GAMEDIR/titansouls-runtime.log" 2>&1
fi
