#!/usr/bin/env python3
"""Draws hangar's logo as SVG files, next to this script.

    python3 logo.py

The mark is a hangar seen from the front, cut from plate: a peaked roof,
spikes at its shoulders, walls that end in blades, and a vehicle hanging
inside on its tether. The vehicle has one eye: the camera dome, and the
electric eye the project descends from. The wordmark is cut the same way,
in capitals: the first and the last letter are drawn down into points, the
letters between them shorten towards the middle, and a cut runs through
the word. Everything is a filled polygon, so no font is needed to render
it.

The colours are those of the Tokyo Night editor theme, in its night and
its day form. Files written:
    logo-night.svg    the mark with the wordmark, for dark backgrounds
    logo-day.svg      the same, for light backgrounds
    icon.svg          the mark on a tile (favicon, avatar)

The logos are transparent. Dawn Patrol's page carries icon.svg within
itself (modules/console/web/index.html): after a change here, that copy is
to be replaced by hand.

Standard library only. The mark is 128 by 128 in its own units.
"""

from pathlib import Path

# (background it is meant for, ink, eye)
NIGHT = ("#1a1b26", "#7aa2f7", "#ff9e64")
DAY = ("#e1e2e7", "#34548a", "#b15c00")

SIZE = 128


def fmt(v):
    return f"{v:.2f}".rstrip("0").rstrip(".")


def poly(points):
    return "M" + " L".join(f"{fmt(x)},{fmt(y)}" for x, y in points) + "Z"


def mark(ink, eye):
    """The mark, as SVG elements in a 128 by 128 box."""
    # The hangar: outer edge, then back along the inner edge. The walls'
    # outer edges run on past the floor to a point.
    hangar = [(13, 120), (13, 52), (3, 27), (25, 38.5), (40, 22), (64, 13),
              (88, 22), (103, 38.5), (125, 27), (115, 52), (115, 120),
              (105, 102), (105, 56), (84, 32), (64, 24.5), (44, 32),
              (23, 56), (23, 102)]
    tether = [(62, 24), (66, 24), (66, 66), (62, 66)]
    body = [(40, 74), (51, 65), (77, 65), (88, 74), (88, 85), (75, 97),
            (64, 92), (53, 97), (40, 85)]
    socket = [(50, 79), (64, 71), (78, 79), (64, 87)]
    thrusters = [[(30, 70), (36, 74), (36, 86), (30, 90)],
                 [(98, 70), (92, 74), (92, 86), (98, 90)]]
    pupil = [(59, 79), (64, 75.5), (69, 79), (64, 82.5)]
    solid = poly(hangar) + poly(tether) + "".join(poly(t) for t in thrusters)
    return (
        f'<path fill="{ink}" d="{solid}"/>'
        f'<path fill="{ink}" fill-rule="evenodd" d="{poly(body)}{poly(socket)}"/>'
        f'<path fill="{eye}" d="{poly(pupil)}"/>'
    )


# The wordmark. Each letter is a set of polygons on a grid with y upwards
# from the baseline; capitals are 60 high and stems 12 wide. H and R, the
# outer letters, carry the blades.
LETTERS = {
    "H": (42, [
        [(0, 74), (12, 62), (12, -8), (0, -30)],            # the blade
        [(12, 38), (30, 32), (30, 20), (12, 26)],
        [(30, 60), (42, 60), (42, 0), (30, 0)],
    ]),
    "A": (42, [
        [(0, 0), (21, 70), (42, 0), (30, 0), (21, 36), (12, 0)],
    ]),
    "N": (42, [
        [(0, 0), (0, 70), (12, 60), (12, 0)],
        [(0, 60), (13, 60), (42, 0), (29, 0)],
        [(30, 60), (42, 60), (42, -10), (30, 0)],
    ]),
    "G": (42, [
        [(10, 60), (40, 60), (34, 48), (15, 48), (12, 45), (12, 15),
         (15, 12), (30, 12), (30, 24), (20, 24), (24, 34), (42, 34),
         (42, 0), (10, 0), (0, 10), (0, 50)],
    ]),
    "R": (46, [
        [(0, 0), (0, 60), (12, 60), (12, 0)],
        [(12, 60), (32, 60), (42, 50), (42, 36), (32, 26), (12, 26),
         (12, 38), (27, 38), (30, 41), (30, 45), (27, 48), (12, 48)],
        [(17, 26), (31, 26), (46, -30), (34, -6)],          # the blade
    ]),
}
GAP = 9
CAP = 60
# The letters keep their tops in line and shorten towards the middle of the
# word, so its lower edge rises between the two blades.
HEIGHTS = (1.0, 0.86, 0.74, 0.74, 0.86, 1.0)


def wordmark(ink, x0, baseline):
    """HANGAR as one filled path; returns the element and the width used."""
    d, x = [], 0.0
    for ch, k in zip("HANGAR", HEIGHTS):
        width, shapes = LETTERS[ch]
        for s in shapes:
            d.append(poly([(x0 + x + px, baseline - (CAP - (CAP - py) * k))
                           for px, py in s]))
        x += width + GAP
    return f'<path fill="{ink}" d="{"".join(d)}"/>', x - GAP


def svg(width, height, body, title):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {fmt(width)} {fmt(height)}" '
        f'width="{fmt(width)}" height="{fmt(height)}" role="img" '
        f'aria-label="{title}"><title>{title}</title>{body}</svg>\n'
    )


def logo(scheme):
    _, ink, eye = scheme
    # The blades of the H and the R reach the same depth as the hangar's.
    x0, baseline = SIZE + 20, 90
    text, used = wordmark(ink, x0, baseline)
    # A cut through the word, rising to the right.
    text = (
        f'<mask id="cut" maskUnits="userSpaceOnUse" x="0" y="0" '
        f'width="{fmt(x0 + used + 20)}" height="{SIZE + 4}">'
        f'<rect width="{fmt(x0 + used + 20)}" height="{SIZE + 4}" '
        f'fill="#fff"/><path d="M{fmt(x0 - 8)},{baseline - 12} '
        f'L{fmt(x0 + used + 8)},{baseline - 34}" stroke="#000" '
        f'stroke-width="3.5"/></mask><g mask="url(#cut)">{text}</g>')
    width = x0 + used + 10
    return svg(width, SIZE + 4, mark(ink, eye) + text, "hangar")


def icon(scheme):
    ground, ink, eye = scheme
    tile = f'<rect width="{SIZE}" height="{SIZE}" rx="24" fill="{ground}"/>'
    inner = (f'<g transform="translate({SIZE / 2} {SIZE / 2}) scale(0.8) '
             f'translate({-SIZE / 2} {-SIZE / 2 - 2})">'
             f'{mark(ink, eye)}</g>')
    return svg(SIZE, SIZE, tile + inner, "hangar")


def main():
    out = Path(__file__).resolve().parent
    files = {
        "logo-night.svg": logo(NIGHT),
        "logo-day.svg": logo(DAY),
        "icon.svg": icon(NIGHT),
    }
    for name, text in files.items():
        (out / name).write_text(text)
        print(name)


if __name__ == "__main__":
    main()
