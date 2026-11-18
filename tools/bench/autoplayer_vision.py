"""Budgeted Claude vision fallback used only when the bench operator opts in."""
import base64
import http.client
import io
import json
import math
import os
import queue
import threading


def scene(thumb):
    # Average small tiles so a cursor or a flashing label does not count as
    # a whole new screen. Preserve RGB to distinguish equal-luminance menus.
    signature = []
    for y in range(0, 64, 8):
        for x in range(0, 96, 8):
            for channel in range(3):
                signature.append(sum(thumb[(row * 96 + col) * 3 + channel]
                                     for row in range(y, y + 8, 2)
                                     for col in range(x, x + 8, 2)) // 16)
    return bytes(signature)


def same_scene(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) <= 12 * len(a)


class Vision:
    def __init__(self, model, goal, budget, responder=None):
        self.model = model
        self.goal = goal
        self.budget = budget
        self.respond = responder or claude_tap
        self.seen = []
        self.previous = None
        self.repeats = 0

    def consider(self, frame):
        current = scene(frame.thumb)
        if self.previous is not None and same_scene(current, self.previous):
            self.repeats += 1
        else:
            self.previous = current
            self.repeats = 0
        if self.repeats < 3 or len(self.seen) >= self.budget:
            return None, None
        if any(same_scene(current, past) for past in self.seen):
            return None, None
        # Failed calls also spend their slot, including ambiguous network errors.
        self.seen.append(current)
        try:
            x, y = self.respond(frame, self.model, self.goal)
        except (OSError, ValueError, KeyError, TypeError, http.client.HTTPException) as err:
            return None, f'vision unavailable ({type(err).__name__})'
        if not all(type(v) in (int, float) and math.isfinite(v) and 0 <= v < 1 for v in (x, y)):
            return None, 'vision returned an invalid point'
        return [(x, y)], 'vision'


def current_view(frame, latest):
    return ((frame.width, frame.height, frame.scale) == (latest.width, latest.height, latest.scale)
            and same_scene(scene(frame.thumb), scene(latest.thumb)))


def claude_tap(frame, model, goal):
    # A socket timeout only limits each read. A slowly arriving response must
    # also release the input loop by a wall-clock deadline.
    result = queue.Queue(maxsize=1)

    def request():
        try:
            result.put((True, request_tap(frame, model, goal)))
        except (OSError, ValueError, KeyError, TypeError, http.client.HTTPException) as err:
            result.put((False, err))

    threading.Thread(target=request, daemon=True).start()
    try:
        ok, value = result.get(timeout=9)
    except queue.Empty as err:
        raise TimeoutError('vision deadline expired') from err
    if not ok:
        raise value
    return value


def request_tap(frame, model, goal):
    from PIL import Image
    image = Image.frombytes('RGBA', (frame.width, frame.height), frame.rgba).convert('RGB')
    image.thumbnail((768, 768))
    data = io.BytesIO()
    image.save(data, format='JPEG', quality=80)
    request = {
        'model': model, 'max_tokens': 80,
        'system': 'Choose a single tap to advance this game toward the stated goal. '
                  'Use coordinates normalized to [0,1), origin at the top left. '
                  'Return only a JSON object with numeric keys x and y.',
        'messages': [{'role': 'user', 'content': [
            {'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/jpeg',
             'data': base64.b64encode(data.getvalue()).decode('ascii')}},
            {'type': 'text', 'text': goal}]}]}
    conn = http.client.HTTPSConnection('api.anthropic.com', timeout=8)
    try:
        conn.request('POST', '/v1/messages', body=json.dumps(request).encode(), headers={
            'Content-Type': 'application/json', 'anthropic-version': '2023-06-01',
            'x-api-key': os.environ['ANTHROPIC_API_KEY']})
        response = conn.getresponse()
        if response.status != 200:
            raise ValueError(f'vision HTTP {response.status}')
        raw = response.read(32_769)
        if len(raw) > 32_768:
            raise ValueError('oversize vision response')
        blocks = json.loads(raw)['content']
        if len(blocks) != 1 or blocks[0]['type'] != 'text':
            raise ValueError('expected one text response')
        point = json.loads(blocks[0]['text'])
        return point['x'], point['y']
    finally:
        conn.close()
