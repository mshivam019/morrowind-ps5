import importlib.util
from pathlib import Path
import unittest
p=Path(__file__).resolve().parents[1]/'tools/analyze-frame-pacing.py'
spec=importlib.util.spec_from_file_location('pacing',p);pacing=importlib.util.module_from_spec(spec);spec.loader.exec_module(pacing)


class PacingTests(unittest.TestCase):
 def test_missing_interval_lines_keep_actual_reporter_cadence(self):
  result=pacing.analyze('[ps5-cpu-profile] present=180 wall_ms=2000 draws=600\n'
                        '[ps5-cpu-profile] present=300 wall_ms=1000 draws=1200')
  self.assertEqual(result['driver_interval_summary']['frames'],120)
  self.assertEqual(result['driver_interval_summary']['weighted_fps'],40)
  self.assertEqual(result['latest_driver_window']['cpu_per_frame']['draws'],20)
 def test_native_join_and_single_frame_warmup(self):
  result=pacing.analyze('[ps5-cpu-profile] present=1 wall_ms=0\n'
                       '[ps5-native-profile] present=2 present_vblank_ms=10\n'
                       '[ps5-cpu-profile] present=2 wall_ms=20')
  self.assertEqual(result['driver_window_count'],1)
  self.assertEqual(result['latest_driver_window']['native_per_frame']['present_vblank_ms'],10)
 def test_engine_hex_ids_and_sample_only_stats(self):
  result=pacing.analyze('PS5 frame timing 3c update_ms=2 render_ms=30 lua_wait_ms=0.1\n'
                       'PS5 frame timing 78 update_ms=3 render_ms=40 lua_wait_ms=0.2')
  self.assertEqual(result['engine_sample_count'],2)
  self.assertAlmostEqual(result['sampled_engine_ms']['median'],37.65)
  self.assertEqual(result['driver_window_count'],0)
 def test_light_cumulative_counters_are_not_timing_windows(self):
  result=pacing.analyze('[ps5-present-diag] present=120 cumulative=1 cpu_flips=100 marker_waits=3\n'
                       '[ps5-batch-breaks] present=120 cumulative=1 capacity_batches=8 capacity_draws=1024\n'
                       '[ps5-present-diag] present=240 cumulative=1 cpu_flips=200 marker_waits=5\n'
                       '[ps5-present-diag] present=120 cumulative=1 cpu_flips=2 marker_waits=1')
  self.assertEqual(result['driver_window_count'],0)
  self.assertEqual(result['latest_light_diagnostics']['ps5-present-diag']['cpu_flips'],2)
  self.assertEqual(result['latest_light_diagnostics']['ps5-batch-breaks']['capacity_draws'],1024)
  self.assertEqual(result['light_diagnostic_reset_boundaries']['ps5-present-diag'],1)
  self.assertEqual(result['light_diagnostic_record_counts']['ps5-present-diag'],3)
 def test_light_integer_counters_preserve_precision(self):
  result=pacing.analyze('[ps5-batch-breaks] present=120 cumulative=1 resource_hazard_draws=9007199254740993')
  self.assertEqual(result['latest_light_diagnostics']['ps5-batch-breaks']['resource_hazard_draws'],9007199254740993)
 def test_startup_separated_from_steady_samples(self):
  result=pacing.analyze('PS5 frame timing 1 update_ms=100 render_ms=900 lua_wait_ms=0\n'
                       'PS5 frame timing 3c update_ms=2 render_ms=30 lua_wait_ms=0')
  self.assertEqual(result['sampled_engine_ms']['maximum'],1000)
  self.assertEqual(result['sampled_engine_after_first_three_frames_ms']['maximum'],32)
 def test_signatures_not_frequencies_and_malformed_records(self):
  result=pacing.analyze('[ps5-outer-batch-reject] mode=6\n[ps5-outer-batch-reject] mode=6\n'
                       '[ps5-cpu-profile] present=150 wall_ms=20\nPS5 frame timing 3c render_ms=30')
  self.assertEqual(len(result['rejection_signatures']),1)
  self.assertEqual(result['driver_window_count'],0)
  self.assertEqual(result['engine_sample_count'],0)

if __name__=='__main__':unittest.main()
