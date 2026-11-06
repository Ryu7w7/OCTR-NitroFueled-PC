from paths import ROOT
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('cc'), 'requires Linux and a C compiler')
class FriendsResponseTests(unittest.TestCase):
    def test_every_friend_reaches_the_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'friends-responses-test'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-format-truncation', '-Wno-stringop-truncation',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I' + str(ROOT), '-I' + str(ROOT / 'src'),
                            str(ROOT / 'tests/test_friends_responses.c'), '-pthread',
                            '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
        responses = iter(map(json.loads, result.stdout.splitlines()))
        for count in (0, 1, 32, 200):
            expected = [f'Friend{i:010d}' for i in range(count)]
            friends, profiles, ids = next(responses), next(responses), next(responses)
            self.assertEqual([f['onlineId'] for f in friends['friendList']], expected)
            self.assertEqual(friends['totalResults'], count)
            self.assertEqual(friends['size'], count)
            self.assertEqual([p['onlineId'] for p in profiles['profiles']], ['LocalPlayer'] + expected)
            self.assertEqual(profiles['totalResults'], count + 1)
            self.assertEqual(ids['friendOnlineIdLists'][0]['friendOnlineIdList'], expected)
