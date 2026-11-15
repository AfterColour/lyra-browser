# Builds resources/icons/LyraIcons.ttf: a small icon font with the same code points as
# Segoe MDL2 Assets / Segoe Fluent Icons, made from Microsoft's Fluent UI System Icons (MIT).
# Lyra uses it only when Windows has neither Segoe icon font (or it lacks a glyph), so the
# toolbar never shows empty boxes.
#
#   python tools/make-icon-font.py <folder with FluentSystemIcons-Regular/Filled .ttf + .json>
#
# The sources: https://github.com/microsoft/fluentui-system-icons/tree/main/fonts
import json, os, sys
from fontTools.ttLib import TTFont
from fontTools import subset

# Segoe code point -> Fluent icon name (size 20). "!" in front = the filled variant.
MAP = {
    0xE708: "weather_moon", 0xE70D: "chevron_down", 0xE710: "add", 0xE711: "dismiss",
    0xE712: "more_horizontal", 0xE713: "settings", 0xE718: "pin", 0xE71B: "link",
    0xE71E: "zoom_fit", 0xE71F: "zoom_out", 0xE721: "search", 0xE728: "star_line_horizontal_3",
    0xE72C: "arrow_clockwise", 0xE72E: "lock_closed", 0xE734: "star", 0xE735: "!star",
    0xE736: "book_open", 0xE737: "document", 0xE749: "print", 0xE74D: "delete",
    0xE74F: "speaker_mute", 0xE765: "keyboard", 0xE767: "speaker_2", 0xE76B: "chevron_left",
    0xE76C: "chevron_right", 0xE774: "globe", 0xE77A: "pin_off", 0xE790: "color",
    0xE7A7: "arrow_undo", 0xE7C4: "tab_desktop", 0xE80F: "home", 0xE81C: "history",
    0xE81D: "location", 0xE838: "folder_open", 0xE896: "arrow_download", 0xE8A0: "split_vertical",
    0xE8A3: "zoom_in", 0xE8A5: "document", 0xE8AB: "arrow_swap", 0xE8B7: "folder",
    0xE8B9: "picture_in_picture", 0xE8BB: "dismiss", 0xE8C1: "translate", 0xE8C8: "copy",
    0xE8D7: "key", 0xE921: "subtract", 0xE922: "maximize", 0xE923: "square_multiple",
    0xE943: "code", 0xE945: "flash", 0xE946: "info", 0xE993: "speaker_1", 0xEA8F: "alert",
    0xEC4A: "top_speed", 0xED1A: "eye_off",
    # menus
    0xE8E5: "open", 0xE8A7: "open", 0xE72D: "share", 0xE74E: "save", 0xE8B3: "select_all_on",
    0xE8C6: "cut", 0xE77F: "clipboard_paste", 0xE7BA: "warning", 0xE73E: "checkmark",
    0xE76E: "emoji", 0xE91B: "image", 0xE8D2: "text_font", 0xE714: "video", 0xE8A1: "eye",
    0xE72A: "arrow_right", 0xE72B: "arrow_left", 0xE7EF: "people", 0xE8F4: "folder_add",
    0xE70F: "edit", 0xE71A: "stop", 0xE7A6: "arrow_redo", 0xE740: "full_screen_maximize", 0xE8EE: "arrow_repeat_all", 0xE768: "play", 0xE769: "pause", 0xE8D6: "music_note_2",
}

def main(src):
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "icons", "LyraIcons.ttf")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    fonts, names = {}, {}
    for kind in ("Regular", "Filled"):
        fonts[kind] = TTFont(os.path.join(src, f"FluentSystemIcons-{kind}.ttf"))
        names[kind] = json.load(open(os.path.join(src, f"FluentSystemIcons-{kind}.json"), encoding="utf-8"))
    base = fonts["Regular"]
    glyf, hmtx = base["glyf"], base["hmtx"]
    rcmap = {}
    for kind in fonts:
        rcmap[kind] = fonts[kind].getBestCmap()
    newmap, keep, missing = {}, set(), []
    for cp, name in MAP.items():
        kind = "Filled" if name.startswith("!") else "Regular"
        name = name.lstrip("!")
        key = f"ic_fluent_{name}_20_{kind.lower()}"
        if key not in names[kind]:
            missing.append(key); continue
        gname = rcmap[kind][names[kind][key]]
        if kind == "Filled":
            new = "lyra_" + gname
            if new not in glyf.glyphs:
                g = fonts["Filled"]["glyf"][gname]
                glyf.glyphs[new] = g
                hmtx.metrics[new] = fonts["Filled"]["hmtx"][gname]
                order = base.getGlyphOrder() + [new]
                base.setGlyphOrder(order)
                glyf.glyphOrder = order
            gname = new
        newmap[cp] = gname
        keep.add(gname)
    if missing:
        print("missing:", ", ".join(missing)); sys.exit(1)
    for t in base["cmap"].tables:
        if t.isUnicode():
            t.cmap = dict(newmap)
    base["maxp"].numGlyphs = len(base.getGlyphOrder())
    opts = subset.Options()
    opts.name_IDs = []
    opts.layout_features = []
    opts.notdef_outline = True
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=list(newmap.keys()))
    sub.subset(base)
    for rec in base["name"].names:
        pass
    n = base["name"]
    n.names = []
    for nid, s in ((1, "Lyra Icons"), (2, "Regular"), (3, "Lyra Icons"), (4, "Lyra Icons"),
                   (5, "Version 1.0"), (6, "LyraIcons-Regular"),
                   (0, "Icons from Fluent UI System Icons, Copyright (c) 2020 Microsoft Corporation, MIT License")):
        n.setName(s, nid, 3, 1, 0x409)
        n.setName(s, nid, 1, 0, 0)
    base.save(out)
    print(out, os.path.getsize(out), "bytes,", len(newmap), "icons")

if __name__ == "__main__":
    main(sys.argv[1])
