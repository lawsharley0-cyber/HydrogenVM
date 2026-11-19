import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
from autoplayer import Frame
from autoplayer_vision import Vision, claude_tap, current_view


def frame(color):
    return Frame(1, 1, 1, bytes(color) * (96 * 64), bytes(color) + b'\xff')


class VisionTests(unittest.TestCase):
    def test_only_stalled_new_scenes_spend_budget(self):
        answer = Mock(return_value=(0.2, 0.4))
        policy = Vision('test', 'play', 2, answer)
        for color, expected in [(0, 1), (1, 1), (120, 2), (0, 2), (255, 2)]:
            screens = [frame((color, color, color)) for _ in range(5)]
            for screen in screens:
                policy.consider(screen)
            self.assertEqual(answer.call_count, expected)

    def test_invalid_replies_spend_slot_and_fall_back(self):
        for reply in [(float('nan'), 0), (-1, 0), ('1', 0), (1, 0), (True, 0), (10**400, 0), ()]:
            policy = Vision('test', 'play', 1, lambda *args: reply)
            for _ in range(4):
                point, reason = policy.consider(frame((10, 10, 10)))
            self.assertIsNone(point)
            self.assertIn('vision', reason)
            self.assertEqual(len(policy.seen), 1)

    def test_reject_stale_points_on_scene_or_scale_change(self):
        old = frame((1, 2, 3))
        self.assertTrue(current_view(old, frame((2, 3, 4))))
        self.assertFalse(current_view(old, frame((40, 30, 20))))
        rotated = Frame(2, 1, 1, old.thumb)
        self.assertFalse(current_view(old, rotated))

    def test_messages_api_request_and_reply(self):
        try:
            import PIL.Image
        except ImportError:
            self.skipTest('vision path requires Pillow')
        with patch.dict(os.environ, {'ANTHROPIC_API_KEY': 'example'}), \
                patch('autoplayer_vision.http.client.HTTPSConnection') as connection:
            client = connection.return_value
            response = client.getresponse.return_value
            response.status = 200
            response.read.return_value = json.dumps({'content': [
                {'type': 'text', 'text': '{"x":0.25,"y":0.5}'}]}).encode()
            self.assertEqual(claude_tap(frame((1, 2, 3)), 'model', 'advance'), (.25, .5))
            connection.assert_called_once_with('api.anthropic.com', timeout=8)
            args, kwargs = client.request.call_args
            self.assertEqual(args, ('POST', '/v1/messages'))
            data = json.loads(kwargs['body'])
            self.assertEqual(data['messages'][0]['content'][0]['source']['media_type'], 'image/jpeg')
            self.assertEqual(data['messages'][0]['content'][1]['text'], 'advance')
            response.read.return_value = b'x' * 32769
            with self.assertRaises(ValueError):
                claude_tap(frame((1, 2, 3)), 'model', 'advance')
            self.assertEqual(client.close.call_count, 2)
