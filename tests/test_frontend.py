#!/usr/bin/env python3
"""Small source-contract checks for the dependency-free score editor."""

import html
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INDEX = (ROOT / "public" / "index.html").read_text(encoding="utf-8")
APP = (ROOT / "public" / "app.js").read_text(encoding="utf-8")


def main():
    decoded = html.unescape(INDEX)
    leaping = re.search(
        r'data-notes="([^"]+)"[^>]*><strong>Leaping duo</strong>', decoded
    )
    assert leaping, "Leaping duo preset is missing"
    assert leaping.group(1) == (
        "Upper: C5/1 G5/0.5 E5/0.5 A5/1 F5/2 D5/1 B4/1 C5/2\n"
        "Lower: R/0.5 C4/0.5 E4/1 G4/1 F4/0.5 A4/0.5 B4/2 G4/1 C5/2"
    ), "Leaping duo rhythm changed unexpectedly"

    for value in ("0.0625", "0.125", "0.25", "0.5", "1", "2", "4"):
        assert f'data-duration="{value}"' in INDEX, f"missing duration {value}"
    for glyph in ("𝅁", "𝅀", "𝄿", "𝄾", "𝄽", "𝄼", "𝄻"):
        assert glyph in APP, f"missing rest glyph {glyph}"

    assert r"^([^:]+):\s*(.*)$" in APP, "named empty voices must remain parseable"
    assert "voice.notes=''" in APP, "Clear voice must produce an empty voice"
    assert "voice.notes='R/1'" not in APP, "Clear voice must not insert a fake rest"
    assert "||'R/1'" not in APP, "deleting the final event must keep the voice empty"
    assert "Empty voice · choose a note or rest" in APP, "empty staff guidance is missing"
    assert "engravingSegments(events,meterLength)" in APP, "bar-aware engraving is missing"
    assert 'class=\"note-tie\"' in APP, "cross-bar notes must display ties"

    print("Frontend score-editor regression checks passed.")


if __name__ == "__main__":
    main()
