from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "src/services/solar_os_gfx_icon_table.inc"
DESCRIPTOR = (ROOT / "src/apps/solar_os_script_api.inc").read_text(encoding="utf-8")
PYTHON = (ROOT / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
LUA = (ROOT / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")


class ScriptGfxIconsTest(unittest.TestCase):
    def test_all_icons_have_generated_canonical_names(self):
        entries = re.findall(
            r'^SOLAR_OS_GFX_ICON_ENTRY\(([A-Z0-9_]+), "([a-z0-9-]+)"\)$',
            TABLE.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
        self.assertEqual(len(entries), 223)
        self.assertEqual(dict(entries)["TABLET"], "tablet")
        for symbol, name in entries:
            self.assertEqual(name, symbol.lower().replace("_", "-"))

    def test_python_and_lua_share_the_icon_binding(self):
        self.assertIn("SOLAR_OS_SCRIPT_API_FUNCTION(gfx, icon, icon);", DESCRIPTOR)
        for source, prefix in ((PYTHON, "PYTHON"), (LUA, "SOLUA")):
            self.assertIn(f"{prefix}_EVENT_GFX_ICON", source)
            self.assertIn("solar_os_gfx_icon_from_name", source)
            self.assertIn("solar_os_gfx_icon(gfx", source)


if __name__ == "__main__":
    unittest.main()
