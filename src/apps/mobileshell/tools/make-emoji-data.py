#!/usr/bin/env python3
# Copyright 2026, air/OS.
# Copyright 2026 Renku Mobile contributors.
# Distributed under the terms of the MIT License.
"""make-emoji-data.py EMOJI-TEST.TXT > ../EmojiData.h

The on-screen keyboard's emoji, from Unicode's emoji-test.txt (15.0, which the
Noto Color Emoji font of the image covers). It keeps the fully-qualified emoji
of one character, with or without the emoji presentation selector: each has
to fit the 8 bytes of a key the keyboard add-on takes (mobile_key in
Protocol.h), which leaves out the sequences (skin tones, families, flags,
keycaps).
"""
import sys

# Unicode's groups, in the keyboard's categories
CATEGORIES = [
    ("\U0001F600", ["Smileys & Emotion", "People & Body"]),
    ("\U0001F436", ["Animals & Nature"]),
    ("\U0001F34F", ["Food & Drink"]),
    ("⚽", ["Activities"]),
    ("\U0001F697", ["Travel & Places"]),
    ("\U0001F4A1", ["Objects"]),
    ("❤️", ["Symbols"]),
]

groups = {}
group = None
for line in open(sys.argv[1], encoding="utf-8"):
    if line.startswith("# group:"):
        group = line.split(":", 1)[1].strip()
        continue
    if not line.strip() or line.startswith("#"):
        continue
    codes, rest = line.split(";", 1)
    if rest.split("#", 1)[0].strip() != "fully-qualified":
        continue
    points = [int(c, 16) for c in codes.split()]
    if len(points) == 2 and points[1] == 0xFE0F:
        points = points[:1] + [0xFE0F]
    elif len(points) != 1:
        continue
    emoji = "".join(chr(p) for p in points)
    assert len(emoji.encode()) < 8, emoji
    groups.setdefault(group, []).append(emoji)

def literal(text):
    return '"' + "".join("\\x%02x" % b for b in text.encode()) + '"'

out = sys.stdout
out.write("""/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */

/*	The keyboard's emoji by category: the fully-qualified emoji of one
	character (with or without the presentation selector) of Unicode's
	emoji-test.txt 15.0 (Unicode License). Generated; don't edit.
*/
#ifndef EMOJI_DATA_H
#define EMOJI_DATA_H


struct EmojiCategory {
	const char*			icon;
	const char* const*	emoji;
	int					count;
};

""")
for index, (icon, names) in enumerate(CATEGORIES):
    emoji = [e for name in names for e in groups[name]]
    out.write("static const char* const kEmoji%d[] = {\n" % index)
    for i in range(0, len(emoji), 8):
        out.write("\t" + ", ".join(literal(e) for e in emoji[i:i + 8]) + ",\n")
    out.write("};\n\n")

out.write("static const EmojiCategory kEmojiCategories[] = {\n")
for index, (icon, names) in enumerate(CATEGORIES):
    out.write("\t{ %s, kEmoji%d, sizeof(kEmoji%d) / sizeof(kEmoji%d[0]) },"
        "\t// %s\n" % (literal(icon), index, index, index, " and ".join(names)))
out.write("""};
static const int kEmojiCategoryCount
	= sizeof(kEmojiCategories) / sizeof(kEmojiCategories[0]);


#endif	// EMOJI_DATA_H
""")
