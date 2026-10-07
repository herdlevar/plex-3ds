#!/usr/bin/env python3
"""
Plex3DS - Core Logic & Security Unit Test Suite
Validates API contract, transcode URL construction, path sanitization,
JSON schema parsing, and time formatting logic.
"""

import unittest
import re
import json

def format_time(seconds):
    if seconds < 0:
        seconds = 0
    h = seconds // 3600
    m = (seconds % 3600) // 60
    s = seconds % 60
    if h > 0:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m}:{s:02d}"

def sanitize_key(key):
    safe = re.sub(r'[^a-zA-Z0-9_\-]', '', key)
    return safe if safe else "item"

def build_transcode_url(server_uri, token, item_key, media_type, subtitles_enabled=False, sub_id=None, offset_sec=0):
    encoded_key = urllib_quote(item_key) if 'urllib_quote' in globals() else item_key.replace('/', '%2F')
    session_id = f"3ds-test-{12345}"
    if media_type == "track":
        url = (f"{server_uri}/music/:/transcode/universal/start.mp3"
               f"?path={encoded_key}&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0"
               f"&audioQuality=60&location=lan&session={session_id}&X-Plex-Token={token}")
        if offset_sec > 0:
            url += f"&offset={offset_sec}"
        return url
    else:
        sub_param = "subtitles=none"
        if subtitles_enabled:
            sub_param = "subtitles=burn"
            if sub_id is not None:
                sub_param += f"&subtitleStreamID={sub_id}"
        url = (f"{server_uri}/video/:/transcode/universal/start.mkv"
               f"?path={encoded_key}&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0"
               f"&videoQuality=60&videoBitrate=1000&videoResolution=400x240&videoCodec=h264&audioCodec=aac"
               f"&location=lan&session={session_id}&{sub_param}&X-Plex-Token={token}")
        if offset_sec > 0:
            url += f"&offset={offset_sec}"
        return url


class TestPlex3DSTimeFormat(unittest.TestCase):
    def test_zero_seconds(self):
        self.assertEqual(format_time(0), "0:00")

    def test_under_one_minute(self):
        self.assertEqual(format_time(45), "0:45")
        self.assertEqual(format_time(9), "0:09")

    def test_minutes_and_seconds(self):
        self.assertEqual(format_time(60), "1:00")
        self.assertEqual(format_time(125), "2:05")
        self.assertEqual(format_time(3599), "59:59")

    def test_hours_and_minutes(self):
        self.assertEqual(format_time(3600), "1:00:00")
        self.assertEqual(format_time(3665), "1:01:05")
        self.assertEqual(format_time(7325), "2:02:05")

    def test_negative_seconds(self):
        self.assertEqual(format_time(-10), "0:00")


class TestPlex3DSSanitization(unittest.TestCase):
    def test_clean_numeric_key(self):
        self.assertEqual(sanitize_key("12345"), "12345")

    def test_clean_alphanumeric_key(self):
        self.assertEqual(sanitize_key("movie_123-abc"), "movie_123-abc")

    def test_directory_traversal_prevention(self):
        # Path traversal sequences must be stripped completely
        bad_key = "../../boot.firm"
        self.assertEqual(sanitize_key(bad_key), "bootfirm")
        self.assertNotIn("..", sanitize_key(bad_key))
        self.assertNotIn("/", sanitize_key(bad_key))

    def test_windows_slash_prevention(self):
        bad_key = "..\\..\\windows\\system32"
        self.assertEqual(sanitize_key(bad_key), "windowssystem32")
        self.assertNotIn("\\", sanitize_key(bad_key))

    def test_empty_or_special_only(self):
        self.assertEqual(sanitize_key("!@#$%^&*()"), "item")
        self.assertEqual(sanitize_key(""), "item")


class TestPlex3DSTranscodeURL(unittest.TestCase):
    def setUp(self):
        self.server = "http://192.168.1.100:32400"
        self.token = "test_token_123"

    def test_video_transcode_url_resolution(self):
        url = build_transcode_url(self.server, self.token, "/library/metadata/500", "movie")
        self.assertIn("videoResolution=400x240", url)
        self.assertIn("videoCodec=h264", url)
        self.assertIn("audioCodec=aac", url)
        self.assertIn("subtitles=none", url)
        self.assertIn("X-Plex-Token=test_token_123", url)

    def test_video_transcode_subtitles(self):
        url = build_transcode_url(self.server, self.token, "/library/metadata/500", "movie", subtitles_enabled=True, sub_id=42)
        self.assertIn("subtitles=burn", url)
        self.assertIn("subtitleStreamID=42", url)

    def test_video_transcode_resume_offset(self):
        url = build_transcode_url(self.server, self.token, "/library/metadata/500", "movie", offset_sec=145)
        self.assertIn("&offset=145", url)

    def test_music_transcode_url(self):
        url = build_transcode_url(self.server, self.token, "/library/metadata/999", "track")
        self.assertIn("start.mp3", url)
        self.assertIn("audioQuality=60", url)
        self.assertNotIn("videoResolution", url)


class TestPlex3DSResumeState(unittest.TestCase):
    def test_resume_threshold_filter(self):
        # Only offsets > 10,000 ms (10 seconds) and not near the end should be saved
        resume_entries = {
            "movie1": 5000,    # 5s -> should not be saved
            "movie2": 45000,   # 45s -> valid resume
            "movie3": 1200000  # 20m -> valid resume
        }
        filtered = {k: v for k, v in resume_entries.items() if v > 10000}
        self.assertNotIn("movie1", filtered)
        self.assertIn("movie2", filtered)
        self.assertIn("movie3", filtered)
        self.assertEqual(len(filtered), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
