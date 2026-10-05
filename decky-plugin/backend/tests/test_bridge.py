"""End-to-end tests for ds5bridged against a fake kernel and a fake controller.

Run: python3 -m unittest discover -s tests -v   (after building into ./build)
"""
from __future__ import annotations

import math
import os
import struct
import subprocess
import time
import unittest

from harness import (DIR_IN, DIR_OUT, TESTTOOL, DaemonUnderTest, check_output_crc, default_features,
                     firmware_configuration_descriptor, firmware_hid_report_descriptor, make_input_report,
                     neutral_body)

ECONNRESET = -104
EPIPE = -32


def zero_crossing_frequency(samples: list[int], rate: int = 48000) -> float:
    crossings = sum(1 for a, b in zip(samples, samples[1:]) if a < 0 <= b)
    return crossings * rate / len(samples)


def four_channel_packet(start_frame: int, frames: int = 48, speaker_hz: float = 1000.0,
                        haptics_hz: float = 100.0) -> bytes:
    out = bytearray()
    for n in range(start_frame, start_frame + frames):
        spk = int(16000 * math.sin(2 * math.pi * speaker_hz * n / 48000))
        hap = int(20000 * math.sin(2 * math.pi * haptics_hz * n / 48000))
        out += struct.pack("<hhhh", spk, -spk, hap, -hap)
    return bytes(out)


class BridgeTestCase(unittest.TestCase):
    features = None
    extra_args: list[str] = []
    fake_pid = 0x0CE6

    def setUp(self):
        self.d = DaemonUnderTest(self.features, self.extra_args, self.fake_pid)
        self.host = self.d.host
        self.ctrl = self.d.controller

    def tearDown(self):
        self.d.stop()
        if os.environ.get("DS5BRIDGE_TEST_LOG"):
            print(f"\n----- daemon log ({self.id()}) -----\n{self.d.log_text()}")

    def stream_audio(self, seconds: float, urb_packets: int = 8, in_flight: int = 3, **kwargs):
        """Feeds the speaker/haptics endpoint like snd-usb-audio does."""
        frame = 0
        pending = []
        total_urbs = int(seconds * 1000 / urb_packets)
        submitted = 0
        completions = []
        while submitted < total_urbs or pending:
            while submitted < total_urbs and len(pending) < in_flight:
                data = b"".join(four_channel_packet(frame + 48 * i, **kwargs) for i in range(urb_packets))
                frame += 48 * urb_packets
                pending.append(self.host.submit(1, DIR_OUT, len(data), data, iso_lengths=[384] * urb_packets))
                submitted += 1
            urb = pending.pop(0)
            self.assertTrue(urb.done.wait(2), "iso OUT URB never completed")
            self.assertEqual(urb.status, 0)
            completions.append(urb.completed_at)
        return completions


class EnumerationTest(BridgeTestCase):
    def test_device_descriptor(self):
        dev = self.host.control(0x80, 6, 0x0100, 0, 18)
        self.assertEqual(dev.status, 0)
        self.assertEqual(len(dev.data), 18)
        vid, pid = struct.unpack("<HH", dev.data[8:12])
        self.assertEqual((vid, pid), (0x054C, 0x0CE6))

    def test_short_device_descriptor_read(self):
        dev = self.host.control(0x80, 6, 0x0100, 0, 8)
        self.assertEqual(len(dev.data), 8)
        self.assertEqual(dev.data[7], 64)

    def test_configuration_matches_firmware(self):
        config = self.host.control(0x80, 6, 0x0200, 0, 512)
        self.assertEqual(config.data, firmware_configuration_descriptor())
        self.assertEqual(len(config.data), 227)

    def test_hid_report_descriptor_matches_firmware(self):
        report = self.host.control(0x81, 6, 0x2200, 3, 512)
        self.assertEqual(report.data, firmware_hid_report_descriptor("desc_hid_report_ds"))

    def test_strings(self):
        langs = self.host.control(0x80, 6, 0x0300, 0, 255)
        self.assertEqual(langs.data, b"\x04\x03\x09\x04")
        product = self.host.control(0x80, 6, 0x0302, 0x0409, 255)
        self.assertEqual(product.data[2:].decode("utf-16-le"), "DualSense Wireless Controller")
        manufacturer = self.host.control(0x80, 6, 0x0301, 0x0409, 255)
        self.assertEqual(manufacturer.data[2:].decode("utf-16-le"), "Sony Interactive Entertainment")

    def test_unknown_descriptor_stalls(self):
        qualifier = self.host.control(0x80, 6, 0x0600, 0, 10)
        self.assertEqual(qualifier.status, EPIPE)

    def test_set_configuration_and_interface(self):
        self.assertEqual(self.host.control(0x00, 9, 1, 0, 0).status, 0)
        self.assertEqual(self.host.control(0x80, 8, 0, 0, 1).data, b"\x01")
        self.assertEqual(self.host.control(0x01, 0x0B, 1, 1, 0).status, 0)
        self.assertEqual(self.host.control(0x81, 0x0A, 0, 1, 1).data, b"\x01")
        self.assertEqual(self.host.control(0x01, 0x0B, 2, 1, 0).status, EPIPE)


class HidTest(BridgeTestCase):
    def test_prefetched_feature_reports(self):
        features = default_features()
        for report_id, size in ((0x05, 41), (0x09, 20), (0x20, 64)):
            urb = self.host.control(0xA1, 0x01, 0x0300 | report_id, 3, size)
            self.assertEqual(urb.status, 0)
            self.assertEqual(urb.data, features[report_id][:size])

    def test_feature_report_fetched_on_demand(self):
        urb = self.host.control(0xA1, 0x01, 0x0381, 3, 64)
        self.assertEqual(urb.status, 0)
        self.assertEqual(urb.data, default_features()[0x81])
        self.assertIn(0x81, self.ctrl.feature_requests)

    def test_missing_feature_report_stalls(self):
        urb = self.host.control(0xA1, 0x01, 0x03F0, 3, 64)
        self.assertEqual(urb.status, EPIPE)

    def test_input_report_forwarded(self):
        self.d.enumerate()
        urb = self.host.submit(4, DIR_IN, 64)
        body = neutral_body()
        body[7] = 0x28  # cross pressed
        self.ctrl.send_input(make_input_report(bytes(body)))
        self.assertTrue(urb.done.wait(2))
        self.assertEqual(urb.status, 0)
        self.assertEqual(urb.data, b"\x01" + bytes(body))

    def test_input_report_queued_until_urb(self):
        self.d.enumerate()
        body = neutral_body()
        self.ctrl.send_input(make_input_report(bytes(body)))
        time.sleep(0.1)
        urb = self.host.submit(4, DIR_IN, 64)
        self.assertTrue(urb.done.wait(2))
        self.assertEqual(urb.data, b"\x01" + bytes(body))

    def test_output_report_translated(self):
        self.d.enumerate()
        since = time.monotonic()
        payload = bytearray(47)
        payload[1] = 0x04  # AllowLedColor
        payload[44:47] = b"\x10\x20\x30"
        urb = self.host.submit(3, DIR_OUT, 48, b"\x02" + bytes(payload))
        self.assertTrue(urb.done.wait(2))
        report = self.ctrl.wait_for(lambda r: r[0] == 0x31, since=since)
        self.assertEqual(len(report), 78)
        self.assertEqual(report[1] & 0x0F, 0)
        self.assertEqual(report[2], 0x10)
        # Like the firmware, the default speaker_gain (2) is applied to every report.
        payload[1] |= 0x80  # AllowAudioControl2
        payload[37] |= 2    # SpeakerCompPreGain
        self.assertEqual(report[3:50], bytes(payload))
        self.assertEqual(report[50:74], bytes(24))
        self.assertTrue(check_output_crc(report))

    def test_output_report_via_control_pipe(self):
        since = time.monotonic()
        payload = bytes([0x01, 0x00, 0x7F, 0x7F]) + bytes(43)
        urb = self.host.control(0x21, 0x09, 0x0202, 3, 48, b"\x02" + payload)
        self.assertEqual(urb.status, 0)
        report = self.ctrl.wait_for(lambda r: r[0] == 0x31, since=since)
        self.assertEqual(report[3], 0x01)
        self.assertEqual(report[5:7], b"\x7f\x7f")

    def test_config_overrides_output_report(self):
        self.assertEqual(self.d.ipc("SET trigger_reduce 5"), "OK")
        self.assertEqual(self.d.ipc("SET lock_volume 1"), "OK")
        since = time.monotonic()
        payload = bytearray(47)
        payload[0] = 0x70  # Allow{Headphone,Speaker,Mic}Volume
        self.host.submit(3, DIR_OUT, 48, b"\x02" + bytes(payload)).done.wait(2)
        report = self.ctrl.wait_for(lambda r: r[0] == 0x31, since=since)
        state = report[3:66]
        self.assertEqual(state[0] & 0x70, 0, "lock_volume must clear the volume flags")
        self.assertTrue(state[1] & 0x40, "AllowMotorPowerLevel")
        self.assertEqual(state[36] >> 4, 5)

    def test_setting_change_sends_state(self):
        since = time.monotonic()
        self.assertEqual(self.d.ipc("SET speaker_gain 5"), "OK")
        report = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[3] == 0x3F, since=since)
        state = report[4:67]
        self.assertTrue(state[1] & 0x80, "AllowAudioControl2")
        self.assertEqual(state[37] & 0x07, 5)
        self.assertTrue(check_output_crc(report))

    def test_mute_light_follows_mic_mute(self):
        since = time.monotonic()
        body = neutral_body()
        body[53] = 0x04
        self.ctrl.send_input(make_input_report(bytes(body)))
        report = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[4 + 1] & 0x01, since=since)
        self.assertEqual(report[4 + 8], 1)

    def test_unlink_pending_interrupt_urb(self):
        self.d.enumerate()
        urb = self.host.submit(4, DIR_IN, 64)
        time.sleep(0.05)
        marker = self.host.unlink(urb)
        self.assertTrue(marker.done.wait(2))
        self.assertEqual(marker.unlink_status, ECONNRESET)
        self.ctrl.send_input(make_input_report(bytes(neutral_body())))
        time.sleep(0.1)
        self.assertFalse(urb.done.is_set(), "unlinked URB must not complete")

    def test_initial_state_sets_light_bar(self):
        report = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[3] == 0x3F and r[4 + 1] & 0x04)
        self.assertEqual(report[4 + 44:4 + 47], b"\xff\xd7\x00")


class AudioControlTest(BridgeTestCase):
    def test_speaker_volume(self):
        since = time.monotonic()
        urb = self.host.control(0x21, 0x01, 0x0200, 0x0200, 2, struct.pack("<h", -20 * 256))
        self.assertEqual(urb.status, 0)
        report = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[4] & 0x30, since=since)
        self.assertEqual(report[4 + 4], 80)
        self.assertEqual(report[4 + 5], 80)
        cur = self.host.control(0xA1, 0x81, 0x0200, 0x0200, 2)
        self.assertEqual(struct.unpack("<h", cur.data)[0], -20 * 256)

    def test_volume_ranges_match_firmware(self):
        self.assertEqual(self.host.control(0xA1, 0x82, 0x0200, 0x0200, 2).data, b"\x00\x9c")
        self.assertEqual(self.host.control(0xA1, 0x83, 0x0200, 0x0200, 2).data, b"\x00\x00")
        self.assertEqual(self.host.control(0xA1, 0x84, 0x0200, 0x0200, 2).data, b"\x00\x01")
        self.assertEqual(self.host.control(0xA1, 0x83, 0x0200, 0x0500, 2).data, b"\x00\x30")
        self.assertEqual(self.host.control(0xA1, 0x81, 0x0200, 0x0500, 2).data, b"\x00\x30")

    def test_mute(self):
        since = time.monotonic()
        self.assertEqual(self.host.control(0x21, 0x01, 0x0100, 0x0200, 1, b"\x01").status, 0)
        report = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[4 + 1] & 0x02, since=since)
        self.assertEqual(report[4 + 9] & 0x60, 0x60)  # speaker + headphone mute
        self.assertEqual(self.host.control(0xA1, 0x81, 0x0100, 0x0200, 1).data, b"\x01")

    def test_sampling_frequency(self):
        self.assertEqual(self.host.control(0x22, 0x01, 0x0100, 0x01, 3, b"\x80\xbb\x00").status, 0)
        self.assertEqual(self.host.control(0xA2, 0x81, 0x0100, 0x01, 3).data, b"\x80\xbb\x00")


class AudioStreamTest(BridgeTestCase):
    def setUp(self):
        super().setUp()
        self.d.enumerate()
        self.assertEqual(self.host.control(0x01, 0x0B, 1, 1, 0).status, 0)

    def test_iso_out_is_paced_like_a_usb_bus(self):
        start = time.monotonic()
        completions = self.stream_audio(0.4)
        elapsed = completions[-1] - start
        self.assertGreater(elapsed, 0.36, "400 packets must take ~400 ms")
        self.assertLess(elapsed, 0.6)

    def test_haptics_and_speaker_reports(self):
        since = time.monotonic()
        self.stream_audio(1.0)
        reports = self.ctrl.reports(0x39, since)
        # 48000 frames / 1024 frames per report = 46.9 reports
        self.assertGreaterEqual(len(reports), 44)
        self.assertLessEqual(len(reports), 48)
        first = reports[0]
        self.assertEqual(len(first), 547)
        self.assertTrue(check_output_crc(first))
        self.assertEqual(first[2], 0x91)
        self.assertEqual(first[3], 6)
        self.assertEqual(first[4], 0x7E)
        self.assertEqual(first[5:9], bytes([48] * 4))
        self.assertEqual(first[10], 0xD2)
        self.assertEqual(first[11], 64)
        self.assertEqual(first[140], 0xD3)  # built-in speaker (no headset plugged)
        self.assertEqual(first[141], 200)
        counters = [r[9] for r in reports]
        self.assertEqual([(b - a) & 0xFF for a, b in zip(counters, counters[1:])], [2] * (len(counters) - 1))

        # Haptics: channels 3/4 decimated 48k -> 3k, int8, gain 1.0.
        expected = bytearray()
        for k in range(64):
            n = 16 * k
            hap = int(20000 * math.sin(2 * math.pi * 100.0 * n / 48000))
            for v in (hap, -hap):
                expected.append(max(-128, min(127, int(v / 32768.0 * 127.0))) & 0xFF)
        self.assertEqual(first[12:140], bytes(expected))

        # Speaker: decode the Opus packets and look at the tone.
        lines = []
        for r in reports[2:]:
            lines.append(r[142:342].hex())
            lines.append(r[342:542].hex())
        pcm = subprocess.run([str(TESTTOOL), "decode-speaker"], input="\n".join(lines).encode(),
                             capture_output=True, check=True).stdout
        left = list(struct.unpack(f"<{len(pcm) // 2}h", pcm))[0::2]
        self.assertGreater(max(left), 8000)
        # 512 frames are squeezed into 480 like the firmware does.
        self.assertAlmostEqual(zero_crossing_frequency(left), 1000 * 512 / 480, delta=30)

    def test_haptics_gain(self):
        self.assertEqual(self.d.ipc("SET haptics_gain 2.0"), "OK")
        since = time.monotonic()
        self.stream_audio(0.1)
        first = self.ctrl.reports(0x39, since)[0]
        hap = int(20000 * math.sin(2 * math.pi * 100.0 * 16 / 48000))
        self.assertEqual(struct.unpack("b", first[14:15])[0], max(-128, min(127, int(hap / 32768.0 * 127.0 * 2.0))))

    def test_speaker_disabled_sends_haptics_only(self):
        self.assertEqual(self.d.ipc("SET speaker_select 3"), "OK")
        since = time.monotonic()
        self.stream_audio(0.3)
        reports = self.ctrl.reports(0x39, since)
        self.assertGreaterEqual(len(reports), 12)
        self.assertEqual(reports[0][140:542], bytes(402))

    def test_headset_routes_speaker_to_headphones(self):
        body = neutral_body()
        body[53] = 0x01  # headphones plugged
        self.ctrl.send_input(make_input_report(bytes(body)))
        time.sleep(0.05)
        since = time.monotonic()
        self.stream_audio(0.1)
        self.assertEqual(self.ctrl.reports(0x39, since)[0][140], 0xD6)


class MicTest(BridgeTestCase):
    def test_mic_stream(self):
        self.d.enumerate()
        since = time.monotonic()
        self.assertEqual(self.host.control(0x01, 0x0B, 1, 2, 0).status, 0)
        status = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[3] == 1, since=since)
        self.assertEqual(status[4], 0x03)

        frames = subprocess.run([str(TESTTOOL), "encode-mic", "440", "60"], capture_output=True, check=True,
                                text=True).stdout.split()
        import threading

        def feed():
            for f in frames:
                report = bytearray(78)
                report[0] = 0x31
                report[1] = 0x02  # mic payload flag
                report[3:74] = bytes.fromhex(f)
                self.ctrl.send_input(bytes(report))
                time.sleep(0.01)

        feeder = threading.Thread(target=feed)
        feeder.start()
        pcm = b""
        pending = []
        for _ in range(60):
            while len(pending) < 3:
                pending.append(self.host.submit(2, DIR_IN, 196 * 8, iso_lengths=[196] * 8))
            urb = pending.pop(0)
            self.assertTrue(urb.done.wait(2))
            self.assertEqual(urb.status, 0)
            self.assertEqual(len(urb.iso_packets), 8)
            self.assertTrue(all(p[2] == 192 for p in urb.iso_packets))
            pcm += urb.data
        feeder.join()

        samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
        left, right = samples[0::2], samples[1::2]
        self.assertEqual(left, right, "mono mic must be duplicated to both channels")
        voiced = [s for s in left if s != 0]
        self.assertGreater(len(voiced), 48 * 200)
        start = next(i for i, s in enumerate(left) if s != 0) + 4800
        self.assertAlmostEqual(zero_crossing_frequency(list(left[start:start + 9600])), 440, delta=25)

        since = time.monotonic()
        self.assertEqual(self.host.control(0x01, 0x0B, 0, 2, 0).status, 0)
        status = self.ctrl.wait_for(lambda r: r[0] == 0x32 and r[3] == 1, since=since)
        self.assertEqual(status[4], 0x02)


class StatusTest(BridgeTestCase):
    def test_status(self):
        self.ctrl.send_input(make_input_report(bytes(neutral_body())))
        self.d.enumerate()
        time.sleep(0.1)
        status = self.d.status()
        self.assertEqual(len(status["controllers"]), 1)
        c = status["controllers"][0]
        self.assertEqual(c["battery"], 75)
        self.assertEqual(c["usb"], "connected")
        self.assertEqual(c["presented_as"], "DualSense")
        self.assertEqual(status["config"]["haptics_gain"], 1.0)

    def test_rejects_bad_settings(self):
        self.assertTrue(self.d.ipc("SET haptics_gain 5").startswith("ERR"))
        self.assertTrue(self.d.ipc("SET nope 1").startswith("ERR"))
        self.assertEqual(self.d.ipc("SET lightbar_color #102030"), "OK")
        self.assertEqual(self.d.status()["config"]["lightbar_color"], "#102030")

    def test_controller_disconnect_detaches_usb(self):
        self.ctrl.disconnect()
        self.assertTrue(self.host.closed.wait(3), "virtual device must go away with the controller")
        time.sleep(0.1)
        self.assertEqual(self.d.status()["controllers"], [])


def edge_features():
    features = default_features()
    firmware = bytearray(features[0x20])
    firmware[22] = 0x44
    features[0x20] = bytes(firmware)
    return features


class EdgeAutoTest(BridgeTestCase):
    features = edge_features()
    fake_pid = 0x0DF2

    def test_edge_presented_as_edge(self):
        dev = self.host.control(0x80, 6, 0x0100, 0, 18)
        self.assertEqual(struct.unpack("<H", dev.data[10:12])[0], 0x0DF2)
        report = self.host.control(0x81, 6, 0x2200, 3, 1024)
        self.assertEqual(report.data, firmware_hid_report_descriptor("desc_hid_report_dse"))
        product = self.host.control(0x80, 6, 0x0302, 0x0409, 255)
        self.assertEqual(product.data[2:].decode("utf-16-le"), "DualSense Edge Wireless Controller")


class EdgeAsDualSenseTest(BridgeTestCase):
    features = edge_features()
    fake_pid = 0x0DF2
    extra_args = ["--set", "controller_mode=0"]

    def test_edge_presented_as_dualsense(self):
        dev = self.host.control(0x80, 6, 0x0100, 0, 18)
        self.assertEqual(struct.unpack("<H", dev.data[10:12])[0], 0x0CE6)
        info = self.host.control(0xA1, 0x01, 0x0320, 3, 64)
        self.assertEqual(info.data[:4], b"\x20Jul")  # firmware's fake DualSense report 0x20


if __name__ == "__main__":
    unittest.main()
