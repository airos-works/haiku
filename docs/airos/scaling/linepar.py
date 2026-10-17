#!/usr/bin/env python3
"""Find one pixel lines whose width depends on where they fall.

linepar.py <scale> A.png <ax> <ay> B.png <bx> <by> <w> <h> [<ox> <oy>]
  A.png, B.png: buffer pixel captures (devshot, parshot.sh) of one window,
  its top left corner at logical (ax, ay) and (bx, by), w x h logical pixels
  in size; captures of part of the screen start at buffer pixel (ox, oy).
  With bx = ax + 1 and by = ay + 1 the grid changes at 150 percent.
Prints the straight thin lines (1 to 3 buffer pixels across, MIN long, with
a clear contrast to both sides) found in both captures at the same logical
place and colour whose width differs, then how many kept their width.
"""
import sys
from math import floor
from PIL import Image

MIN = 24


def contrast(a, b):
    return max(abs(a[k] - b[k]) for k in range(3))


def segments(img, x0, y0, x1, y1, vertical):
    """Thin lines along one axis: {(across, width, colour): [(along0, along1)]}"""
    px = img.load()
    if vertical:
        lines_range, across_range = range(y0, y1), (x0, x1)
        get = lambda along, across: px[across, along]
    else:
        lines_range, across_range = range(x0, x1), (y0, y1)
        get = lambda along, across: px[along, across]
    found = {}
    for along in lines_range:
        a, b = across_range
        row = [get(along, i) for i in range(a, b)]
        i = 0
        n = len(row)
        while i < n:
            j = i
            while j + 1 < n and row[j + 1] == row[i]:
                j += 1
            width = j - i + 1
            if 1 <= width <= 3 and i > 0 and j < n - 1 \
                    and contrast(row[i - 1], row[i]) >= 6 \
                    and contrast(row[j + 1], row[i]) >= 6:
                found.setdefault((a + i, width, row[i]), []).append(along)
            i = j + 1
    result = []
    for (across, width, colour), alongs in found.items():
        alongs.sort()
        start = prev = alongs[0]
        for v in alongs[1:] + [None]:
            if v is not None and v == prev + 1:
                prev = v
                continue
            if prev - start + 1 >= MIN:
                result.append((across, width, colour, start, prev))
            if v is not None:
                start = prev = v
    return result


def main():
    scale = float(sys.argv[1])
    A = Image.open(sys.argv[2]).convert('RGB')
    ax, ay = int(sys.argv[3]), int(sys.argv[4])
    B = Image.open(sys.argv[5]).convert('RGB')
    bx, by = int(sys.argv[6]), int(sys.argv[7])
    w, h = int(sys.argv[8]), int(sys.argv[9])
    ox, oy = (int(sys.argv[10]), int(sys.argv[11])) if len(sys.argv) > 11 \
        else (0, 0)

    def box(x, y):
        return (int(floor(x * scale)) - ox, int(floor(y * scale)) - oy,
            int(floor((x + w) * scale)) - ox, int(floor((y + h) * scale)) - oy)

    bad = good = 0
    for vertical in (True, False):
        sa = segments(A, *box(ax, ay), vertical)
        sb = segments(B, *box(bx, by), vertical)
        oa, ob = (ax, bx) if vertical else (ay, by)
        oa2, ob2 = (ay, by) if vertical else (ax, bx)
        # logical position relative to the window
        da, db = (ox, oy) if vertical else (oy, ox)

        def key(s, o, o2):
            across, width, colour, s0, s1 = s
            return ((across + da) / scale - o,
                ((s0 + db) / scale - o2, (s1 + db) / scale - o2))
        used = set()
        for s in sa:
            la, (l0, l1) = key(s, oa, oa2)
            best = None
            nearest = 0.9
            for k, t in enumerate(sb):
                if k in used or t[2] != s[2]:
                    continue
                lb, (m0, m1) = key(t, ob, ob2)
                if abs(la - lb) < nearest and abs(l0 - m0) < 2 \
                        and abs(l1 - m1) < 2:
                    best = k
                    nearest = abs(la - lb)
            if best is None:
                continue
            used.add(best)
            t = sb[best]
            if t[1] != s[1]:
                bad += 1
                print('%s line at logical %s=%.1f, %s %.0f..%.0f, colour %02x%02x%02x: '
                    'width %d vs %d' % ('vertical' if vertical else 'horizontal',
                    'x' if vertical else 'y', la, 'y' if vertical else 'x', l0, l1,
                    *s[2], s[1], t[1]))
            else:
                good += 1
    print('even: %d, depends on position: %d' % (good, bad))


main()
