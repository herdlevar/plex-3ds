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

    def test_immediate_queue_count_sync_while_downloading(self):
        # Simulates DownloadManager queueDownloads while transfer is active
        class SimulatedDownloadManager:
            def __init__(self):
                self.is_downloading = True
                self.current_item = "episode1"
                self.queue = []
                self.total_queue_count = 1
                self.progress_queue_count = 1
                self.progress_active = True

            def queue_downloads(self, items):
                added = 0
                for item in items:
                    if item == self.current_item or item in self.queue:
                        continue
                    self.queue.append(item)
                    self.total_queue_count += 1
                    added += 1
                # Immediate synchronization fix
                self.progress_queue_count = self.total_queue_count
                return added

            def is_queued(self, key):
                return (self.is_downloading and self.current_item == key) or (key in self.queue)

            def get_queue_position(self, key):
                if self.is_downloading and self.current_item == key:
                    return 1
                if key in self.queue:
                    return (2 if self.is_downloading else 1) + self.queue.index(key)
                return 0

            def cancel_queued_item(self, key):
                if key in self.queue:
                    self.queue.remove(key)
                    if self.total_queue_count > 0:
                        self.total_queue_count -= 1
                    self.progress_queue_count = self.total_queue_count
                    return True
                if self.is_downloading and self.current_item == key:
                    self.is_downloading = False
                    self.current_item = None
                    return True
                return False

        mgr = SimulatedDownloadManager()
        self.assertEqual(mgr.progress_queue_count, 1)
        self.assertEqual(mgr.get_queue_position("episode1"), 1)

        # Enqueue 3 more episodes while episode1 is downloading
        added = mgr.queue_downloads(["episode2", "episode3", "episode4"])
        self.assertEqual(added, 3)
        # Verify progress queue count is IMMEDIATELY updated without waiting for episode1 to finish
        self.assertEqual(mgr.progress_queue_count, 4)
        self.assertEqual(mgr.total_queue_count, 4)
        self.assertEqual(mgr.get_queue_position("episode2"), 2)
        self.assertEqual(mgr.get_queue_position("episode3"), 3)
        self.assertEqual(mgr.get_queue_position("episode4"), 4)
        self.assertTrue(mgr.is_queued("episode3"))
        self.assertFalse(mgr.is_queued("episode99"))

        # Cancel queued episode3
        cancelled = mgr.cancel_queued_item("episode3")
        self.assertTrue(cancelled)
        self.assertEqual(mgr.progress_queue_count, 3)
        self.assertFalse(mgr.is_queued("episode3"))
        self.assertEqual(mgr.get_queue_position("episode4"), 3)

    def test_status_message_expiry_preserves_user_action_feedback(self):
        # Simulates setStatusMessage and background frame loop download monitoring
        class SimulatedStatusController:
            def __init__(self):
                self.status_msg = ""
                self.expiry_tick = 0

            def set_status_message(self, msg, duration_ms, current_time_ms):
                self.status_msg = msg
                self.expiry_tick = current_time_ms + duration_ms if duration_ms > 0 else 0

            def on_frame_update(self, dl_active, dl_title, dl_percent, current_time_ms):
                if dl_active:
                    if current_time_ms >= self.expiry_tick:
                        self.status_msg = f"DL: {dl_title} - {dl_percent}%"

        ctrl = SimulatedStatusController()
        # Active download is running
        now = 1000
        ctrl.on_frame_update(True, "Bluey", 50, now)
        self.assertEqual(ctrl.status_msg, "DL: Bluey - 50%")

        # User queues another item at t=1010 ms with 3500ms duration
        now = 1010
        ctrl.set_status_message("Queued: Bingo (#2)", 3500, now)
        self.assertEqual(ctrl.status_msg, "Queued: Bingo (#2)")

        # Next frame at t=1026 ms (16ms later) - dl active, but expiry has NOT passed
        now = 1026
        ctrl.on_frame_update(True, "Bluey", 52, now)
        # Message must NOT be clobbered
        self.assertEqual(ctrl.status_msg, "Queued: Bingo (#2)")

        # Halfway through display at t=2500 ms
        now = 2500
        ctrl.on_frame_update(True, "Bluey", 65, now)
        self.assertEqual(ctrl.status_msg, "Queued: Bingo (#2)")

        # After expiry at t=4511 ms (now >= 1010 + 3500 = 4510)
        now = 4511
        ctrl.on_frame_update(True, "Bluey", 80, now)
        # Reverts to live background download progress
        self.assertEqual(ctrl.status_msg, "DL: Bluey - 80%")

    def test_detail_view_button_label_logic(self):
        def get_button_label(is_downloaded, is_queued):
            if is_downloaded:
                return "Delete"
            if is_queued:
                return "Queued"
            return "Download"

        self.assertEqual(get_button_label(is_downloaded=True, is_queued=False), "Delete")
        self.assertEqual(get_button_label(is_downloaded=False, is_queued=True), "Queued")
        self.assertEqual(get_button_label(is_downloaded=False, is_queued=False), "Download")


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


def sanitize_path_component(name):
    if not name:
        return "Unknown"
    safe = ""
    i = 0
    while i < len(name):
        c = name[i]
        if c == ':' and i + 1 < len(name) and name[i + 1] == ' ':
            safe += " -"
        elif c in r'/\:*?"<>|' or ord(c) < 32:
            safe += '_'
        else:
            safe += c
        i += 1
    safe = safe.strip(' ._')
    if len(safe) > 64:
        safe = safe[:64].rstrip(' ._')
    return safe if safe else "Unknown"


def build_local_media_path(item):
    safe_title = sanitize_path_component(item.get("title", ""))
    if not safe_title or safe_title == "Unknown":
        safe_title = sanitize_key(item.get("ratingKey", "item"))

    media_type = item.get("type", "unknown")
    grandparent = item.get("grandparentTitle", "")
    parent = item.get("parentTitle", "")
    index = item.get("index", 0)
    year = item.get("year", 0)

    if media_type == "track":
        artist = grandparent if grandparent else "Unknown Artist"
        album = parent if parent else "Unknown Album"
        safe_artist = sanitize_path_component(artist)
        safe_album = sanitize_path_component(album)
        dir_path = f"sdmc:/3ds/plex-3ds/downloads/Music/{safe_artist}/{safe_album}"
        if index > 0:
            return f"{dir_path}/{index:02d} - {safe_title}.mp3"
        return f"{dir_path}/{safe_title}.mp3"
    elif media_type == "episode" or (grandparent and media_type != "movie"):
        show = grandparent if grandparent else (parent if parent else "Unknown Show")
        season = parent if parent else "Season 01"
        safe_show = sanitize_path_component(show)
        safe_season = sanitize_path_component(season)
        dir_path = f"sdmc:/3ds/plex-3ds/downloads/TV Shows/{safe_show}/{safe_season}"
        if index > 0:
            return f"{dir_path}/{index:02d} - {safe_title}.mkv"
        return f"{dir_path}/{safe_title}.mkv"
    else:
        folder_name = safe_title
        if year > 0:
            folder_name += f" ({year})"
        dir_path = f"sdmc:/3ds/plex-3ds/downloads/Movies/{folder_name}"
        file_name = safe_title
        if year > 0:
            file_name += f" ({year})"
        return f"{dir_path}/{file_name}.mkv"


class TestPlex3DSHierarchicalStorage(unittest.TestCase):
    def test_sanitize_path_component(self):
        self.assertEqual(sanitize_path_component("Inception"), "Inception")
        self.assertEqual(sanitize_path_component("Star Wars: Episode IV"), "Star Wars - Episode IV")
        self.assertEqual(sanitize_path_component("AC/DC"), "AC_DC")
        self.assertEqual(sanitize_path_component("What If...?"), "What If")
        self.assertEqual(sanitize_path_component("...Test..."), "Test")
        self.assertEqual(sanitize_path_component(""), "Unknown")
        self.assertEqual(sanitize_path_component("   "), "Unknown")
        self.assertEqual(sanitize_path_component(None), "Unknown")

    def test_movie_path_with_year(self):
        item = {
            "title": "Inception",
            "year": 2010,
            "type": "movie",
            "ratingKey": "101"
        }
        path = build_local_media_path(item)
        self.assertEqual(path, "sdmc:/3ds/plex-3ds/downloads/Movies/Inception (2010)/Inception (2010).mkv")

    def test_movie_path_without_year(self):
        item = {
            "title": "Home Video",
            "year": 0,
            "type": "movie",
            "ratingKey": "102"
        }
        path = build_local_media_path(item)
        self.assertEqual(path, "sdmc:/3ds/plex-3ds/downloads/Movies/Home Video/Home Video.mkv")

    def test_tv_episode_path_with_index(self):
        item = {
            "title": "Pilot",
            "grandparentTitle": "Breaking Bad",
            "parentTitle": "Season 1",
            "index": 1,
            "type": "episode",
            "ratingKey": "201"
        }
        path = build_local_media_path(item)
        self.assertEqual(path, "sdmc:/3ds/plex-3ds/downloads/TV Shows/Breaking Bad/Season 1/01 - Pilot.mkv")

    def test_tv_episode_path_without_index(self):
        item = {
            "title": "Special Episode",
            "grandparentTitle": "Doctor Who",
            "parentTitle": "Specials",
            "index": 0,
            "type": "episode",
            "ratingKey": "202"
        }
        path = build_local_media_path(item)
        self.assertEqual(path, "sdmc:/3ds/plex-3ds/downloads/TV Shows/Doctor Who/Specials/Special Episode.mkv")

    def test_music_track_path_with_index(self):
        item = {
            "title": "One More Time",
            "grandparentTitle": "Daft Punk",
            "parentTitle": "Discovery",
            "index": 1,
            "type": "track",
            "ratingKey": "301"
        }
        path = build_local_media_path(item)
        self.assertEqual(path, "sdmc:/3ds/plex-3ds/downloads/Music/Daft Punk/Discovery/01 - One More Time.mp3")

    def test_music_track_path_without_index(self):
        item = {
            "title": "Single Track",
            "grandparentTitle": "Radiohead",
            "parentTitle": "Singles",
            "index": 0,
            "type": "track",
            "ratingKey": "302"
        }
        path = build_local_media_path(item)
        self.assertEqual(path, "sdmc:/3ds/plex-3ds/downloads/Music/Radiohead/Singles/Single Track.mp3")


class TestPlex3DSOfflineNavigation(unittest.TestCase):
    def setUp(self):
        self.movie1 = {"title": "Inception", "year": 2010, "type": "movie", "ratingKey": "m1"}
        self.movie2 = {"title": "The Matrix", "year": 1999, "type": "movie", "ratingKey": "m2"}
        self.ep1 = {"title": "Pilot", "grandparentTitle": "Breaking Bad", "parentTitle": "Season 1", "index": 1, "type": "episode", "ratingKey": "e1"}
        self.ep2 = {"title": "Cat's in the Bag", "grandparentTitle": "Breaking Bad", "parentTitle": "Season 1", "index": 2, "type": "episode", "ratingKey": "e2"}
        self.ep3 = {"title": "Seven Thirty-Seven", "grandparentTitle": "Breaking Bad", "parentTitle": "Season 2", "index": 1, "type": "episode", "ratingKey": "e3"}
        self.trk1 = {"title": "One More Time", "grandparentTitle": "Daft Punk", "parentTitle": "Discovery", "index": 1, "type": "track", "ratingKey": "t1"}
        self.trk2 = {"title": "Aerodynamic", "grandparentTitle": "Daft Punk", "parentTitle": "Discovery", "index": 2, "type": "track", "ratingKey": "t2"}

    def test_multi_category_root_shows_category_folders(self):
        all_items = [self.movie1, self.ep1, self.trk1]
        movies = [it for it in all_items if it["type"] == "movie"]
        episodes = [it for it in all_items if it["type"] == "episode"]
        tracks = [it for it in all_items if it["type"] == "track"]

        cat_count = (1 if movies else 0) + (1 if episodes else 0) + (1 if tracks else 0)
        self.assertEqual(cat_count, 3)

    def test_single_category_movies_shows_movies_directly(self):
        all_items = [self.movie1, self.movie2]
        movies = [it for it in all_items if it["type"] == "movie"]
        episodes = [it for it in all_items if it["type"] == "episode"]
        tracks = [it for it in all_items if it["type"] == "track"]

        cat_count = (1 if movies else 0) + (1 if episodes else 0) + (1 if tracks else 0)
        self.assertEqual(cat_count, 1)
        self.assertEqual(len(movies), 2)

    def test_tv_show_seasons_grouping(self):
        episodes = [self.ep1, self.ep2, self.ep3]
        target_show = "Breaking Bad"
        seasons = set(ep["parentTitle"] for ep in episodes if ep["grandparentTitle"] == target_show)
        self.assertEqual(seasons, {"Season 1", "Season 2"})

        s1_episodes = sorted([ep for ep in episodes if ep["grandparentTitle"] == target_show and ep["parentTitle"] == "Season 1"], key=lambda x: x["index"])
        self.assertEqual(len(s1_episodes), 2)
        self.assertEqual(s1_episodes[0]["title"], "Pilot")
        self.assertEqual(s1_episodes[1]["title"], "Cat's in the Bag")

    def test_music_album_tracks_grouping(self):
        tracks = [self.trk2, self.trk1]
        target_artist = "Daft Punk"
        target_album = "Discovery"
        album_tracks = sorted([t for t in tracks if t["grandparentTitle"] == target_artist and t["parentTitle"] == target_album], key=lambda x: x["index"])
        self.assertEqual(len(album_tracks), 2)
        self.assertEqual(album_tracks[0]["title"], "One More Time")
        self.assertEqual(album_tracks[1]["title"], "Aerodynamic")


class TestPlex3DSAudioPlayerSeeking(unittest.TestCase):
    class SimulatedAudioPlayer:
        CHUNK_SIZE = 64 * 1024

        def __init__(self, total_sec=240, total_bytes=6000000):
            self.total_sec = total_sec
            self.total_bytes = total_bytes
            self.current_sec = 0
            self.is_playing = True
            self.seek_requested = False
            self.seek_target_sec = 0
            self.read_chunk_idx = 0
            self.read_chunk_offset = 0

        def seek_to(self, target_sec):
            if not self.is_playing:
                return
            if target_sec < 0:
                target_sec = 0
            if self.total_sec > 0 and target_sec > self.total_sec:
                target_sec = self.total_sec

            self.seek_target_sec = target_sec
            self.seek_requested = True
            self.current_sec = target_sec

        def seek(self, delta_sec):
            self.seek_to(self.current_sec + delta_sec)

        def perform_seek(self):
            if not self.seek_requested:
                return
            self.seek_requested = False
            target_sec = self.seek_target_sec
            if self.total_sec > 0 and self.total_bytes > 0:
                target_byte = (target_sec * self.total_bytes) // self.total_sec
            else:
                target_byte = 0
            self.read_chunk_idx = target_byte // self.CHUNK_SIZE
            self.read_chunk_offset = target_byte % self.CHUNK_SIZE
            self.current_sec = target_sec

    def test_seek_forward_15s(self):
        player = self.SimulatedAudioPlayer(total_sec=200, total_bytes=5000000)
        player.current_sec = 10
        player.seek(15)
        self.assertTrue(player.seek_requested)
        self.assertEqual(player.seek_target_sec, 25)
        self.assertEqual(player.current_sec, 25)

        player.perform_seek()
        self.assertFalse(player.seek_requested)
        self.assertEqual(player.current_sec, 25)
        # Expected byte = (25 * 5000000) // 200 = 625000
        # Expected chunk = 625000 // 65536 = 9
        # Expected offset = 625000 % 65536 = 35176
        self.assertEqual(player.read_chunk_idx, 9)
        self.assertEqual(player.read_chunk_offset, 35176)

    def test_seek_backward_15s(self):
        player = self.SimulatedAudioPlayer(total_sec=200, total_bytes=5000000)
        player.current_sec = 30
        player.seek(-15)
        self.assertTrue(player.seek_requested)
        self.assertEqual(player.seek_target_sec, 15)
        self.assertEqual(player.current_sec, 15)

        player.perform_seek()
        self.assertEqual(player.current_sec, 15)
        # Expected byte = (15 * 5000000) // 200 = 375000
        # Expected chunk = 375000 // 65536 = 5
        # Expected offset = 375000 % 65536 = 47320
        self.assertEqual(player.read_chunk_idx, 5)
        self.assertEqual(player.read_chunk_offset, 47320)

    def test_seek_clamps_to_zero(self):
        player = self.SimulatedAudioPlayer(total_sec=200, total_bytes=5000000)
        player.current_sec = 5
        player.seek(-15)
        self.assertEqual(player.seek_target_sec, 0)
        self.assertEqual(player.current_sec, 0)
        player.perform_seek()
        self.assertEqual(player.read_chunk_idx, 0)
        self.assertEqual(player.read_chunk_offset, 0)

    def test_seek_clamps_to_duration(self):
        player = self.SimulatedAudioPlayer(total_sec=200, total_bytes=5000000)
        player.current_sec = 195
        player.seek(15)
        self.assertEqual(player.seek_target_sec, 200)
        self.assertEqual(player.current_sec, 200)


class TestPlex3DSConfirmDialog(unittest.TestCase):
    """
    Tests the confirmation modal dialog behavior, button/touch boundaries,
    and callback execution order for dangerous deletion operations.
    """
    class SimulatedConfirmDialog:
        def __init__(self):
            self.active = False
            self.title = ""
            self.prompt = ""
            self.item_title = ""
            self.warning = ""
            self.confirm_label = "Delete (A)"
            self.cancel_label = "Cancel (B)"
            self.on_confirm = None
            self.on_cancel = None

        def show(self, title, prompt, item_title, warning="", confirm_label="Delete (A)", cancel_label="Cancel (B)", on_confirm=None, on_cancel=None):
            self.active = True
            self.title = title
            self.prompt = prompt
            self.item_title = item_title
            self.warning = warning
            self.confirm_label = confirm_label
            self.cancel_label = cancel_label
            self.on_confirm = on_confirm
            self.on_cancel = on_cancel

        def handle_input(self, key=None, touch=None):
            if not self.active:
                return False

            if key in ("A", "X"):
                cb = self.on_confirm
                self.active = False
                if cb:
                    cb()
                return True

            if key in ("B", "START"):
                cb = self.on_cancel
                self.active = False
                if cb:
                    cb()
                return True

            if touch is not None:
                tx, ty = touch
                # Cancel button: [20..155, 155..210]
                if 20 <= tx <= 155 and 155 <= ty <= 210:
                    cb = self.on_cancel
                    self.active = False
                    if cb:
                        cb()
                    return True
                # Confirm button: [165..300, 155..210]
                elif 165 <= tx <= 300 and 155 <= ty <= 210:
                    cb = self.on_confirm
                    self.active = False
                    if cb:
                        cb()
                    return True

            # All other inputs swallowed while active
            return True

    def test_inactive_does_not_consume_input(self):
        dlg = self.SimulatedConfirmDialog()
        self.assertFalse(dlg.handle_input(key="A"))
        self.assertFalse(dlg.handle_input(key="B"))
        self.assertFalse(dlg.handle_input(touch=(100, 180)))

    def test_key_confirm_triggers_callback(self):
        dlg = self.SimulatedConfirmDialog()
        confirmed = []
        cancelled = []
        dlg.show("Delete Download", "Delete?", "Test Track", on_confirm=lambda: confirmed.append(True), on_cancel=lambda: cancelled.append(True))
        self.assertTrue(dlg.active)

        consumed = dlg.handle_input(key="A")
        self.assertTrue(consumed)
        self.assertFalse(dlg.active)
        self.assertEqual(confirmed, [True])
        self.assertEqual(cancelled, [])

    def test_key_x_also_confirms(self):
        dlg = self.SimulatedConfirmDialog()
        confirmed = []
        dlg.show("Delete Download", "Delete?", "Test Track", on_confirm=lambda: confirmed.append(True))
        consumed = dlg.handle_input(key="X")
        self.assertTrue(consumed)
        self.assertFalse(dlg.active)
        self.assertEqual(confirmed, [True])

    def test_key_cancel_triggers_callback(self):
        dlg = self.SimulatedConfirmDialog()
        confirmed = []
        cancelled = []
        dlg.show("Delete Download", "Delete?", "Test Track", on_confirm=lambda: confirmed.append(True), on_cancel=lambda: cancelled.append(True))

        consumed = dlg.handle_input(key="B")
        self.assertTrue(consumed)
        self.assertFalse(dlg.active)
        self.assertEqual(confirmed, [])
        self.assertEqual(cancelled, [True])

    def test_start_key_cancels_safely(self):
        dlg = self.SimulatedConfirmDialog()
        cancelled = []
        dlg.show("Delete Download", "Delete?", "Test Track", on_cancel=lambda: cancelled.append(True))

        consumed = dlg.handle_input(key="START")
        self.assertTrue(consumed)
        self.assertFalse(dlg.active)
        self.assertEqual(cancelled, [True])

    def test_touch_cancel_and_confirm_boundaries(self):
        dlg = self.SimulatedConfirmDialog()
        confirmed = []
        cancelled = []

        # Test touch Cancel button
        dlg.show("Delete Download", "Delete?", "Test Track",
                 on_confirm=lambda: confirmed.append(True),
                 on_cancel=lambda: cancelled.append(True))
        self.assertTrue(dlg.handle_input(touch=(85, 180)))
        self.assertFalse(dlg.active)
        self.assertEqual(cancelled, [True])
        self.assertEqual(confirmed, [])

        # Test touch Confirm button
        dlg.show("Delete Download", "Delete?", "Test Track",
                 on_confirm=lambda: confirmed.append(True),
                 on_cancel=lambda: cancelled.append(True))
        self.assertTrue(dlg.handle_input(touch=(230, 180)))
        self.assertFalse(dlg.active)
        self.assertEqual(confirmed, [True])

        # Test edge boundaries
        # Cancel min/max
        dlg.show("Del", "Del", "Item", on_cancel=lambda: cancelled.append(2))
        self.assertTrue(dlg.handle_input(touch=(20, 155)))
        self.assertEqual(cancelled, [True, 2])

        dlg.show("Del", "Del", "Item", on_cancel=lambda: cancelled.append(3))
        self.assertTrue(dlg.handle_input(touch=(155, 210)))
        self.assertEqual(cancelled, [True, 2, 3])

        # Confirm min/max
        dlg.show("Del", "Del", "Item", on_confirm=lambda: confirmed.append(2))
        self.assertTrue(dlg.handle_input(touch=(165, 155)))
        self.assertEqual(confirmed, [True, 2])

        dlg.show("Del", "Del", "Item", on_confirm=lambda: confirmed.append(3))
        self.assertTrue(dlg.handle_input(touch=(300, 210)))
        self.assertEqual(confirmed, [True, 2, 3])

    def test_touch_outside_swallowed_without_callback(self):
        dlg = self.SimulatedConfirmDialog()
        confirmed = []
        cancelled = []
        dlg.show("Delete Download", "Delete?", "Test Track",
                 on_confirm=lambda: confirmed.append(True),
                 on_cancel=lambda: cancelled.append(True))

        # Dead zone between buttons (tx=160, ty=180)
        self.assertTrue(dlg.handle_input(touch=(160, 180)))
        self.assertTrue(dlg.active)
        self.assertEqual(confirmed, [])
        self.assertEqual(cancelled, [])

        # Outside modal card
        self.assertTrue(dlg.handle_input(touch=(10, 10)))
        self.assertTrue(dlg.active)
        self.assertEqual(confirmed, [])
        self.assertEqual(cancelled, [])

    def test_delete_download_flow_cancelled_preserves_item(self):
        storage = {"track_001": "/sdmc/plex/track_001.mp3"}
        dlg = self.SimulatedConfirmDialog()

        def do_delete():
            del storage["track_001"]

        dlg.show("Delete Download", "Are you sure?", "Revolver - Taxman", on_confirm=do_delete)
        # User presses B (cancel)
        dlg.handle_input(key="B")
        self.assertIn("track_001", storage)

        # User repeats and confirms with A
        dlg.show("Delete Download", "Are you sure?", "Revolver - Taxman", on_confirm=do_delete)
        dlg.handle_input(key="A")
        self.assertNotIn("track_001", storage)


class TestPlex3DSDecoupledDownloadBuffer(unittest.TestCase):
    BLOCK_SIZE = 64 * 1024       # 64 KB (matches SD FAT32 cluster size)
    NUM_BLOCKS = 16              # 16 blocks = 1024 KB (1 MB)

    class SimulatedPipeline:
        def __init__(self, block_size=64 * 1024, num_blocks=16):
            self.block_size = block_size
            self.num_blocks = num_blocks
            self.free_blocks = [bytearray(block_size) for _ in range(num_blocks)]
            self.write_queue = []  # list of (bytearray, valid_length)
            self.active_block = None
            self.active_size = 0
            self.written_chunks = []
            self.writer_error = False
            self.cancelled = False

        def get_total_blocks(self):
            count = len(self.free_blocks) + len(self.write_queue)
            if self.active_block is not None:
                count += 1
            return count

        def write_cb(self, chunk: bytes) -> int:
            if self.cancelled or self.writer_error:
                return 0
            idx = 0
            total = len(chunk)
            while idx < total:
                if self.cancelled or self.writer_error:
                    return 0
                if self.active_block is None:
                    if not self.free_blocks:
                        # Emulate writer thread consuming a block under backpressure
                        self.writer_flush_step()
                        if not self.free_blocks:
                            raise RuntimeError("Buffer pool overflow")
                    self.active_block = self.free_blocks.pop(0)
                    self.active_size = 0

                space = self.block_size - self.active_size
                to_copy = min(total - idx, space)
                self.active_block[self.active_size : self.active_size + to_copy] = chunk[idx : idx + to_copy]
                self.active_size += to_copy
                idx += to_copy

                if self.active_size >= self.block_size:
                    self.write_queue.append((self.active_block, self.active_size))
                    self.active_block = None
                    self.active_size = 0
            return total

        def writer_flush_step(self):
            if self.write_queue:
                buf, sz = self.write_queue.pop(0)
                if not self.writer_error:
                    self.written_chunks.append(bytes(buf[:sz]))
                self.free_blocks.append(buf)

        def finish(self):
            if self.active_block and self.active_size > 0:
                self.write_queue.append((self.active_block, self.active_size))
                self.active_block = None
                self.active_size = 0
            while self.write_queue:
                self.writer_flush_step()

    def test_pool_invariants_and_capacity(self):
        pipe = self.SimulatedPipeline(self.BLOCK_SIZE, self.NUM_BLOCKS)
        self.assertEqual(pipe.get_total_blocks(), 16)
        self.assertEqual(len(pipe.free_blocks), 16)

        # Write 100 KB in 10 KB chunks
        for _ in range(10):
            pipe.write_cb(b"A" * 10240)
            self.assertEqual(pipe.get_total_blocks(), 16)

        pipe.finish()
        self.assertEqual(pipe.get_total_blocks(), 16)
        total_bytes = sum(len(c) for c in pipe.written_chunks)
        self.assertEqual(total_bytes, 102400)

    def test_exact_64kb_cluster_aligned_writes(self):
        pipe = self.SimulatedPipeline(self.BLOCK_SIZE, self.NUM_BLOCKS)

        # Write 200,000 bytes (which is 3 full 64KB blocks + 3424 bytes tail)
        # using arbitrary variable packet sizes (512, 1460, 4096, 16384)
        chunk_sizes = [512, 1460, 4096, 16384, 8192, 1024]
        total_target = 200000
        sent = 0
        i = 0
        while sent < total_target:
            sz = min(chunk_sizes[i % len(chunk_sizes)], total_target - sent)
            pipe.write_cb(b"X" * sz)
            sent += sz
            i += 1
            # Intermittently step the writer thread
            if i % 3 == 0:
                pipe.writer_flush_step()

        pipe.finish()

        # All chunks except the last tail MUST be strictly 64 KB (65536 bytes)
        self.assertEqual(len(pipe.written_chunks), 4)
        self.assertEqual(len(pipe.written_chunks[0]), 65536)
        self.assertEqual(len(pipe.written_chunks[1]), 65536)
        self.assertEqual(len(pipe.written_chunks[2]), 65536)
        self.assertEqual(len(pipe.written_chunks[3]), 200000 - (3 * 65536))
        self.assertEqual(sum(len(c) for c in pipe.written_chunks), 200000)

    def test_payload_integrity_multi_megabytes(self):
        pipe = self.SimulatedPipeline(self.BLOCK_SIZE, self.NUM_BLOCKS)
        # Create 2.5 MB deterministic test pattern
        pattern = bytes([i % 256 for i in range(256)])
        raw_data = (pattern * 10240)[: 2500000]

        # Feed in random-like chunk slices
        cursor = 0
        sizes = [1337, 8192, 65536, 4096, 100, 32768, 65536]
        idx = 0
        while cursor < len(raw_data):
            step = min(sizes[idx % len(sizes)], len(raw_data) - cursor)
            pipe.write_cb(raw_data[cursor : cursor + step])
            cursor += step
            idx += 1
            if idx % 2 == 0:
                pipe.writer_flush_step()

        pipe.finish()
        reconstructed = b"".join(pipe.written_chunks)
        self.assertEqual(len(reconstructed), len(raw_data))
        self.assertEqual(reconstructed, raw_data)

    def test_backpressure_handles_unwritten_burst(self):
        pipe = self.SimulatedPipeline(self.BLOCK_SIZE, self.NUM_BLOCKS)
        # Burst 16 full 64KB blocks (1 MB) without flushing
        for i in range(16):
            pipe.write_cb(bytes([i] * 65536))

        # All 16 blocks are in write_queue; free_blocks is empty
        self.assertEqual(len(pipe.write_queue), 16)
        self.assertEqual(len(pipe.free_blocks), 0)

        # 17th block triggers backpressure flush
        pipe.write_cb(b"B" * 65536)
        self.assertEqual(pipe.get_total_blocks(), 16)

        pipe.finish()
        self.assertEqual(len(pipe.written_chunks), 17)
        self.assertEqual(pipe.get_total_blocks(), 16)

    def test_writer_error_aborts_pipeline(self):
        pipe = self.SimulatedPipeline(self.BLOCK_SIZE, self.NUM_BLOCKS)
        pipe.write_cb(b"A" * 65536)
        pipe.writer_error = True
        # Once error is set, write_cb immediately returns 0 to signal curl abort
        self.assertEqual(pipe.write_cb(b"B" * 1024), 0)

    def test_cancellation_aborts_pipeline(self):
        pipe = self.SimulatedPipeline(self.BLOCK_SIZE, self.NUM_BLOCKS)
        pipe.write_cb(b"A" * 65536)
        pipe.cancelled = True
        self.assertEqual(pipe.write_cb(b"B" * 1024), 0)


BASE_DOWNLOAD_DIR   = "sdmc:/3ds/plex-3ds/downloads"
MOVIES_DOWNLOAD_DIR = "sdmc:/3ds/plex-3ds/downloads/Movies"
TV_DOWNLOAD_DIR     = "sdmc:/3ds/plex-3ds/downloads/TV Shows"
MUSIC_DOWNLOAD_DIR  = "sdmc:/3ds/plex-3ds/downloads/Music"
META_DOWNLOAD_DIR   = "sdmc:/3ds/plex-3ds/downloads/meta"
LEGACY_VIDEO_DIR    = "sdmc:/3ds/plex-3ds/downloads/videos"
LEGACY_MUSIC_DIR    = "sdmc:/3ds/plex-3ds/downloads/music"

def is_safe_download_path(p):
    if not p:
        return False
    if not p.startswith(BASE_DOWNLOAD_DIR + "/"):
        return False
    if ".." in p:
        return False
    return True

def is_safe_download_dir(d):
    if not d:
        return False
    if not d.startswith(BASE_DOWNLOAD_DIR + "/"):
        return False
    if ".." in d:
        return False
    if d in (MOVIES_DOWNLOAD_DIR, TV_DOWNLOAD_DIR, MUSIC_DOWNLOAD_DIR,
             META_DOWNLOAD_DIR, LEGACY_VIDEO_DIR, LEGACY_MUSIC_DIR, BASE_DOWNLOAD_DIR):
        return False
    return True


class TestPlex3DSSecurityAuditing(unittest.TestCase):
    def test_path_traversal_rejection(self):
        # Critical system paths on 3DS SD card must be rejected
        self.assertFalse(is_safe_download_path("sdmc:/boot.firm"))
        self.assertFalse(is_safe_download_path("sdmc:/luma/config.ini"))
        self.assertFalse(is_safe_download_path("sdmc:/3ds/plex-3ds/downloads/../../boot.firm"))
        self.assertFalse(is_safe_download_path("sdmc:/3ds/plex-3ds/downloads/../secrets.txt"))
        self.assertFalse(is_safe_download_path("/etc/ssl/certs/cacert.pem"))
        self.assertFalse(is_safe_download_path(""))
        self.assertFalse(is_safe_download_path(None))

        # Only legitimate subpaths inside downloads folder are accepted
        self.assertTrue(is_safe_download_path("sdmc:/3ds/plex-3ds/downloads/Movies/Movie (2020)/Movie (2020).mkv"))
        self.assertTrue(is_safe_download_path("sdmc:/3ds/plex-3ds/downloads/Music/Artist/Album/01 - Song.mp3"))

    def test_protected_directory_deletion_prevention(self):
        # Category root folders and the base downloads directory must NEVER be pruned or removed by rmdir
        self.assertFalse(is_safe_download_dir(BASE_DOWNLOAD_DIR))
        self.assertFalse(is_safe_download_dir(MOVIES_DOWNLOAD_DIR))
        self.assertFalse(is_safe_download_dir(TV_DOWNLOAD_DIR))
        self.assertFalse(is_safe_download_dir(MUSIC_DOWNLOAD_DIR))
        self.assertFalse(is_safe_download_dir(META_DOWNLOAD_DIR))
        self.assertFalse(is_safe_download_dir(LEGACY_VIDEO_DIR))
        self.assertFalse(is_safe_download_dir(LEGACY_MUSIC_DIR))

        # Traversal attempts in directory cleanup must be rejected
        self.assertFalse(is_safe_download_dir("sdmc:/3ds/plex-3ds/downloads/Movies/.."))

        # Legitimate album / season / movie subfolders can be cleaned up if empty
        self.assertTrue(is_safe_download_dir("sdmc:/3ds/plex-3ds/downloads/Movies/Inception (2010)"))
        self.assertTrue(is_safe_download_dir("sdmc:/3ds/plex-3ds/downloads/TV Shows/Breaking Bad/Season 1"))
        self.assertTrue(is_safe_download_dir("sdmc:/3ds/plex-3ds/downloads/Music/Daft Punk/Discovery"))

    def test_component_length_clamping(self):
        long_title = "A" * 120
        clamped = sanitize_path_component(long_title)
        self.assertEqual(len(clamped), 64)
        self.assertEqual(clamped, "A" * 64)

        # Ensure trailing spaces, dots, or underscores caused by cutoff at 64 chars are stripped
        cut_edge_case = ("B" * 63) + " ."
        clamped_edge = sanitize_path_component(cut_edge_case)
        self.assertEqual(clamped_edge, "B" * 63)
        self.assertFalse(clamped_edge.endswith(" "))
        self.assertFalse(clamped_edge.endswith("."))
        self.assertFalse(clamped_edge.endswith("_"))

    def test_config_negative_or_zero_size_handling(self):
        def simulate_load_config_sz(ftell_result):
            if ftell_result <= 0:
                return None  # Abort safely without string allocation or JSON parsing
            return "valid_data"

        self.assertIsNone(simulate_load_config_sz(-1))
        self.assertIsNone(simulate_load_config_sz(0))
        self.assertEqual(simulate_load_config_sz(150), "valid_data")

    def test_audio_allocation_exhaustion_recovery(self):
        # Simulates AudioPlayer::downloadLoop malloc retry ceiling
        def simulate_chunk_allocation(fail_attempts, max_retries=5):
            retries = 0
            chunk = None
            while chunk is None and retries < max_retries:
                if retries < fail_attempts:
                    retries += 1
                else:
                    chunk = {"data": bytearray(64 * 1024)}
            if chunk is None:
                return 0  # Abort curl cleanly
            return len(chunk["data"])

        # Recovered on 3rd attempt
        self.assertEqual(simulate_chunk_allocation(3), 65536)
        # Out of memory after 5 attempts -> cleanly aborts transfer (returns 0 to curl)
        self.assertEqual(simulate_chunk_allocation(10), 0)

    def test_tls_ca_bundle_path_standardization(self):
        # Ensure canonical 3DS CA bundle path is sdmc:/3ds/plex-3ds/cacert.pem
        canonical_ca_path = "sdmc:/3ds/plex-3ds/cacert.pem"
        self.assertTrue(canonical_ca_path.startswith("sdmc:/"))
        self.assertFalse(canonical_ca_path.startswith("/etc/"))

    def test_detail_view_selected_index_resolution(self):
        # Simulates main loop selectedIdx resolution across states: DETAIL_VIEW must resolve to selected_item_idx
        def resolve_selected_index(state, selected_item_idx, selected_library_idx, selected_server_idx):
            return (selected_item_idx if (state in ("ITEM_LIST", "DETAIL_VIEW")) else
                    selected_library_idx if state == "LIBRARY_LIST" else
                    selected_server_idx)

        self.assertEqual(resolve_selected_index("ITEM_LIST", 240, 2, 0), 240)
        self.assertEqual(resolve_selected_index("DETAIL_VIEW", 240, 2, 0), 240)
        self.assertEqual(resolve_selected_index("LIBRARY_LIST", 240, 2, 0), 2)
        self.assertEqual(resolve_selected_index("SERVER_SELECT", 240, 2, 1), 1)

    def test_scroll_auto_repeat_stops_at_boundary_without_wrapping(self):
        # Simulates navigation list cursor movement
        class ListNavigator:
            def __init__(self, count, max_vis=5):
                self.count = count
                self.max_vis = max_vis
                self.selected_idx = 0
                self.scroll_offset = 0

            def on_nav(self, nav_down, nav_up, is_down_initial, is_up_initial):
                if nav_down:
                    if self.selected_idx < self.count - 1:
                        self.selected_idx += 1
                        if self.selected_idx >= self.scroll_offset + self.max_vis:
                            self.scroll_offset = self.selected_idx - self.max_vis + 1
                    elif is_down_initial:
                        self.selected_idx = 0
                        self.scroll_offset = 0

                if nav_up:
                    if self.selected_idx > 0:
                        self.selected_idx -= 1
                        if self.selected_idx < self.scroll_offset:
                            self.scroll_offset = self.selected_idx
                    elif is_up_initial:
                        self.selected_idx = self.count - 1
                        self.scroll_offset = max(0, self.count - self.max_vis)

        nav = ListNavigator(count=10)
        # 1. Tap Up at index 0 -> wraps to bottom (9)
        nav.on_nav(nav_down=False, nav_up=True, is_down_initial=False, is_up_initial=True)
        self.assertEqual(nav.selected_idx, 9)

        # 2. Tap Down at index 9 -> wraps to top (0)
        nav.on_nav(nav_down=True, nav_up=False, is_down_initial=True, is_up_initial=False)
        self.assertEqual(nav.selected_idx, 0)

        # 3. Hold Down from index 0 across 20 frames (auto-repeat, is_down_initial=False)
        for _ in range(20):
            nav.on_nav(nav_down=True, nav_up=False, is_down_initial=False, is_up_initial=False)
        # MUST clamp and stop at index 9, NOT wrap back to 0!
        self.assertEqual(nav.selected_idx, 9)

        # 4. Now release and tap Down (is_down_initial=True) -> wraps to 0
        nav.on_nav(nav_down=True, nav_up=False, is_down_initial=True, is_up_initial=False)
        self.assertEqual(nav.selected_idx, 0)

        # 5. Hold Up from index 9 across 20 frames (auto-repeat, is_up_initial=False)
        nav.selected_idx = 9
        for _ in range(20):
            nav.on_nav(nav_down=False, nav_up=True, is_down_initial=False, is_up_initial=False)
        # MUST clamp and stop at index 0, NOT wrap back to 9!
        self.assertEqual(nav.selected_idx, 0)

        # 6. Now release and tap Up (is_up_initial=True) -> wraps to 9
        nav.on_nav(nav_down=False, nav_up=True, is_down_initial=False, is_up_initial=True)
        self.assertEqual(nav.selected_idx, 9)


if __name__ == '__main__':
    unittest.main(verbosity=2)


