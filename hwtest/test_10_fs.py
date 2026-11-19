"""Filesystem round trips through the shell."""

from harness import check

F = "hwt_fs.txt"


def _rm(p, *names):
    for name in names:
        p.cmd(f"rm {name}")   # ignore "not found"


def test_write_cat_rm(ctx):
    p = ctx.pico
    try:
        out = p.cmd(f"fs write {F} hello hardware test")
        check(f"wrote 20 bytes to '{F}'" in out, f"fs write reports 20 bytes: {out!r}")
        check("hello hardware test" in p.cmd(f"cat {F}"), "cat shows the text")
        check(F in p.cmd("ls"), "ls lists the file")
        check(f"deleted '{F}'" in p.cmd(f"rm {F}"), "rm deletes it")
        check("cannot open" in p.cmd(f"cat {F}"), "cat fails after rm")
    finally:
        _rm(p, F)


def test_append(ctx):
    p = ctx.pico
    try:
        p.cmd(f"fs write {F} line one")
        out = p.cmd(f"fs append {F} line two")
        check("appended 9 bytes" in out, f"append reports 9 bytes: {out!r}")
        text = p.cmd(f"cat {F}")
        check("line one\nline two" in text, f"both lines, in order: {text!r}")
    finally:
        _rm(p, F)


def test_multiline_write(ctx):
    """`fs write <f>` reads its own lines.  Each one is sent only after its
    "> " prompt; sent early, a line would be read by the wrong reader."""
    p = ctx.pico
    try:
        body = [f"row {i} of the multi-line test" for i in range(8)]
        since = p.start(f"fs write {F}")   # prompts "> ", not "pico> "
        p.expect(r"\n> ", since=since)
        out = p.type_lines(body + ["."])
        check("wrote" in out, f"fs write finished: {out[-200:]!r}")
        text = p.cmd(f"cat {F}")
        check(text.strip().split("\n") == body,
              f"file holds exactly the lines sent: {text!r}")
    finally:
        _rm(p, F)


def test_survives_reboot(ctx):
    p = ctx.pico
    try:
        p.cmd(f"fs write {F} persists across reboot")
        p.reboot()
        check("persists across reboot" in p.cmd(f"cat {F}"), "file survives a reboot")
    finally:
        _rm(p, F)


# Issue #7: names of FS_NAME_MAX (16) characters or more.  They used to be
# stored cut to 15, which no lookup matched: each write made another entry
# under the short name, and `rm` with the full name deleted nothing.
LONG = "hwt_longname_16c"   # 16 chars: one too many
SHORT = LONG[:15]           # the most a name can hold


def _entries(p, name):
    """ls rows whose name column is exactly `name`."""
    return [ln for ln in p.lines(p.cmd("ls")) if ln.split()[:1] == [name]]


def test_long_name_refused_by_shell(ctx):
    p = ctx.pico
    assert len(LONG) == 16 and len(SHORT) == 15
    try:
        for _ in range(2):
            out = p.cmd(f"fs write {LONG} too long")
            check("too long (max 15 characters)" in out,
                  f"fs write names the limit: {out!r}")
        out = p.cmd(f"fs append {LONG} too long")
        check("too long (max 15 characters)" in out, f"fs append names the limit: {out!r}")
        check(_entries(p, SHORT) == [], "no truncated entry was created")
        check("not found" in p.cmd(f"rm {LONG}"), "rm of a 16-char name finds nothing")

        out = p.cmd(f"fs write {SHORT} fits")
        check(f"wrote 5 bytes to '{SHORT}'" in out, f"a 15-char name still works: {out!r}")
        check(len(_entries(p, SHORT)) == 1, "exactly one entry for the 15-char name")
        check("cannot open" in p.cmd(f"cat {LONG}"), "the 16-char name does not alias it")
    finally:
        _rm(p, SHORT)


def test_long_name_refused_by_kernel(ctx):
    """Through vfs_open() on the device, past the shell's length check."""
    p = ctx.pico
    try:
        since = p.mark()
        p.cmd("run hwtest fs-longname")
        m = p.expect(r"\[hwtest\] fs-longname: RESULT: (PASS|FAIL)", timeout=20.0, since=since)
        fails = [ln for ln in p.text_since(since).split("\n") if "FAIL " in ln]
        check(m.group(1) == "PASS", f"device test fs-longname failed: {fails}")
        check(_entries(p, SHORT) == [], "the device test left no file behind")
    finally:
        _rm(p, SHORT)


def test_trunc_without_write_survives_reboot(ctx):
    """#18: `fs write <f>` then "." at once opens with O_TRUNC and writes
    nothing.  The file showed 0 bytes, but a reboot brought the old data
    back: the truncation was never committed to flash."""
    p = ctx.pico
    try:
        p.cmd(f"fs write {F} old contents")
        since = p.start(f"fs write {F}")
        p.expect(r"\n> ", since=since)
        p.type_lines(["."])
        check(_entries(p, F) and _entries(p, F)[0].split()[1] == "0", "0 bytes after truncate")
        p.reboot()
        rows = _entries(p, F)
        check(rows and rows[0].split()[1] == "0", f"still 0 bytes after reboot: {rows}")
        check("old contents" not in p.cmd(f"cat {F}"), "the old data is gone")
    finally:
        _rm(p, F)


def test_full_length_line_keeps_newline(ctx):
    """#39: interactive `fs write` dropped the newline of a line that filled
    the shell's line buffer (127 chars), joining it to the next line."""
    p = ctx.pico
    long_line = "L" * 127
    try:
        since = p.start(f"fs write {F}")
        p.expect(r"\n> ", since=since)
        out = p.type_lines([long_line, "next", "."])
        check("wrote 133 bytes" in out, f"127 + 1 + 4 + 1 bytes written: {out[-120:]!r}")
        text = p.cmd(f"cat {F}")
        check(long_line + "\nnext" in text, "the long line ends with its own newline")
    finally:
        _rm(p, F)


def test_other_fds_during_write_and_delete(ctx):
    """#31 and #8, through vfs_* on the device (see `run hwtest fs-fds`)."""
    p = ctx.pico
    try:
        since = p.mark()
        p.cmd("run hwtest fs-fds")
        m = p.expect(r"\[hwtest\] fs-fds: RESULT: (PASS|FAIL)", timeout=20.0, since=since)
        fails = [ln for ln in p.text_since(since).split("\n") if "FAIL " in ln]
        check(m.group(1) == "PASS", f"device test fs-fds failed: {fails}")
    finally:
        _rm(p, "hwt_fds_a.txt", "hwt_fds_b.txt")
