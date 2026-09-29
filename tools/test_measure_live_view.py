import unittest

from measure_live_view import evaluate


class MeasurementTests(unittest.TestCase):
    def test_rejects_latency_or_display_regression_despite_fast_capture(self):
        capture = dict(frames=70, window_ms=5000, jpeg_avg_bytes=32000,
                       jpeg_width=640, jpeg_height=424, frame_failures=0)
        display = dict(frames=47, window_ms=5000, ready_to_display_avg_ms=160,
                       display_failures=0)

        def accepted(c=capture, d=display):
            return evaluate([c], [d] if d else [], min_fps=9, width=640, height=424,
                            min_display_fps=9, max_display_age_ms=200)[0]

        self.assertTrue(accepted())
        self.assertFalse(accepted(d={**display, "frames": 40}))
        self.assertFalse(accepted(d={**display, "ready_to_display_avg_ms": 240}))
        self.assertFalse(accepted(d={**display, "display_failures": 1}))
        self.assertFalse(accepted(d=None))
        self.assertFalse(accepted(c={**capture, "jpeg_width": 320}))
        self.assertFalse(accepted(c={**capture, "jpeg_height": 212}))
        self.assertFalse(accepted(c={**capture, "frame_failures": 1}))
        self.assertFalse(accepted(c={**capture, "frames": 0}))


if __name__ == "__main__":
    unittest.main()
