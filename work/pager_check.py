"""Pure-logic model of the paging arithmetic, so it can be checked in seconds.

The calculator program keeps only one window of the document in RAM.  A window
holds a whole number of 5-row pages (except the last one), because a page that
straddles two windows shows a gap: the previous window's last rows and the next
window's first rows were read with different offsets.

This model mirrors the algorithm the C code must use, expressed on row counts so
it can be exhaustively tested without the emulator.
"""

PAGE = 5


def windows(n_rows, cap):
    """Cut n_rows into windows, each a whole number of pages apart from the last."""
    out = []
    first = 0
    while first < n_rows:
        left = n_rows - first
        if left <= cap:
            out.append((first, left))
            break
        pages = max(1, cap // PAGE)
        take = min(pages * PAGE, left)
        out.append((first, take))
        first += take
    return out


def last_top(count):
    return ((count - 1) // PAGE) * PAGE


def step(win, idx, view_top, direction):
    """One page step: exactly PAGE rows, or no movement at the very ends."""
    count = win[idx][1]
    if direction > 0:
        if view_top < last_top(count):
            return idx, view_top + PAGE
        if idx + 1 < len(win):
            return idx + 1, 0
        return idx, view_top
    if view_top >= PAGE:
        return idx, view_top - PAGE
    if idx > 0:
        return idx - 1, last_top(win[idx - 1][1])
    return idx, 0


def global_row(win, idx, view_top):
    return win[idx][0] + view_top + 1        # 1-based row the counter shows


def check(n_rows, cap):
    win = windows(n_rows, cap)
    assert sum(c for _, c in win) == n_rows, "windows lose rows"

    idx, top = 0, 0
    downs = [global_row(win, idx, top)]
    for _ in range(n_rows + 2):
        idx, top = step(win, idx, top, +1)
        downs.append(global_row(win, idx, top))

    deltas = [b - a for a, b in zip(downs, downs[1:])]
    assert all(d in (0, PAGE) for d in deltas), "down step is not a page"
    assert all(b >= a for a, b in zip(downs, downs[1:])), "down went backwards"
    final = last_top(win[-1][1]) + win[-1][0] + 1
    assert downs[-1] == final, "down ends at %d, expected %d" % (downs[-1], final)

    ups = [downs[-1]]
    for _ in range(n_rows + 2):
        idx, top = step(win, idx, top, -1)
        ups.append(global_row(win, idx, top))
    assert all(b <= a for a, b in zip(ups, ups[1:])), "up went forwards"
    assert all(a - b in (0, PAGE) for a, b in zip(ups, ups[1:])), "up step is not a page"
    assert ups[-1] == 1, "up ends at %d, expected 1" % ups[-1]

    # the two directions must visit the same rows in reverse order
    def uniq(seq):
        out = [seq[0]]
        for v in seq[1:]:
            if v != out[-1]:
                out.append(v)
        return out

    assert list(reversed(uniq(ups))) == uniq(downs), "up and down are not symmetric"

    # every row is reachable
    for r in range(n_rows):
        i = next(i for i, (f, c) in enumerate(win) if f <= r < f + c)
        assert 0 <= r - win[i][0] < win[i][1]
    return win


if __name__ == "__main__":
    cases = [1, 2, 4, 5, 6, 9, 10, 11, 1199, 1200, 1201, 1202, 2399, 2400, 2401, 4801]
    caps = [5, 6, 10, 11, 25, 100, 250, 511, 512, 1000, 4096]
    for n in cases:
        for cap in caps:
            check(n, cap)
    print("PASS: %d combinations, every page step is exactly %d rows, "
          "up/down symmetric, no row lost" % (len(cases) * len(caps), PAGE))
