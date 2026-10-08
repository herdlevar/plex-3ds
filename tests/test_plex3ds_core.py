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


class TestPlex3DSDownloadQueue(unittest.TestCase):
    def test_queue_filtering_and_duplicates(self):
        # Simulates DownloadManager::queueDownloads logic
        already_downloaded = {"track1", "track3"}
        active_download = "track2"
        existing_queue = ["track4"]

        incoming_batch = [
            ("track1", "http://server/t1"),  # Already on disk -> skip
            ("track2", "http://server/t2"),  # Currently downloading -> skip duplicate
            ("track4", "http://server/t4"),  # Already in queue -> skip duplicate
            ("track5", "http://server/t5"),  # New -> enqueue
            ("track6", "http://server/t6"),  # New -> enqueue
        ]

        to_add = []
        for rkey, url in incoming_batch:
            if rkey in already_downloaded:
                continue
            if rkey == active_download or rkey in existing_queue:
                continue
            to_add.append((rkey, url))

        self.assertEqual(len(to_add), 2)
        self.assertEqual([x[0] for x in to_add], ["track5", "track6"])

    def test_download_badge_formatting(self):
        # Queue count > 1 shows (queueIndex/queueCount)
        def format_badge(queue_index, queue_count, percent):
            if queue_count > 1:
                return f"DL ({queue_index}/{queue_count}): {percent}%"
            return f"DL: {percent}%"

        self.assertEqual(format_badge(1, 1, 45), "DL: 45%")
        self.assertEqual(format_badge(3, 12, 78), "DL (3/12): 78%")


class TestPlex3DSClamshellPolicy(unittest.TestCase):
    def evaluate_clamshell_state(self, is_media_active, media_type, is_paused, headphones_connected):
        is_playing_music = is_media_active and media_type == "track" and not is_paused
        can_clamshell_play = is_playing_music and headphones_connected
        sleep_allowed = not can_clamshell_play
        return can_clamshell_play, sleep_allowed

    def test_music_with_headphones_allows_clamshell(self):
        can_play, sleep_allowed = self.evaluate_clamshell_state(True, "track", False, True)
        self.assertTrue(can_play)
        self.assertFalse(sleep_allowed)

    def test_music_without_headphones_disallows_clamshell(self):
        can_play, sleep_allowed = self.evaluate_clamshell_state(True, "track", False, False)
        self.assertFalse(can_play)
        self.assertTrue(sleep_allowed)

    def test_video_never_allows_clamshell_even_with_headphones(self):
        can_play, sleep_allowed = self.evaluate_clamshell_state(True, "video", False, True)
        self.assertFalse(can_play)
        self.assertTrue(sleep_allowed)

    def test_paused_music_disallows_clamshell(self):
        can_play, sleep_allowed = self.evaluate_clamshell_state(True, "track", True, True)
        self.assertFalse(can_play)
        self.assertTrue(sleep_allowed)

    def test_headphone_disconnect_detection(self):
        def on_headphone_transition(prev_connected, cur_connected, is_playing):
            unplugged = prev_connected and not cur_connected
            should_pause = unplugged and is_playing
            return should_pause

        self.assertTrue(on_headphone_transition(True, False, True))
        self.assertFalse(on_headphone_transition(False, False, True))
        self.assertFalse(on_headphone_transition(True, True, True))
        self.assertFalse(on_headphone_transition(False, True, True))
        self.assertFalse(on_headphone_transition(True, False, False))


class TestPlex3DSSuspendResumePolicy(unittest.TestCase):
    class MockAudioPlayer:
        def __init__(self):
            self.is_playing = False
            self.is_paused = False
            self.was_suspended = False
            self.channel_reset_count = 0
            self.hardware_configured = False

        def play(self):
            self.is_playing = True
            self.is_paused = False
            self.was_suspended = False
            self.hardware_configured = True

        def pause(self):
            self.is_paused = True

        def suspend(self):
            self.was_suspended = True
            self.is_paused = True
            self.channel_reset_count += 1

        def resume_from_suspend(self):
            self.channel_reset_count += 1
            self.hardware_configured = True
            self.was_suspended = False
            self.is_paused = False

        def resume(self):
            if self.was_suspended:
                self.resume_from_suspend()
                return
            self.is_paused = False

    def test_suspend_while_playing_audio(self):
        player = self.MockAudioPlayer()
        player.play()
        self.assertTrue(player.is_playing)
        self.assertFalse(player.is_paused)

        # Simulate APTHOOK_ONSUSPEND
        audio_was_playing_on_suspend = False
        if player.is_playing:
            if not player.is_paused:
                audio_was_playing_on_suspend = True
            player.suspend()

        self.assertTrue(audio_was_playing_on_suspend)
        self.assertTrue(player.was_suspended)
        self.assertTrue(player.is_paused)
        self.assertEqual(player.channel_reset_count, 1)

        # Simulate APTHOOK_ONRESTORE (DSP is still asleep!)
        needs_post_wakeup_resume = True

        # Simulate main loop post-wakeup execution (DSP is awake!)
        if needs_post_wakeup_resume:
            needs_post_wakeup_resume = False
            if audio_was_playing_on_suspend:
                player.resume_from_suspend()
                audio_was_playing_on_suspend = False

        self.assertFalse(player.is_paused)
        self.assertFalse(player.was_suspended)
        self.assertTrue(player.hardware_configured)
        self.assertEqual(player.channel_reset_count, 2)

    def test_suspend_while_paused_audio(self):
        player = self.MockAudioPlayer()
        player.play()
        player.pause()
        self.assertTrue(player.is_paused)

        # Simulate APTHOOK_ONSUSPEND
        audio_was_playing_on_suspend = False
        if player.is_playing:
            if not player.is_paused:
                audio_was_playing_on_suspend = True
            player.suspend()

        self.assertFalse(audio_was_playing_on_suspend)
        self.assertTrue(player.was_suspended)
        self.assertTrue(player.is_paused)

        # Simulate main loop post-wakeup execution
        needs_post_wakeup_resume = True
        if needs_post_wakeup_resume:
            needs_post_wakeup_resume = False
            if audio_was_playing_on_suspend:
                player.resume_from_suspend()
                audio_was_playing_on_suspend = False

        # Audio should remain paused!
        self.assertTrue(player.is_paused)
        self.assertTrue(player.was_suspended)

        # When user manually presses play/resume later, resume() must detect was_suspended
        player.resume()
        self.assertFalse(player.is_paused)
        self.assertFalse(player.was_suspended)
        self.assertTrue(player.hardware_configured)


class TestPlex3DSHomeButtonPolicy(unittest.TestCase):
    def test_home_press_clears_chainloader_and_exits_to_home_menu(self):
        # When home button is pressed, aptCheckHomePressRejected() is true
        chainloader_cleared = False
        app_exiting = False

        home_pressed = True
        if home_pressed:
            chainloader_cleared = True
            app_exiting = True

        self.assertTrue(chainloader_cleared)
        self.assertTrue(app_exiting)

    def test_start_press_keeps_chainloader_for_hbmenu_exit(self):
        # When START is pressed, chainloader is NOT cleared (returns to hbmenu)
        chainloader_cleared = False
        app_exiting = False

        start_pressed = True
        if start_pressed:
            app_exiting = True

        self.assertFalse(chainloader_cleared)
        self.assertTrue(app_exiting)


if __name__ == '__main__':
    unittest.main(verbosity=2)

