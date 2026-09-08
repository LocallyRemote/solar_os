from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMANDS = (ROOT / "src/shell/solar_os_shell_commands.c").read_text(
    encoding="utf-8"
)


class OtaProgressWidthTest(unittest.TestCase):
    def test_upgrade_progress_reserves_one_column_before_wrapping(self):
        start = COMMANDS.index("static void ota_render_progress_line(")
        end = COMMANDS.index("static void ota_shell_progress_cb(", start)
        renderer = COMMANDS[start:end]

        self.assertIn("solar_os_shell_io_cols(term)", renderer)
        self.assertIn("cols > 1U ? cols - 1U : cols", renderer)
        self.assertIn(
            "line_budget > fixed_width ? line_budget - fixed_width : 0U",
            renderer,
        )
        self.assertIn("solar_os_shell_io_write_len(term, line, line_len)", renderer)

    def test_upgrade_progress_is_emitted_as_one_bounded_string(self):
        start = COMMANDS.index("static void ota_shell_progress_cb(")
        end = COMMANDS.index("static void ota_upgrade_task(", start)
        callback = COMMANDS[start:end]

        self.assertIn("ota_render_progress_line(", callback)
        self.assertNotIn('solar_os_shell_io_printf(state->term, " v%s"', callback)


if __name__ == "__main__":
    unittest.main()
