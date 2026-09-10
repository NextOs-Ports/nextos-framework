#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-04B: the NXExtract 1.3.0 Recipe class is the single recipe authority.

Proves that nxgenerator 0.3.15 delegates the WHOLE structural validation of
extractor.json to the exact framework engine (same bytes, same module, same
decisions), over a versioned adversarial corpus; that error messages never
leak host paths; and — the negative control — that the old 0.3.12 isolated
validator really accepted a structurally broken recipe the authority
refuses.  V4-04C: the engine bytes are AUTHENTICATED before any of them
execute — pinned independent SHA-256, component-wise symlink refusal,
no-follow open, single bounded read, AST version extraction and execution
of the verified snapshot only, under a stable logical name."""

import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile

TESTS_DIR = pathlib.Path(__file__).resolve().parent
GENERATOR_ROOT = TESTS_DIR.parent
REPOSITORY = GENERATOR_ROOT.parents[1]
CORPUS = TESTS_DIR / "fixtures" / "recipe-authority-corpus"


class GateError(AssertionError):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module



GOOD_FAKE_ENGINE = """\
NXEXTRACT_VERSION = "1.3.0"


class NXError(Exception):
    pass


class Recipe:
    def __init__(self, path):
        raise NXError("fixture recipe authority is not for recipes")
"""


class PatchedAuthorityTree:
    """Repoint the generator's canonical constants at a fixture tree."""

    def __init__(self, generator, root, pin=None, max_bytes=None):
        self.generator = generator
        self.root = pathlib.Path(root)
        self.engine_dir = self.root.joinpath(
            *generator.NXEXTRACT_ENGINE_COMPONENTS[:-1])
        self.engine = self.root.joinpath(
            *generator.NXEXTRACT_ENGINE_COMPONENTS)
        self.pin = pin
        self.max_bytes = max_bytes
        self.saved = {}

    def __enter__(self):
        g = self.generator
        self.engine_dir.mkdir(parents=True, exist_ok=True)
        for name in ("REPOSITORY", "NXEXTRACT_ENGINE", "NXEXTRACT_ROOT",
                     "NXEXTRACT_ENGINE_SHA256", "NXEXTRACT_ENGINE_MAX_BYTES",
                     "_NXEXTRACT_AUTHORITY", "_NXEXTRACT_AUTHORITY_SHA256"):
            self.saved[name] = getattr(g, name)
        g.REPOSITORY = self.root
        g.NXEXTRACT_ENGINE = self.engine
        g.NXEXTRACT_ROOT = self.engine_dir
        if self.pin is not None:
            g.NXEXTRACT_ENGINE_SHA256 = self.pin
        if self.max_bytes is not None:
            g.NXEXTRACT_ENGINE_MAX_BYTES = self.max_bytes
        g._NXEXTRACT_AUTHORITY = None
        g._NXEXTRACT_AUTHORITY_SHA256 = None
        return self

    def __exit__(self, *exc):
        for name, value in self.saved.items():
            setattr(self.generator, name, value)
        return False

    def write_engine(self, source):
        self.engine.write_text(source, encoding="utf-8")
        (self.engine_dir / "VERSION").write_text("1.3.0\n", encoding="utf-8")
        return hashlib.sha256(source.encode("utf-8")).hexdigest()


def preverify_battery(generator):
    import os
    cases = 0

    def refuse(tree, fragment, sentinel=None):
        nonlocal cases
        try:
            generator.nxextract_recipe_authority()
        except generator.ProjectError as error:
            message = str(error)
            require(fragment in message,
                    "unexpected refusal: %r (wanted %r)" % (message, fragment))
            require(str(tree.root) not in message and
                    "Traceback" not in message,
                    "refusal leaked a host path or traceback: " + message)
        else:
            raise GateError("hostile engine was accepted (%s)" % fragment)
        require(generator._NXEXTRACT_AUTHORITY is None,
                "a failure left a partial module in the cache")
        if sentinel is not None:
            require(not sentinel.exists(),
                    "hostile engine bytes EXECUTED before refusal")
        cases += 1

    with tempfile.TemporaryDirectory() as raw:
        root = pathlib.Path(raw)

        # 1. Tampered engine with the SAME version string, against the REAL
        # pinned identity: refused without executing the sentinel.
        sentinel = root / "sentinel-executed"
        hostile = ('NXEXTRACT_VERSION = "1.3.0"\n'
                   'open(%r, "w").close()\n' % str(sentinel))
        with PatchedAuthorityTree(generator, root / "t1") as tree:
            tree.write_engine(hostile)
            refuse(tree, "pinned independent identity", sentinel)

        # 2. Version drift (engine constant) is proven BEFORE execution.
        with PatchedAuthorityTree(generator, root / "t2") as tree:
            drifted = ('NXEXTRACT_VERSION = "9.9.9"\n'
                       'open(%r, "w").close()\n' % str(sentinel))
            tree.pin = tree.write_engine(drifted)
            tree.generator.NXEXTRACT_ENGINE_SHA256 = tree.pin
            refuse(tree, "version drifted", sentinel)

        # 3. Dynamic expression, duplicate declaration, syntax error and a
        # missing declaration are refused by AST before execution.
        ast_cases = (
            ('NXEXTRACT_VERSION = "1.3" + ".0"\n', "literal string"),
            ('NXEXTRACT_VERSION = "1.3.0"\nNXEXTRACT_VERSION = "1.3.0"\n',
             "exactly once"),
            ('def broken(:\n', "does not parse"),
            ('OTHER = 1\n', "exactly once"),
        )
        for index, (source, fragment) in enumerate(ast_cases):
            with PatchedAuthorityTree(generator, root / ("t3-%d" % index)) \
                    as tree:
                pin = tree.write_engine(source)
                tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
                refuse(tree, fragment)

        # 4. Verified bytes that lack the Recipe/NXError interface never
        # enter the cache.
        with PatchedAuthorityTree(generator, root / "t4") as tree:
            pin = tree.write_engine('NXEXTRACT_VERSION = "1.3.0"\n')
            tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
            refuse(tree, "Recipe/NXError")

        # 5. Path shape: engine symlink, symlinked middle component, FIFO,
        # directory and oversize file are refused; a repointed engine path
        # (candidate/env trying to choose the module) is refused as drift.
        with PatchedAuthorityTree(generator, root / "t5") as tree:
            real = tree.root / "real-engine.py"
            real.write_text(GOOD_FAKE_ENGINE, encoding="utf-8")
            tree.engine.symlink_to(real)
            (tree.engine_dir / "VERSION").write_text("1.3.0\n",
                                                     encoding="utf-8")
            refuse(tree, "symlink")
        with PatchedAuthorityTree(generator, root / "t6") as tree:
            tree.write_engine(GOOD_FAKE_ENGINE)
            moved = tree.root / "moved-universal"
            tree.engine_dir.rename(moved)
            tree.engine_dir.symlink_to(moved)
            refuse(tree, "symlink")
        with PatchedAuthorityTree(generator, root / "t7") as tree:
            os.mkfifo(tree.engine)
            (tree.engine_dir / "VERSION").write_text("1.3.0\n",
                                                     encoding="utf-8")
            refuse(tree, "regular file")
        with PatchedAuthorityTree(generator, root / "t8") as tree:
            tree.engine.mkdir()
            refuse(tree, "regular file")
        with PatchedAuthorityTree(generator, root / "t9",
                                  max_bytes=32) as tree:
            pin = tree.write_engine(GOOD_FAKE_ENGINE)
            tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
            refuse(tree, "size ceiling")
        with PatchedAuthorityTree(generator, root / "t10") as tree:
            pin = tree.write_engine(GOOD_FAKE_ENGINE)
            tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
            tree.generator.NXEXTRACT_ENGINE = tree.root / "elsewhere.py"
            refuse(tree, "path drifted")

        # 6. VERSION file authority: symlink and divergence refused.
        with PatchedAuthorityTree(generator, root / "t11") as tree:
            pin = tree.write_engine(GOOD_FAKE_ENGINE)
            tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
            version = tree.engine_dir / "VERSION"
            version.unlink()
            other = tree.root / "other-version"
            other.write_text("1.3.0\n", encoding="utf-8")
            version.symlink_to(other)
            refuse(tree, "regular non-symlink")
        with PatchedAuthorityTree(generator, root / "t12") as tree:
            pin = tree.write_engine(GOOD_FAKE_ENGINE)
            tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
            (tree.engine_dir / "VERSION").write_text("1.2.9\n",
                                                     encoding="utf-8")
            refuse(tree, "version drifted")

        # 7. Swap between verification and use never executes new bytes:
        # only the authenticated snapshot runs.
        with PatchedAuthorityTree(generator, root / "t13") as tree:
            pin = tree.write_engine(GOOD_FAKE_ENGINE)
            tree.generator.NXEXTRACT_ENGINE_SHA256 = pin
            original_read = generator._read_authenticated_engine

            def swapping_read():
                data, digest = original_read()
                tree.engine.write_text(
                    'open(%r, "w").close()\n' % str(sentinel),
                    encoding="utf-8")
                return data, digest

            generator._read_authenticated_engine = swapping_read
            try:
                module = generator.nxextract_recipe_authority()
            finally:
                generator._read_authenticated_engine = original_read
            require(module.NXEXTRACT_VERSION == "1.3.0",
                    "authenticated snapshot did not load")
            require(not sentinel.exists(),
                    "post-verification swap executed new bytes")
            cases += 1
    return cases


def main():
    generator = load_module(GENERATOR_ROOT / "nxgenerator.py",
                            "nxgenerator_authority_gate")
    engine_path = pathlib.Path(generator.NXEXTRACT_ENGINE)

    # The authority is loaded from the framework tree, never from a
    # candidate source_root, and both VERSION declarations agree.
    require(engine_path.is_file(), "canonical engine path is not a file")
    require(REPOSITORY in engine_path.parents,
            "engine path escapes the repository framework tree")
    authority_module = generator.nxextract_recipe_authority()
    require(authority_module.NXEXTRACT_VERSION ==
            generator.NXEXTRACT_RECIPE_AUTHORITY_VERSION == "1.3.0",
            "authority version gate drifted")
    engine_sha = hashlib.sha256(engine_path.read_bytes()).hexdigest()
    require(engine_sha == generator.NXEXTRACT_ENGINE_SHA256,
            "pinned independent identity drifted from the canonical engine")
    require(generator._NXEXTRACT_AUTHORITY_SHA256 == engine_sha,
            "generator executed different bytes than the canonical engine")
    require(authority_module.__file__ ==
            generator.NXEXTRACT_AUTHORITY_LOGICAL_NAME,
            "authority module leaks a real pathname as __file__")

    # V4-04C pre-verification battery over a patched fixture tree: not one
    # byte of a presented engine may execute before its identity is proven.
    preverify_cases = preverify_battery(generator)

    # Corpus parity: for every versioned case the generator's delegated
    # decision equals the direct authority decision and the expectation.
    expectations = json.loads(
        (CORPUS / "expectations.json").read_text(encoding="utf-8"))
    require(len(expectations) >= 20, "corpus shrank")
    checked = 0
    for name, expected in sorted(expectations.items()):
        case = CORPUS / name
        require(case.is_file(), "missing corpus case: " + name)

        try:
            generator.validate_recipe_with_nxextract_authority(
                case, "extractor.json")
            generator_decision = "accept"
            generator_message = ""
        except generator.ProjectError as error:
            generator_decision = "refuse"
            generator_message = str(error)

        try:
            authority_module.Recipe(str(case))
            authority_decision = "accept"
        except (authority_module.NXError, RecursionError):
            authority_decision = "refuse"

        require(generator_decision == authority_decision,
                "consumers diverged on %s: generator=%s authority=%s"
                % (name, generator_decision, authority_decision))
        require(generator_decision == expected,
                "unexpected decision on %s: got %s, expected %s%s"
                % (name, generator_decision, expected,
                   " (" + generator_message + ")" if generator_message
                   else ""))
        if generator_message:
            require(str(case) not in generator_message and
                    str(case.resolve()) not in generator_message,
                    "host path leaked in message for " + name)
        checked += 1

    # Generated hardening cases (deterministic, too big/deep to version):
    with tempfile.TemporaryDirectory() as raw:
        root = pathlib.Path(raw)
        oversize = root / "oversize.json"
        filler = json.dumps({"schema": 1, "pad": "x" * (1024 * 1024)})
        oversize.write_text(filler, encoding="utf-8")
        try:
            generator.validate_recipe_with_nxextract_authority(
                oversize, "extractor.json")
        except generator.ProjectError as error:
            require("ceiling" in str(error), str(error))
        else:
            raise GateError("oversize recipe was accepted")
        deep = root / "deep.json"
        deep.write_text("[" * 4000 + "]" * 4000, encoding="utf-8")
        try:
            generator.validate_recipe_with_nxextract_authority(
                deep, "extractor.json")
        except generator.ProjectError:
            pass
        else:
            raise GateError("deep nesting was accepted")
        missing = root / "does-not-exist.json"
        try:
            generator.validate_recipe_with_nxextract_authority(
                missing, "extractor.json")
        except generator.ProjectError:
            pass
        else:
            raise GateError("missing recipe was accepted")

    # Generator-specific policies stay ADDITIVE: the authority accepts the
    # canonical minimal recipe and both extra policies also pass over it, so
    # nothing the generator keeps contradicts the NXExtract grammar.
    minimal = json.loads((CORPUS / "valid-minimal.json").read_text())
    generator.validate_apk_variant_policy(minimal)
    require(minimal["input"]["search_dirs"][0] ==
            generator.OWNER_DATA_DIRECTORY,
            "canonical minimal recipe violates the owner-data-first policy")

    # NEGATIVE CONTROL: the old 0.3.12 isolated validator accepted a
    # commit-less recipe end-to-end through validate_project; the 0.3.13
    # delegation refuses it.  The old tree is materialized from the mission
    # base commit by the runner (NXGEN_OLD_TREE); without it the control is
    # SKIPPED WITH FAILURE so it can never silently pass.
    import os
    old_tree = os.environ.get("NXGEN_OLD_TREE")
    require(old_tree, "NXGEN_OLD_TREE (base-commit tree) is required for "
                      "the negative control")
    old = load_module(pathlib.Path(old_tree) /
                      "framework/nxgenerator/nxgenerator.py",
                      "nxgenerator_old_control")
    broken = json.loads((CORPUS / "invalid-commit-missing.json").read_text())
    document = json.loads(
        (GENERATOR_ROOT / "examples" /
         "nxproject-aarch64.example.json").read_text(encoding="utf-8"))
    document["nxextract_recipe"] = "extractor.json"
    with tempfile.TemporaryDirectory() as raw:
        root = pathlib.Path(raw)
        (root / "extractor.json").write_text(json.dumps(broken),
                                             encoding="utf-8")
        license_name = document["license"]["source"]
        (root / license_name).write_bytes(
            (REPOSITORY / "LICENSE").read_bytes())
        old.validate_project(json.loads(json.dumps(document)),
                             source_root=root)  # undue acceptance
        try:
            generator.validate_project(json.loads(json.dumps(document)),
                                       source_root=root)
        except generator.ProjectError as error:
            require("commit" in str(error), str(error))
        else:
            raise GateError("0.3.13 also accepted the commit-less recipe")

    print("nxgenerator 0.3.15 recipe authority: PASS corpus=%d "
          "engine_sha256=%s parity=1 preverify_cases=%d sanitized=1 "
          "negative_control=1" % (checked, engine_sha, preverify_cases))


if __name__ == "__main__":
    try:
        main()
    except GateError as error:
        print("nxgenerator recipe authority: FAIL %s" % error,
              file=sys.stderr)
        sys.exit(1)
