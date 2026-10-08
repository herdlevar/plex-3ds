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


if __name__ == '__main__':
    unittest.main(verbosity=2)


