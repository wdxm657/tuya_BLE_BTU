import unittest

from water_level_core import MCP1081Parser, WaterLevelAlgorithm


class ParserTests(unittest.TestCase):
    def test_verbose_frame(self):
        parser = MCP1081Parser()
        lines = [
            "T:25.6", "data_ref:30324\tfreq_ref:1.292 MHz",
            "data_ch[0]:23254\tfreq_ch[0]:1.684 MHz\tcap_ch[0]:15.337 pF",
            "data_ch[1]:24321\tfreq_ch[1]:1.610 MHz\tcap_ch[1]:16.041 pF",
            "data_ch[2]:23862\tfreq_ch[2]:1.641 MHz\tcap_ch[2]:15.738 pF",
            "data_ch[3]:21747\tfreq_ch[3]:1.801 MHz\tcap_ch[3]:14.343 pF",
            "data_ch[4]:11145\tfreq_ch[4]:3.514 MHz\tcap_ch[4]:7.310 pF",
            "data_ch[5]:10671\tfreq_ch[5]:3.671 MHz\tcap_ch[5]:6.999 pF",
            "data_ch[6]:23594\tfreq_ch[6]:1.660 MHz\tcap_ch[6]:15.475 pF",
            "data_ch[7]:26151\tfreq_ch[7]:1.498 MHz\tcap_ch[7]:17.152 pF",
            "data_ch[8]:25917\tfreq_ch[8]:1.511 MHz\tcap_ch[8]:16.998 pF",
            "data_ch[9]:24677\tfreq_ch[9]:1.587 MHz\tcap_ch[9]:16.185 pF",
        ]
        frames = [frame for line in lines if (frame := parser.feed_line(line))]
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].cap_pf[:4], (15.337, 16.041, 15.738, 14.343))
        self.assertEqual(frames[0].cap_pf[6:10], (15.475, 17.152, 16.998, 16.185))
        self.assertEqual(frames[0].temperature_c, 25.6)

    def test_compact_frame(self):
        frame = MCP1081Parser().feed_line("S,30000,15000,18000,21000,24000,1,2,3,4,5,6,25.5")
        self.assertIsNotNone(frame)
        self.assertEqual(frame.cap_pf[:4], (10.0, 12.0, 14.0, 16.0))
        self.assertEqual(len(frame.cap_pf), 10)


class AlgorithmTests(unittest.TestCase):
    def test_normalization_and_reverse_channel(self):
        algorithm = WaterLevelAlgorithm(median_window=1, average_window=1)
        algorithm.set_empty((10.0, 10.0, 10.0, 10.0))
        algorithm.set_full((12.0, 14.0, 8.0, 16.0))
        half = algorithm.process((11.0, 12.0, 9.0, 13.0))
        self.assertAlmostEqual(half.level_percent, 50.0)
        self.assertEqual(half.direction, (1, 1, -1, 1))
        full = algorithm.process((12.0, 14.0, 8.0, 16.0))
        self.assertAlmostEqual(full.level_percent, 100.0)


if __name__ == "__main__":
    unittest.main()
