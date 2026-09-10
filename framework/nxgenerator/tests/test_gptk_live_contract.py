#!/usr/bin/env python3
"""Directed contract gate for the fail-safe GPTK runtime opt-in."""

import importlib.util
import json
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "nxgenerator.py"
RUNTIME_GATE = ROOT / "tests" / "test_generation_runtime.py"
RELEASE_RENDERER = ROOT.parent / "nxrelease" / "nx-render-manifest.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("nxgenerator_gptk", TOOL)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load nxgenerator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runtime_gate():
    spec = importlib.util.spec_from_file_location(
        "nxgenerator_runtime_fixture", RUNTIME_GATE)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load runtime fixture")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_release_renderer():
    spec = importlib.util.spec_from_file_location(
        "nxrelease_static_gptk_renderer", RELEASE_RENDERER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load nxrelease renderer")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def controls(runtime=True):
    value = {
        "schema": 2,
        "actions": [
            {"id": "menu.accept", "kind": "button",
             "sinks": ["engine.accept"]},
            {"id": "player.move", "kind": "vector",
             "sinks": ["engine.move"]},
        ],
        "contexts": {
            "menu": {"A": "menu.accept", "UP": "native", "DOWN": "native",
                     "LEFT": "native", "RIGHT": "native"},
            "gameplay": {"LEFT_STICK": "player.move", "UP": "native",
                         "DOWN": "native", "LEFT": "native",
                         "RIGHT": "native"},
        },
    }
    if runtime:
        value["runtime_mapping"] = "nxinput-gptk"
    return value


def main():
    tool = load_tool()
    release_renderer = load_release_renderer()
    live = tool.validate_controls(controls(), 3)
    expected = {
        "schema": "nxinput-gptk-live/1",
        "context_initial": "unproven",
        "unproven_policy": "native-passthrough",
        "sink_coverage": "all-actions-before-activation",
        "delivery_ack": "required",
    }
    require(live["runtime_contract"] == expected,
            "live opt-in lost its generated fail-safe policy")

    legacy = tool.validate_controls(controls(runtime=False), 3)
    require("runtime_contract" not in legacy,
            "port without opt-in gained live-runtime bytes")

    # What promotion must copy verbatim into adapter/input.  A port may name
    # real sinks, but may not relax any activation policy itself.
    promoted_input = {
        "actions": live["actions"],
        "contexts": live["contexts"],
        "runtime_mapping": live["runtime_mapping"],
        "runtime_contract": live["runtime_contract"],
    }
    require(json.loads(json.dumps(promoted_input))["runtime_contract"] ==
            expected, "runtime contract is not JSON-stable")
    tool.validate_promoted_gptk_live_contract(promoted_input, live)
    relaxed = dict(expected)
    relaxed["context_initial"] = "gameplay"
    promoted_input["runtime_contract"] = relaxed
    try:
        tool.validate_promoted_gptk_live_contract(promoted_input, live)
    except tool.ProjectError:
        pass
    else:
        raise AssertionError("relaxed implicit gameplay policy passed")

    # The live runtime must never be generated without authority 3 inside the
    # package. This is a generator boundary, not only a release audit.
    for profiles in (None, {"enabled": False}):
        try:
            tool.validate_runtime_mapping_authority_bundle(live, profiles)
        except tool.ProjectError:
            pass
        else:
            raise AssertionError(
                "live nxinput passed without an enabled profile bundle")
    tool.validate_runtime_mapping_authority_bundle(live, {
        "enabled": True,
        "bundle": "controllers.nxb",
        "sha256": "ab" * 32,
    })
    try:
        tool.validate_controller_profiles({
            "enabled": True,
            "bundle": "renamed-profiles.nxb",
            "sha256": "ab" * 32,
        }, 3)
    except tool.ProjectError as error:
        require("exactly controllers.nxb" in str(error),
                "renamed bundle failed for the wrong reason")
    else:
        raise AssertionError(
            "a bundle name the runtime cannot declare passed generation")
    tool.validate_runtime_mapping_authority_bundle(legacy, None)

    # Prove the helper is wired into the real project boundary: an invalid
    # schema-v3 project is rejected, while the same project with a complete
    # pinned bundle reaches the normalized configuration.
    runtime_gate = load_runtime_gate()
    with tempfile.TemporaryDirectory(prefix="nxgenerator-gptk-live-") as raw:
        source_root = Path(raw)
        runtime_root = source_root / "runtime"
        runtime_root.mkdir()
        payloads = runtime_gate.write_runtime(runtime_root)
        project = runtime_gate.project_document(payloads)
        (source_root / "LICENSE").write_text("fixture\n", encoding="utf-8")

        # Without the explicit runtime opt-in the generated file is a static
        # description.  It must not promise that owner edits reach the engine.
        static_output = source_root / "static-output"
        tool.generate_project(project, static_output, source_root)
        static_port = static_output / "nxgenerator-runtime-fixture"
        static_gptk = (static_port / "defaults" /
                       "NEXTOSCONTROLLERS.gptk").read_text(encoding="utf-8")
        require("This file is YOURS" not in static_gptk and
                "Este arquivo é SEU" not in static_gptk,
                "static controls still claim to be editable")
        require("does NOT load edits" in static_gptk and
                "NÃO carrega edições" in static_gptk,
                "static controls do not state that edits are inert")
        release_renderer._validate_controls_runtime(project, static_port)

        project["controls"]["runtime_mapping"] = "nxinput-gptk"
        try:
            tool.validate_project(project, source_root)
        except tool.ProjectError:
            pass
        else:
            raise AssertionError("real project boundary accepted no bundle")
        project["controls"]["controller_profiles"] = {
            "enabled": True,
            "bundle": "controllers.nxb",
            "sha256": "ab" * 32,
        }
        normalized = tool.validate_project(project, source_root)
        require(normalized["controller_profiles"]["enabled"] is True,
                "real project boundary lost the enabled authority bundle")

        # Promotion replaces the generated skeleton, so the promoted bytes
        # themselves must carry the exact same authority-3 pin.  Prove both
        # the rejection and the final rendered adapter, not only normalization.
        promoted = {
            "schema": "nxadapter-skeleton-v1", "schema_version": 1,
            "status": "implemented_release", "release_ready": True,
            "lifecycle": {"sequence": ["runtime-main"],
                          "source_evidence": ["tests/gptk-live"]},
            "input": {
                "actions": normalized["controls"]["actions"],
                "contexts": normalized["controls"]["contexts"],
                "runtime_mapping": "nxinput-gptk",
                "runtime_contract": normalized["controls"][
                    "runtime_contract"],
            },
            "input_controller_profiles": normalized["controller_profiles"],
            "language_access": normalized["language_access"],
            "graphics": normalized["graphics"],
        }
        promoted_path = source_root / "promoted-adapter.json"
        project["promotion"] = {
            "adapter_contract": "promoted-adapter.json",
            "claims": {"release_ready": True,
                       "physical_support_proven": False,
                       "adapter_lifecycle_implemented": True},
        }

        mismatched = json.loads(json.dumps(promoted))
        mismatched["input_controller_profiles"]["sha256"] = "cd" * 32
        promoted_path.write_text(json.dumps(mismatched), encoding="utf-8")
        try:
            tool.validate_project(project, source_root)
        except tool.ProjectError as error:
            require("controller profiles differ" in str(error),
                    "mismatched promotion failed for the wrong reason")
        else:
            raise AssertionError(
                "promotion replaced the declared authority-3 pin")

        promoted_path.write_text(json.dumps(promoted), encoding="utf-8")
        promoted_output = source_root / "promoted-output"
        tool.generate_project(project, promoted_output, source_root)
        rendered = json.loads((
            promoted_output / "nxgenerator-runtime-fixture" / "adapter" /
            "adapter-contract.json"
        ).read_text(encoding="utf-8"))
        require(rendered.get("input_controller_profiles") ==
                normalized["controller_profiles"],
                "promoted render lost or rewrote the authority-3 pin")
        live_gptk = (promoted_output / "nxgenerator-runtime-fixture" /
                     "defaults" / "NEXTOSCONTROLLERS.gptk").read_text(
                         encoding="utf-8")
        established_live_header = (
            "# Edite as ações à direita para trocar os controles. A lista de "
            "ações\n"
            "# válidas vem do adapter-contract.json do port. Este arquivo é "
            "SEU:\n"
            "# atualizações do port nunca sobrescrevem a sua cópia editada.\n"
            "# Edit the actions on the right to remap. Valid actions come "
            "from the\n"
            "# port's adapter-contract.json. This file is YOURS: port updates "
            "never\n"
            "# overwrite your edited copy."
        )
        require(established_live_header in live_gptk,
                "live runtime did not preserve the editable header bytewise")
        require("does NOT load edits" not in live_gptk and
                "NÃO carrega edições" not in live_gptk,
                "live runtime received the static-controls warning")
        tool.validate_promoted_controller_profiles({}, None)
        for stale, expected_profiles in (
            ({"input_controller_profiles":
              normalized["controller_profiles"]}, None),
            ({}, normalized["controller_profiles"]),
        ):
            try:
                tool.validate_promoted_controller_profiles(
                    stale, expected_profiles)
            except tool.ProjectError:
                pass
            else:
                raise AssertionError(
                    "promotion did not enforce exact profile presence")

    require((ROOT / "VERSION").read_text(encoding="utf-8").strip() ==
            "0.4.5", "nxgenerator version did not advance")
    print("nxgenerator 0.4.5 GPTK static/live contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
