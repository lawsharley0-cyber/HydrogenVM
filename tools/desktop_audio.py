"""Bounded socket -> SDL audio bridge. No guest assets are read by the viewer."""
import json
import os
import socket
import threading
import time
import wave
from collections import deque

import pygame


class DesktopAudio:
    def __init__(self, muted=False, capture=None):
        self.reader = self.writer = None
        self.thread = None
        self.stop_event = threading.Event()
        self.capture = capture
        self.stats = {'enabled': False, 'blocks': 0, 'underruns': 0, 'error': None}
        if muted:
            return
        try:
            # WSLg exposes PulseAudio, but SDL otherwise tries ALSA/dsp first.
            if os.environ.get('PULSE_SERVER'):
                os.environ.setdefault('SDL_AUDIODRIVER', 'pulseaudio')
            pygame.mixer.init(frequency=44100, size=-16, channels=2, buffer=2048, allowedchanges=0)
            self.channel = pygame.mixer.Channel(0)
            self.reader, self.writer = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
            self.writer.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
            self.reader.settimeout(0.1)
            self.stats.update(enabled=True, format=list(pygame.mixer.get_init()),
                              driver=os.environ.get('SDL_AUDIODRIVER', 'SDL default'))
        except (pygame.error, OSError) as error:
            self.stats['error'] = str(error)
            print(f'Audio unavailable: {error}. See docs/LOCAL_TESTING.md.', flush=True)

    def start(self):
        if not self.stats['enabled']:
            return
        self.thread = threading.Thread(target=self._pump, name='hvm-audio', daemon=True)
        self.thread.start()

    def _pump(self):
        recording = None
        try:
            if self.capture:
                recording = wave.open(str(self.capture), 'wb')
                recording.setparams((2, 2, 44100, 0, 'NONE', 'not compressed'))
            pending = deque()
            started = False
            while not self.stop_event.is_set():
                # SDL holds at most a playing block and one queued block. The
                # kernel socket holds a few more; the producer drops on overflow.
                if self.channel.get_queue() is not None:
                    self.stop_event.wait(0.002)
                    continue
                busy = self.channel.get_busy()
                if not busy and started:
                    self.stats['underruns'] += 1
                    started = False
                if busy and pending:
                    self.channel.queue(pending.popleft())
                    continue
                try:
                    block = self.reader.recv(8193)
                except socket.timeout:
                    continue
                if len(block) != 8192:
                    raise ValueError('Invalid audio block length')
                sound = pygame.mixer.Sound(buffer=block)
                if recording:
                    recording.writeframesraw(block)
                self.stats['blocks'] += 1
                if busy:
                    self.channel.queue(sound)
                else:
                    # Prime three blocks (~139 ms). Starting with the very first
                    # packet leaves no margin for host scheduling/SDL callback jitter.
                    pending.append(sound)
                    if len(pending) >= 3:
                        self.channel.play(pending.popleft())
                        self.channel.queue(pending.popleft())
                        started = True
        except Exception as error:
            self.stats['error'] = str(error)
            print(f'Audio output failed: {error}', flush=True)
        finally:
            if recording:
                recording.close()

    def close(self, report):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)
        for endpoint in (self.reader, self.writer):
            if endpoint:
                endpoint.close()
        report.write_text(json.dumps(self.stats, indent=2) + '\n')
        pygame.mixer.quit()
