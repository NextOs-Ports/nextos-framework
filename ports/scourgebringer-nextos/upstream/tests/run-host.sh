#!/usr/bin/env bash
set -euo pipefail

PORT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d /tmp/scourge-host-tests.XXXXXX)
CC=${CC:-cc}

cleanup() {
  find "$TEST_DIR" -mindepth 1 -delete 2>/dev/null || true
  rmdir "$TEST_DIR" 2>/dev/null || true
}
trap cleanup EXIT

"$CC" -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 \
  -c "$PORT_DIR/tests/test_bionic_compat.c" \
  -o "$TEST_DIR/test_bionic_compat.o"
"$CC" -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 \
  -D_FORTIFY_SOURCE=0 \
  -D__fread_chk=sb_guest_fread_chk \
  -c "$PORT_DIR/src/bionic_shims.c" \
  -o "$TEST_DIR/bionic_shims.o"
"$CC" "$TEST_DIR/test_bionic_compat.o" "$TEST_DIR/bionic_shims.o" \
  -o "$TEST_DIR/test_bionic_compat"
"$TEST_DIR/test_bionic_compat"

"$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -I"$PORT_DIR/src" \
  "$PORT_DIR/tests/test_language_policy.c" \
  "$PORT_DIR/src/language_policy.c" \
  -o "$TEST_DIR/test_language_policy"
"$TEST_DIR/test_language_policy"

"$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -I"$PORT_DIR/src" \
  "$PORT_DIR/tests/test_save_migration.c" \
  "$PORT_DIR/src/save_migration.c" \
  -o "$TEST_DIR/test_save_migration"
"$TEST_DIR/test_save_migration"

"$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -I"$PORT_DIR/src" \
  "$PORT_DIR/tests/test_title_menu_guard.c" \
  "$PORT_DIR/src/title_menu_guard.c" \
  -o "$TEST_DIR/test_title_menu_guard"
"$TEST_DIR/test_title_menu_guard"

python3 -B "$PORT_DIR/tests/test_bionic_import_catalog.py"
python3 -B "$PORT_DIR/tests/test_play_review_contract.py"
python3 -B "$PORT_DIR/tests/test_crash_handler_contract.py"
