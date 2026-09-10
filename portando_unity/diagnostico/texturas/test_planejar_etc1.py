import copy
import json
import unittest
from planejar_etc1 import dimensions, payload, plan, unique_keys


class TexturePlanTests(unittest.TestCase):
    def fixture(self, **changes):
        item = dict(id="atlas", width=1024, height=1024, levels=1,
                    source_format="RGBA8888", role="color", alpha="opaque", dynamic=False)
        item.update(changes)
        return dict(schema="unity-texture-plan/1", kind="synthetic", textures=[item])

    def row(self, **changes):
        return plan(self.fixture(**changes))["textures"][0]

    def test_known_1024_base_costs(self):
        costs = self.row()["payload_comparison_bytes"]
        self.assertEqual(costs["RGBA8888"], 4194304)
        self.assertEqual(costs["ETC1"], 524288)
        self.assertEqual(costs["ETC1_DUAL"], 1048576)

    def test_full_mips_include_minimum_blocks(self):
        dims = dimensions(4, 4, "full")
        self.assertEqual(dims, [(4, 4), (2, 2), (1, 1)])
        self.assertEqual(payload("ETC1", dims), 24)
        self.assertEqual(payload("RGBA8888", dims), 84)

    def test_npot_cost_does_not_round_geometry(self):
        dims = dimensions(5, 3, "full")
        self.assertEqual(dims, [(5, 3), (2, 1), (1, 1)])
        self.assertEqual(payload("ETC1", dims), 32)

    def test_thin_texture_block_overhead(self):
        self.assertEqual(payload("ETC1", dimensions(8, 1, 1)), 16)
        self.assertEqual(self.row(width=1, height=1)["decision"], "NO_PAYLOAD_SAVING")

    def test_alpha_selects_dual_without_approving_runtime(self):
        for alpha in ("blend", "cutout", "premultiplied"):
            row = self.row(alpha=alpha)
            self.assertEqual(row["candidate_format"], "ETC1_DUAL")
            self.assertEqual(row["fragment_samplers_for_one_sampled_image"], 2)
            self.assertFalse(row["runtime_validated"])

    def test_special_uses_do_not_get_color_conversion(self):
        for role in ("render_target", "depth", "normal", "data", "font_sdf", "video", "unknown"):
            self.assertIsNone(self.row(role=role)["candidate_format"])

    def test_dynamic_and_unknown_alpha_stay_unplanned(self):
        self.assertIsNone(self.row(dynamic=True)["candidate_format"])
        self.assertIsNone(self.row(alpha="unknown")["candidate_format"])

    def test_hdr_and_single_channel_require_analysis_even_when_named_color(self):
        self.assertIsNone(self.row(source_format="RGBA16F")["candidate_format"])
        self.assertIsNone(self.row(source_format="A8")["candidate_format"])

    def test_rgb_format_cannot_supply_declared_transparency(self):
        for fmt, alpha in (("RGB888", "blend"), ("RGB565", "premultiplied")):
            row = self.row(source_format=fmt, alpha=alpha)
            self.assertIsNone(row["candidate_format"])
            self.assertEqual(row["potential_saving_bytes"], 0)

    def test_already_compressed_preserved(self):
        for fmt in ("ETC1", "ETC1_DUAL"):
            row = self.row(source_format=fmt)
            self.assertEqual(row["potential_saving_bytes"], 0)

    def test_totals_do_not_claim_rss_fps_or_peak(self):
        data = self.fixture()
        other = copy.deepcopy(data["textures"][0])
        other.update(id="alpha", alpha="blend")
        data["textures"].append(other)
        report = plan(data)
        self.assertEqual(report["totals"]["planned_payload_bytes"], 1572864)
        for key in ("rss_bytes", "conversion_peak_bytes", "fps_change"):
            self.assertEqual(report[key], "NOT_MEASURED")

    def test_invalid_dimensions_levels_and_types(self):
        for change in (dict(width=0), dict(height=-1), dict(width=True), dict(width=65537),
                       dict(levels=12), dict(levels=0), dict(levels=False), dict(dynamic=1),
                       dict(source_format="PNG"), dict(id="../atlas")):
            with self.assertRaises(ValueError):
                self.row(**change)

    def test_duplicate_ids_rejected(self):
        data = self.fixture()
        data["textures"].append(copy.deepcopy(data["textures"][0]))
        with self.assertRaises(ValueError):
            plan(data)

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaises(ValueError):
            json.loads('{"width": 1, "width": 1024}', object_pairs_hook=unique_keys)

    def test_unknown_fields_and_empty_inventory_rejected(self):
        for data in (dict(self.fixture(), extra=1), dict(self.fixture(), textures=[])):
            with self.assertRaises(ValueError):
                plan(data)


if __name__ == "__main__":
    unittest.main()
