#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Canonical host gate: every command gets its own durable run log.
set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
RUN_LOGGED=$REPO_ROOT/framework/tools/run-logged.sh

if [[ ${1:-} != --log-root || -z ${2:-} || $# -ne 2 ]]; then
  printf 'usage: %s --log-root ABSOLUTE_DIRECTORY\n' "${0##*/}" >&2
  exit 2
fi
LOG_ROOT=$2
case $LOG_ROOT in /*) ;; *)
  printf 'run-safe-gates: log root must be absolute\n' >&2
  exit 2
  ;;
esac

cd -- "$REPO_ROOT"
passed=0
run_gate() {
  local gate_id=$1
  shift
  printf '[safe-gates] START %s\n' "$gate_id"
  "$RUN_LOGGED" --log-root "$LOG_ROOT" -- "$@"
  passed=$((passed + 1))
  printf '[safe-gates] PASS %s\n' "$gate_id"
}

# A gate that needs an external, owner-supplied artifact CANNOT be turned into
# a PASS when that artifact is absent. It exits 77 and is counted and printed
# as a SKIP, so a limit of claim stays visible in the battery output instead of
# disappearing into a green count.
skipped=0
run_gate_external() {
  local gate_id=$1 status=0
  shift
  printf '[safe-gates] START %s\n' "$gate_id"
  "$RUN_LOGGED" --log-root "$LOG_ROOT" -- "$@" || status=$?
  if [ "$status" -eq 0 ]; then
    passed=$((passed + 1))
    printf '[safe-gates] PASS %s\n' "$gate_id"
  elif [ "$status" -eq 77 ]; then
    skipped=$((skipped + 1))
    printf '[safe-gates] SKIP %s (external artifact absent; LIMIT OF CLAIM)\n' \
      "$gate_id"
  else
    printf '[safe-gates] FAIL %s (status %s)\n' "$gate_id" "$status"
    exit "$status"
  fi
}

# Infrastructure must pass before any filesystem/process-heavy suite.
run_gate test-infrastructure \
  python3 -B framework/tests/test_infrastructure.py
run_gate toolchain-preflight \
  python3 -B framework/nxabi/nxabi.py toolchain
run_gate bootstrap-static-safety \
  bash framework/nxbootstrap/tests/test-safety-static.sh
run_gate nxsplash-host \
  bash framework/nxsplash/tests/run.sh
run_gate nxgl-gles1 \
  bash framework/nxgl/tests/test_gles1.sh
run_gate nxgl-provider-discovery \
  bash framework/nxgl/tests/test_provider_discovery.sh
run_gate tooling-filesystem \
  bash framework/tests/test-tools.sh
run_gate tools-args \
  bash tools/tests/test-nx-verify-args.sh
run_gate nx-add-capability \
  bash framework/tools/tests/test_add_capability.sh
run_gate firmware-zip-audit \
  python3 -B framework/tests/test_firmware_zip_audit.py
run_gate gamedir-logical-physical \
  bash framework/nxbootstrap/tests/test-gamedir-logical-physical.sh
run_gate component-versions \
  python3 -B framework/tests/test_component_versions.py

run_gate catalog \
  python3 -B framework/catalog/tests/test_catalog.py
run_gate nxport-contract \
  python3 -B framework/nxbootstrap/tests/test-manifest-contract.py
run_gate nxbootstrap-options \
  python3 -B framework/nxbootstrap/tests/test_options.py
run_gate portmaster-contract \
  python3 -B framework/portmaster/tests/test_portmaster_contract.py
run_gate portmaster-real-cycle \
  python3 -B framework/portmaster/tests/test_harbourmaster_cycle.py
run_gate firmware-profiles \
  python3 -B framework/tests/test_firmware_profiles.py
run_gate firmware-matrix-v2-static \
  python3 -B framework/tests/test_firmware_matrix_v2.py
run_gate gles1-profile-matrix \
  python3 -B framework/tests/test_gles1_profile_matrix.py
run_gate device-environments-negative \
  python3 -B framework/tests/test_device_environments_negative.py
run_gate nxcompat-host \
  bash framework/nxcompat/tests/run-host.sh
run_gate nxcompat-system-font \
  bash framework/nxcompat/tests/test_system_font.sh
run_gate nxcompat-m12-audit \
  python3 -B framework/nxcompat/tests/test_m12_audit.py
run_gate nxobs-m12a-host \
  python3 -B framework/nxobs/tests/test_m12a_observability.py
run_gate nxobs-support-flow \
  bash tools/tests/test-nx-logs-privacy.sh
run_gate nxobs-events-jsonl \
  python3 -B framework/nxobs/tests/test_events_jsonl.py
run_gate nxobs-budget-check \
  python3 -B framework/nxobs/tests/test_budget_check.py
run_gate nxobs-mem-sampler \
  bash framework/nxobs/tests/run-mem-sampler.sh
run_gate nxobs-perf-sampler \
  bash framework/nxobs/tests/run-perf-sampler.sh
run_gate nxobs-device-launch-proof-dir \
  python3 -B framework/nxobs/tests/test_device_launch_proof_dir.py
run_gate nxobs-v4-graphics-evidence \
  python3 -B framework/nxobs/tests/test_v4_graphics_evidence.py
run_gate nxgl-m13-host \
  bash framework/nxgl/tests/run-m13-host.sh
run_gate nxgl-m13-audit \
  python3 -B framework/nxgl/tests/test_m13_audit.py
run_gate nxgl-video-receipt \
  bash framework/nxgl/tests/test-video-receipt.sh
run_gate nxaudio-m14-host \
  bash framework/nxaudio/tests/run-host.sh
run_gate nxaudio-m14-audit \
  python3 -B framework/nxaudio/tests/test_m14_audio_contract.py
run_gate nxextract-python \
  python3 -B -m unittest discover \
    -s suportando_outros_devices/extrator-universal/tests -p 'test_*.py'
run_gate nxextract-phase-validation \
  python3 -B suportando_outros_devices/extrator-universal/tests/test_phase_validation.py
run_gate nxextract-hook-transaction \
  python3 -B suportando_outros_devices/extrator-universal/tests/test_hook_transaction.py
run_gate nxextract-runtime-env \
  bash suportando_outros_devices/extrator-universal/tests/test_runtime_env.sh
run_gate nxextract-audit \
  python3 -B framework/nxextract/tests/test_p06_terminal_result.py
run_gate nxextract-m07-audit \
  python3 -B framework/nxextract/tests/test_m07_audit.py
run_gate_external nxextract-legal-fixture \
  python3 -B framework/nxextract/tests/test_legal_fixture_e2e.py
run_gate nxextract-release \
  bash suportando_outros_devices/extrator-universal/tools/check-release.sh \
    --require-ui
run_gate nxabi-units \
  python3 -B framework/nxabi/tests/test_nxabi.py
run_gate nxabi-sdl-authority \
  python3 -B framework/nxabi/tests/test_sdl_authority.py
run_gate nxabi-m17-gate \
  bash framework/nxabi/tools/nx-abi-gate.sh
run_gate nxabi-m17-closure \
  python3 -B framework/nxabi/tests/test_m17_closure.py
run_gate nxloader-audit \
  python3 -B framework/nxloader/tests/test_m08_audit.py
run_gate nxloader-m09-audit \
  python3 -B framework/nxloader/tests/test_m09_audit.py
run_gate nxloader-m10-audit \
  python3 -B framework/nxloader/tests/test_m10_audit.py
run_gate nxloader-m11-audit \
  python3 -B framework/nxloader/tests/test_m11_audit.py
run_gate nxloader-072-closure \
  python3 -B framework/nxloader/tests/test_072_closure.py
run_gate nxloader-080-closure \
  python3 -B framework/nxloader/tests/test_080_closure.py
run_gate nxandroid-profile-audit \
  python3 -B framework/nxandroid/tests/test_m11_adapter_profiles.py
run_gate nxandroid-m11-audit \
  python3 -B framework/nxandroid/tests/test_m11_audit.py
run_gate nxandroid-m16-contract \
  python3 -B framework/nxandroid/tests/test_m16_adapter_contract.py
run_gate nxandroid-020-adapter-contract \
  python3 -B framework/nxandroid/tests/test_020_adapter_contract.py
run_gate nxloader-host \
  bash framework/nxloader/tests/run-host.sh
run_gate nxloader-armv7-cross \
  bash framework/nxloader/tests/run-armv7-cross.sh
run_gate nxloader-aarch64-cross \
  bash framework/nxloader/tests/run-aarch64-cross.sh
run_gate nxloader-m11-armv7-lifecycle-cross \
  bash framework/nxloader/tests/run-m11-armv7-lifecycle-cross.sh
run_gate nxloader-m11-aarch64-lifecycle-cross \
  bash framework/nxloader/tests/run-m11-aarch64-lifecycle-cross.sh
run_gate nxandroid-host \
  bash framework/nxandroid/tests/run-host.sh
run_gate_external nxinput-sdl3-portmaster \
  bash framework/nxinput/tests/run-sdl3-portmaster-host.sh
run_gate nxinput-m15-contract \
  python3 -B framework/nxinput/tests/test_m15_input_contract.py
run_gate nxinput-touch-sequence \
  bash framework/nxinput/tests/test_touch_sequence.sh
run_gate input-action-contract \
  python3 -B framework/tests/test_input_action_contract.py
run_gate nxinput-pad-ordinal \
  python3 -B framework/nxinput/tests/test_pad_ordinal_fix.py
run_gate device-controls \
  bash framework/tests/test-device-controls.sh
run_gate terminal-compat \
  bash framework/nxbootstrap/tests/test-terminal-compat.sh
run_gate muos-device-faithful \
  bash framework/nxbootstrap/tests/test-muos-device-faithful.sh
run_gate bootstrap-isolated \
  bash framework/nxbootstrap/tests/run-isolated.sh
run_gate nxrelease-m18-closure \
  python3 -B framework/nxrelease/tests/test_m18_closure.py
run_gate nxrelease-owner-runtime \
  python3 -B framework/nxrelease/tests/test_owner_runtime_release.py
run_gate nxrelease-gptk4 \
  python3 -B framework/nxrelease/tests/test_gptk4_release.py
run_gate nxrelease-macro-dynsym-gate \
  python3 -B framework/nxrelease/tests/test_macro_dynsym_gate.py
run_gate nxrelease-nx-reseal \
  python3 -B framework/nxrelease/tests/test_nx_reseal.py
run_gate nxrelease-pad-ordinal-gate \
  python3 -B framework/nxrelease/tests/test_pad_ordinal_gate.py
run_gate nxbootstrap-audio-shield \
  bash framework/nxbootstrap/tests/test-audio-shield.sh
run_gate nxrelease \
  bash framework/nxrelease/tests/test_nxrelease.sh
run_gate nxrelease-options \
  python3 -B framework/nxrelease/tests/test_options_integration.py
run_gate nxrelease-gptk-runtime-proof \
  python3 -B framework/nxrelease/tests/test_gptk_runtime_proof.py
run_gate nxrelease-on-device-input-proof \
  python3 -B framework/nxrelease/tests/test_on_device_input_proof_class.py
run_gate nxrelease-generation-store-v2 \
  python3 -B framework/nxrelease/tests/test_generation_store_v2.py
run_gate nxrelease-nxextract-private-library \
  python3 -B framework/nxrelease/tests/test_nxextract_private_library.py
run_gate nxrelease-generator-root \
  python3 -B framework/nxrelease/tests/test_generator_root.py
run_gate nxrelease-refresh-pins-schema3 \
  python3 -B framework/nxrelease/tests/test_refresh_pins_schema3.py
run_gate nxextract-engines \
  python3 -B framework/nxrelease/tests/test_nxextract_engines.py
run_gate nxrelease-v4-repro \
  python3 -B framework/nxrelease/tests/test_v4_repro.py
run_gate nxrelease-v4-optins \
  python3 -B framework/nxrelease/tests/test_v4_optins.py
run_gate nxgenerator-host \
  python3 -B framework/nxgenerator/tests/test_nxgenerator.py
run_gate nxgenerator-generation-runtime \
  python3 -B framework/nxgenerator/tests/test_generation_runtime.py
run_gate nxgenerator-package-payload \
  python3 -B framework/nxgenerator/tests/test_package_payload.py
run_gate nxgenerator-gptk-live-contract \
  python3 -B framework/nxgenerator/tests/test_gptk_live_contract.py
run_gate nxgenerator-framework-pin \
  python3 -B framework/nxgenerator/tests/test_framework_pin.py
run_gate nxgenerator-m19-closure \
  python3 -B framework/nxgenerator/tests/test_m19_closure.py
run_gate nxgenerator-v4-declarative \
  python3 -B framework/nxgenerator/tests/test_v4_declarative.py
run_gate nxgenerator-v5-owner-video \
  python3 -B framework/nxgenerator/tests/test_v5_owner_video.py
run_gate nxgenerator-v5-gptk4-trigger-override \
  python3 -B framework/nxgenerator/tests/test_v5_gptk4_trigger_override.py
# The recipe-authority negative control compares against the pre-delegation
# generator; that tree lives in this repository's own history, so the runner
# materializes it deterministically instead of asking the owner for it.
nxgen_old_tree=$(mktemp -d)
git -C "$REPO_ROOT" archive 59703d7 framework | tar -x -C "$nxgen_old_tree"
export NXGEN_OLD_TREE=$nxgen_old_tree
run_gate nxgenerator-recipe-authority \
  python3 -B framework/nxgenerator/tests/test_recipe_authority.py
unset NXGEN_OLD_TREE
rm -rf "$nxgen_old_tree"
run_gate nxrelease-nxscan-wired \
  python3 -B framework/nxrelease/tests/test_nxscan.py
run_gate m20-closure \
  python3 -B framework/tests/test_m20_closure.py
run_gate chrono-m21-audit \
  python3 -B ports/chrono/tests/test_m21_pilot.py
run_gate chrono-m22-audit \
  python3 -B ports/chrono/tests/test_m22_physical.py
run_gate nxinput-gptk-host \
  bash framework/nxinput/tests/run-gptk-host.sh
run_gate nxinput-muos-layout-host \
  bash framework/nxinput/tests/run-muos-layout-host.sh
run_gate nxandroid-input-sinks \
  bash framework/nxandroid/tests/run-input-sinks-host.sh
run_gate nxgl-v4-host \
  bash framework/nxgl/tests/run-v4-host.sh
run_gate nxgl-v3-graphics \
  bash framework/nxgl/tests/run-v3-graphics-host.sh
run_gate nxabi-v3-roles \
  python3 -B framework/nxabi/tests/test_v3_roles.py
run_gate nxaudio-receipt \
  bash framework/nxaudio/tests/run-receipt-host.sh
run_gate nxdoctor \
  python3 -B framework/nxdoctor/tests/test_nxdoctor.py
run_gate cfw-evidence \
  python3 -B framework/tests/test_cfw_evidence.py
run_gate firmware-matrix-v3 \
  python3 -B framework/tests/test_firmware_matrix_v3.py
run_gate repro-build \
  bash framework/tests/test_repro_build.sh
run_gate nxcompat-settings-language \
  bash framework/nxcompat/tests/run-settings-host.sh
run_gate nxcompat-installed-consumer \
  bash framework/nxcompat/tests/run-installed-consumer.sh
run_gate nxledger-host \
  bash framework/nxledger/tests/run-nxledger-host.sh
run_gate nxinput-installed-consumer \
  bash framework/nxinput/tests/run-installed-consumer.sh
run_gate nxandroid-installed-consumer \
  bash framework/nxandroid/tests/run-installed-consumer.sh
run_gate nxbootstrap-owner-materialize-atomic \
  bash framework/nxbootstrap/tests/test-owner-materialize-atomic.sh
run_gate nxbootstrap-owner-runtime \
  bash framework/nxbootstrap/tests/test-owner-runtime.sh
run_gate nxbootstrap-owner-runtime-generator \
  python3 -B framework/nxbootstrap/tests/test_owner_runtime.py
run_gate input-language-host-contract \
  bash framework/tests/test-input-language-host-contract.sh
run_gate nxaudio-runtime-host \
  bash framework/nxaudio/tests/run-runtime-host.sh
run_gate nxdoctor-evidence \
  python3 -B framework/nxdoctor/tests/test_evidence.py
run_gate nxloader-090-closure \
  python3 -B framework/nxloader/tests/test_090_closure.py
run_gate nxloader-m124-attestation \
  python3 -B framework/nxloader/tests/test_m124_attestation.py
run_gate nxgenerator-c3-bytes-preserved \
  python3 -B framework/nxgenerator/tests/test_c3_bytes_preserved.py
run_gate nxgenerator-c4-gptk-bytes \
  python3 -B framework/nxgenerator/tests/test_c4_gptk_bytes.py
run_gate nxgenerator-face-layout-variants \
  python3 -B framework/nxgenerator/tests/test_face_layout_variants.py
run_gate nxgenerator-input-proof-roteiro \
  python3 -B framework/nxgenerator/tests/test_input_proof_roteiro.py
run_gate nxgenerator-nextos-catalog \
  python3 -B framework/nxgenerator/tests/test_nextos_catalog.py
run_gate nxobs-v4-input-observe \
  python3 -B framework/nxobs/tests/test_v4_input_observe.py
run_gate nxobs-v4-c6-receipt \
  python3 -B framework/nxobs/tests/test_v4_c6_receipt.py
run_gate nxrelease-c4-gptk-closure \
  python3 -B framework/nxrelease/tests/test_c4_gptk_closure.py
run_gate nxandroid-c7-consumer-ledger \
  python3 -B framework/nxandroid/tests/test_c7_consumer_ledger.py
run_gate nxandroid-c8-static-contract \
  python3 -B framework/nxandroid/tests/test_c8_static_contract.py
run_gate nxandroid-c8-unity-ledger \
  python3 -B framework/nxandroid/tests/test_c8_unity_ledger.py
run_gate nxinput-observe-host \
  bash framework/nxinput/tests/run-observe-host.sh
run_gate nxinput-padset-host \
  bash framework/nxinput/tests/run-padset-host.sh
run_gate nxinput-v5-host \
  bash framework/nxinput/tests/run-v5-host.sh
run_gate nxinput-device-input-proof \
  python3 -B framework/nxinput/tests/test_device_input_proof.py
run_gate_external nxinput-sovereign-corpus \
  bash framework/nxinput/tests/run-sovereign-corpus-host.sh
run_gate_external nxinput-gptk-v2-host \
  bash framework/nxinput/tests/run-gptk-v2-host.sh
run_gate nxinput-godot-runtime-policy \
  bash framework/nxinput/tests/run-godot-runtime-policy.sh
run_gate nxgl-godot-frame-proof \
  bash framework/nxgl/tests/run-godot-frame-proof.sh
run_gate nxextract-corpus-replay \
  python3 -B suportando_outros_devices/extrator-universal/tests/test_corpus_replay.py
run_gate nxbootstrap-owner-e6a-real \
  bash framework/nxbootstrap/tests/test-owner-e6a-real.sh
run_gate nxrelease-vendor-pin-gate \
  python3 -B framework/nxrelease/tests/test_vendor_pin_gate.py
run_gate nxrelease-port-init-predicate \
  python3 -B framework/nxrelease/tests/test_port_init_predicate.py
run_gate nxinput-gptk-live-boundary \
  bash framework/nxinput/tests/run-gptk-live-boundary.sh
run_gate v5-matrix-manifest \
  python3 -B framework/tests/test_v5_matrix_manifest.py
run_gate nxgenerator-controls-closure \
  python3 -B framework/nxgenerator/tests/test_controls_closure.py
run_gate nxrelease-controls-closure-gate \
  python3 -B framework/nxrelease/tests/test_controls_closure_gate.py
run_gate nxrelease-prompt-capture-gate \
  python3 -B framework/nxrelease/tests/test_prompt_capture_gate.py
run_gate diff-check \
  git diff --check -- framework suportando_outros_devices publicando_ports
run_gate m20-green-checkpoint \
  bash framework/tests/capture-green-checkpoint.sh \
  --checkpoint-root "$LOG_ROOT/checkpoints"

printf '[safe-gates] ALL PASS count=%s skipped_external=%s ' "$passed" "$skipped"
printf 'hardware_ran=0 device_access=0 '
printf 'external_guest_initializers_executed=0 '
printf 'external_guest_jni_onload_executed=0 synthetic_lifecycle_runs=4\n'
