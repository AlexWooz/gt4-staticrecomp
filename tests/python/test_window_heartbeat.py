"""TASK-145: the game window's responsiveness timer must stay (docs/decisions/0076-window-responsiveness-timer.md).

Without it Windows takes the game window as "not responding" after 5 seconds without input, and in borderless
fullscreen on NVIDIA RTX cards the desktop flashes and a frame comes late every 6 seconds. The C++ fixture
(window_heartbeat_fixture) checks that the timer message arrives; these checks keep the game wired to it.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WindowHeartbeatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (ROOT / 'native/window_heartbeat.hpp').read_text(encoding='utf-8')
        cls.window = (ROOT / 'native/window.cpp').read_text(encoding='utf-8')

    def test_interval_is_well_inside_windows_threshold(self):
        match = re.search(r'constexpr UINT interval_ms = (\d+);', self.header)
        self.assertIsNotNone(match, 'interval_ms not found')
        self.assertLessEqual(int(match.group(1)), 1000, 'keep at least four timer messages per 5 second window')

    def test_game_window_starts_the_timer_after_creation(self):
        created = self.window.index('game_window = CreateWindowExW(')
        started = self.window.index('gt4::window_heartbeat::start(game_window)')
        self.assertGreater(started, created)
        self.assertIn('gt4::window_heartbeat::wanted(std::getenv("GT4_WINDOW_HEARTBEAT"))', self.window)

    def test_window_procedure_answers_the_timer(self):
        procedure = self.window[self.window.index('LRESULT CALLBACK window_procedure('):]
        procedure = procedure[:procedure.index('\n}\n')]
        self.assertIn('gt4::window_heartbeat::is_heartbeat(message, first)', procedure)

    def test_message_loop_reads_every_message(self):
        pump = self.window[self.window.index('bool pump_messages() {'):]
        pump = pump[:pump.index('\n}\n')]
        self.assertIn('PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)', pump,
                      'a filtered PeekMessage would leave the timer message unread')


if __name__ == '__main__':
    unittest.main()
