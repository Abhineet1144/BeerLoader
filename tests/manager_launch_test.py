"""Manager launch regression tests; run with /usr/bin/python3 tests/manager_launch_test.py."""
import copy
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "manager"))
import beer_manager as manager


class ManagerLaunchTest(unittest.TestCase):
    def launch_environment(self, values=None, inherited=None, extra_env=""):
        game = {"name": "Local listen server", "exe": "/games/Game With Spaces/game.exe",
                "steam_mode": "official", "env": extra_env}
        if values is not None:
            game["values"] = copy.deepcopy(values)
        with tempfile.TemporaryDirectory() as logs:
            app = SimpleNamespace(lib=SimpleNamespace(settings={"loader": "/beer/loader"}),
                                  children={}, stack=Mock(), toast=Mock(), prune_logs=Mock())
            env = {"HOME": "/home/test user", **(inherited or {})}
            with patch.dict(os.environ, env, clear=True), \
                    patch.object(manager, "LOG_DIR", logs), \
                    patch.object(manager.os.path, "exists", return_value=True), \
                    patch.object(manager.storage, "migrate_cache"), \
                    patch.object(manager, "load_overlay_config", return_value={}), \
                    patch.object(manager, "flat_message", return_value={}), \
                    patch.object(manager, "load_display_config", return_value={}), \
                    patch.object(manager, "post_message", return_value={}), \
                    patch.object(manager.subprocess, "Popen", return_value=SimpleNamespace(pid=123)) as spawn:
                manager.Manager.launch(app, game)
            spawn.assert_called_once()
            self.assertEqual(spawn.call_args.args[0][-1], game["exe"])
            self.assertEqual(spawn.call_args.kwargs["env"]["BEER_STEAM_API_MODE"], "official")
            self.assertIn(123, app.children)
            return spawn.call_args.kwargs["env"]

    def test_saved_networking_path_is_expanded_and_passed(self):
        env = self.launch_environment({"steam_networking_library": "  ~/.local/lib/libGameNetworkingSockets.so  "})
        self.assertEqual(env["BEER_STEAM_NETWORKING_LIBRARY"],
                         "/home/test user/.local/lib/libGameNetworkingSockets.so")

    def test_networking_remains_opt_in(self):
        for values in (None, {}, {"steam_networking_library": "   "}):
            with self.subTest(values=values):
                self.assertNotIn("BEER_STEAM_NETWORKING_LIBRARY", self.launch_environment(values))

    def test_inherited_or_advanced_environment_still_works(self):
        key = "BEER_STEAM_NETWORKING_LIBRARY"
        self.assertEqual(self.launch_environment(inherited={key: "/native/inherited.so"})[key],
                         "/native/inherited.so")
        self.assertEqual(self.launch_environment(extra_env=key + "='/native/custom path.so'")[key],
                         "/native/custom path.so")

    def test_saved_value_overrides_inherited_environment(self):
        env = self.launch_environment({"steam_networking_library": "/native/saved.so"},
                                      {"BEER_STEAM_NETWORKING_LIBRARY": "/stale/build.so"})
        self.assertEqual(env["BEER_STEAM_NETWORKING_LIBRARY"], "/native/saved.so")

    def test_setting_has_file_picker(self):
        self.assertEqual(manager.PATH_VALUES["steam_networking_library"], (False, None))
        self.assertIn("BEER_STEAM_NETWORKING_LIBRARY",
                      [entry[2] for entry in manager.LAUNCH_VALUES if entry[0] == "steam_networking_library"])


if __name__ == "__main__":
    unittest.main()