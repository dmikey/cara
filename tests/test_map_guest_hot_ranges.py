import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "mapper", Path(__file__).parents[1] / "tools/map_guest_hot_ranges.py")
mapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mapper)


class MappingTests(unittest.TestCase):
    def fixture(self):
        return {"kind": "resident_block_census_not_time_samples", "frame": 20,
                "cores": [{"core": 0, "resident_entries": 100,
                           "blocks": [
                               {"pc": 0x1000, "end_pc": 0x1004, "executions": 7,
                                "thumb": False, "source_bytes": [1, 2, 3, 4]},
                               {"pc": 0x1004, "end_pc": 0x1008, "executions": 3,
                                "thumb": True, "source_bytes": []}]}]}

    def test_half_open_bounds_and_missing_sources(self):
        result = mapper.summarize(self.fixture(), [("a", 0x1000, 0x1004)],
                                  bytes([1, 2, 3, 4, 0, 0, 0, 0]), [(0x1000, 0, 8)])
        group = result["cores"][0]["ranges"][0]
        self.assertEqual(group["retained_block_executions"], 7)
        self.assertEqual(len(group["blocks"]), 1)
        self.assertEqual(result["verification"]["matched"], 1)
        self.assertEqual(result["verification"]["source_unavailable"], 1)
        self.assertEqual(result["cores"][0]["resident_entries"], 100)

    def test_cross_segment_mapping_and_mismatch(self):
        segments = [(0x1000, 3, 2), (0x1002, 0, 2)]
        result = mapper.summarize(self.fixture(), [], bytes([3, 4, 0, 1, 2]), segments)
        self.assertEqual(result["verification"]["matched"], 1)
        result = mapper.summarize(self.fixture(), [], bytes([3, 9, 0, 1, 2]), segments)
        self.assertEqual(result["verification"]["mismatched"], 1)

    def test_unmapped_is_not_mismatch_or_match(self):
        result = mapper.summarize(self.fixture(), [], bytes([1, 2]), [(0x1000, 0, 2)])
        self.assertEqual(result["verification"]["unmapped"], 1)

    def test_unknown_kind_rejected(self):
        census = self.fixture()
        census["kind"] = "time_samples"
        with self.assertRaises(ValueError):
            mapper.summarize(census, [])

    def test_no_image_has_no_verification_claim(self):
        result = mapper.summarize(self.fixture(), [("a", 0x1000, 0x1008)])
        self.assertEqual(result["verification"]["not_requested"], 2)
        self.assertIsNone(result["image_sha256"])

    def test_bad_range_and_oversized_capture(self):
        with self.assertRaises(ValueError):
            mapper.parse_range("empty=0x1000:0x1000")
        census = self.fixture()
        census["cores"][0]["blocks"][0]["source_bytes"].append(5)
        with self.assertRaises(ValueError):
            mapper.summarize(census, [], bytes(8), [(0x1000, 0, 8)])


if __name__ == "__main__":
    unittest.main()
