#!/usr/bin/env python3
"""Typeset the picoOS book as a print-ready PDF with LaTeX.

Usage:  python3 docs/book/build_pdf.py [--paper letter|a4|6x9] [--tex-only]

Reads learning-operating-systems-with-picoos.md, converts it to LaTeX, and
compiles it with LuaLaTeX and makeindex. The PDF has a title page, a table
of contents and an index, both with page numbers, running heads, and margins
set for two-sided printing and binding. The index uses the same terms as the
Markdown index (INDEX_TERMS in build_toc_index.py); each term is indexed at
its first mention in every section that build_toc_index.py cites.

Output:  learning-operating-systems-with-picoos[-a4|-6x9].pdf next to this
script. Intermediate files go to latex/ (safe to delete).

The converter handles only the Markdown this book uses: headings, paragraphs,
**bold**, *italic*, `code`, links, bullet and numbered lists (nested by
indentation), pipe tables, fenced code blocks and the one figure.
"""

import argparse
import datetime
import importlib.util
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT_DIR = HERE / "latex"
STEM = "learning-operating-systems-with-picoos"

# Reuse the Markdown tooling so the two indexes cannot drift apart.
_spec = importlib.util.spec_from_file_location("toc", HERE / "build_toc_index.py")
toc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(toc)

# Page geometry per paper size. inner > outer leaves room for the binding.
PAPERS = {
    "letter": "letterpaper, inner=1.1in, outer=0.85in, top=1in, bottom=1in",
    "a4": "a4paper, inner=28mm, outer=22mm, top=25mm, bottom=27mm",
    "6x9": "paperwidth=6in, paperheight=9in, inner=0.8in, outer=0.55in, "
           "top=0.75in, bottom=0.8in",
}
BASE_FONT = {"letter": "11pt", "a4": "11pt", "6x9": "10pt"}

# ---------------------------------------------------------------------------
# Inline Markdown -> LaTeX
# ---------------------------------------------------------------------------

_TEX_SPECIAL = {
    "\\": r"\textbackslash{}", "{": r"\{", "}": r"\}", "$": r"\$",
    "&": r"\&", "#": r"\#", "^": r"\textasciicircum{}", "_": r"\_",
    "%": r"\%", "~": r"\textasciitilde{}",
}


def tex_escape(s):
    return "".join(_TEX_SPECIAL.get(c, c) for c in s)


def code_escape(s):
    """Escape text for \\code{}, allowing line breaks after _ / . in names."""
    out = []
    for c in s:
        out.append(_TEX_SPECIAL.get(c, c))
        if c in "_/" or (c == "." and len(s) > 12):
            out.append(r"\allowbreak{}")
    return "".join(out)


def smart_quotes(s):
    s = re.sub(r'(^|[\s(\[{—–-])"', "\\1\u201c", s)
    s = s.replace('"', "\u201d")
    s = re.sub(r"(^|[\s(\[{—–-])'", "\\1\u2018", s)
    return s.replace("'", "\u2019")


def md_inline(s):
    """Convert one line of inline Markdown to LaTeX."""
    slots = []

    def keep(tex):
        slots.append(tex)
        return f"\x00{len(slots) - 1}\x00"

    s = re.sub(r"`([^`]+)`", lambda m: keep(r"\code{" + code_escape(m.group(1)) + "}"), s)

    def link(m):
        return keep(r"\href{" + m.group(2).replace("%", r"\%").replace("#", r"\#")
                    + "}{" + emphasis(smart_quotes(tex_escape(m.group(1)))) + "}")

    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, s)
    s = re.sub(r"\\([\\`*_{}\[\]()#+\-.!~|<>])", lambda m: keep(tex_escape(m.group(1))), s)
    s = emphasis(smart_quotes(tex_escape(s)))
    while "\x00" in s:
        s = re.sub(r"\x00(\d+)\x00", lambda m: slots[int(m.group(1))], s)
    return s


def emphasis(s):
    s = re.sub(r"\*\*(.+?)\*\*", r"\\textbf{\1}", s)
    return re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"\\emph{\1}", s)


def plain(s):
    """A heading as plain text, for PDF bookmarks."""
    s = re.sub(r"\\(.)", r"\1", s).replace("`", "")
    return tex_escape(s)


def heading(cmd, title):
    return f"\\{cmd}{{\\texorpdfstring{{{md_inline(title)}}}{{{plain(title)}}}}}"


# ---------------------------------------------------------------------------
# Index entries
# ---------------------------------------------------------------------------

def mi_quote(s):
    """Quote makeindex's special characters."""
    return re.sub(r'([!@|"])', r'"\1', s)


def index_cmd(label):
    key = toc.index_sort_key(label) or label
    return r"\index{" + mi_quote(key) + "@" + mi_quote(md_inline(label)) + "}"


def plan_index(sections):
    """Map each section id to {line number or 'start': [labels]}.

    A searched term is placed on the first body line that matches (the
    section start if only the title matched). A term with a fixed list of
    sections is placed at the start of each of them.
    """
    plan = {}
    for label, pattern, fixed in toc.INDEX_TERMS:
        rx = re.compile(pattern, re.I) if pattern else None
        for sid, title, _, body in sections:
            if fixed:
                if sid in fixed:
                    plan.setdefault(sid, {}).setdefault("start", []).append(label)
                continue
            if sid in toc.NO_SEARCH:
                continue
            where = next((i for i, l in enumerate(body) if rx.search(l)), None)
            if where is None and rx.search(title):
                where = "start"
            if where is not None:
                plan.setdefault(sid, {}).setdefault(where, []).append(label)
    return plan


# ---------------------------------------------------------------------------
# Block Markdown -> LaTeX
# ---------------------------------------------------------------------------

ITEM_RX = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")


def split_row(line):
    """Split a pipe-table row, ignoring | inside `code`."""
    cells, cur, in_code = [], "", False
    for c in line.strip().strip("|"):
        if c == "`":
            in_code = not in_code
        if c == "|" and not in_code:
            cells.append(cur.strip())
            cur = ""
        else:
            cur += c
    cells.append(cur.strip())
    return cells


def render_table(rows, marks):
    """rows: list of (line number, cells); the second row is the rule.

    Index entries for the rows are placed just before the table: inside a
    cell, \\index is written differently than in running text, so makeindex
    would list the same term twice.
    """
    header, body = rows[0], rows[2:]
    ncol = len(header[1])
    width = [max(len(re.sub(r"[`*\\]", "", r[1][c])) for r in [header] + body
                 if c < len(r[1])) for c in range(ncol)]
    spec = ["l" if w <= 20 else "Y" for w in width]
    if "Y" not in spec:
        spec[width.index(max(width))] = "Y"

    def row(n, cells, bold=False):
        cells = cells + [""] * (ncol - len(cells))
        tex = [md_inline(c) for c in cells]
        if bold:
            tex = [r"\textbf{" + t + "}" if t else t for t in tex]
        return " & ".join(tex) + r" \\"

    pre = "".join(marks(n) for n, _ in rows)
    out = [pre + "%"] if pre else []
    out += [r"\begin{small}", r"\begin{xltabular}{\linewidth}{@{}"
           + " ".join(spec) + "@{}}", r"\toprule", row(*header, bold=True),
           r"\midrule", r"\endhead", r"\bottomrule", r"\endlastfoot"]
    out += [row(n, cells) for n, cells in body]
    out += [r"\end{xltabular}", r"\end{small}", ""]
    return out


FIGURES = {"thread-states.svg": "thread-states"}


def thread_states_tikz():
    """The thread-state diagram, redrawn from thread-states.svg in TikZ.

    Coordinates are the SVG's (pixels, y down) so the two stay comparable.
    One pixel is 0.0072 in, shrunk if needed to fit the text width.
    """
    box = r"\draw[%s, rounded corners=3pt] (%d,%d) rectangle ++(130,48);"
    lab = r"\node[font=\footnotesize\bfseries\sffamily] at (%d,%d) {%s};"
    edge = r"\draw[->, >=Stealth, line width=0.6pt, draw=black!60] %s;"
    note = r"\node[font=\scriptsize\sffamily, text=black!70, anchor=%s] at (%d,%d) {%s};"
    out = [r"\begin{tikzpicture}[x={min(0.0072in, 0.00128\linewidth)},"
           r" y={-min(0.0072in, 0.00128\linewidth)}]",
           r"\draw[dashed, draw=black!60, rounded corners=3pt] (25,96) rectangle ++(110,48);",
           r"\node[font=\footnotesize\sffamily, text=black!70] at (80,120) {created};"]
    for x, y, name, run in [(205, 96, "READY", False), (475, 96, "RUNNING", True),
                            (340, 226, "BLOCKED", False), (340, 326, "SLEEPING", False),
                            (595, 326, "ZOMBIE", False)]:
        style = "draw=black, line width=1pt, fill=black!8" if run else "draw=black!60"
        out.append(box % (style, x, y))
        out.append(lab % (x + 65, y + 24, name))
    for path in ["(135,120) -- (205,120)", "(335,108) -- (475,108)",
                 "(475,132) -- (335,132)", "(520,144) |- (470,250)",
                 "(575,144) |- (470,350)", "(605,120) -| (680,326)",
                 "(340,250) -| (300,144)", "(340,350) -| (240,144)"]:
        out.append(edge % path)
    for anchor, x, y, text in [
            ("base", 405, 100, "picked by scheduler"),
            ("base", 405, 152, "slice ends or yield"),
            ("base east", 512, 200, "waits on a lock"),
            ("base west", 583, 200, r"\texttt{sys\_sleep()}"),
            ("base west", 688, 200, "returns"),
            ("base west", 308, 200, "woken"),
            ("base east", 232, 200, "time is up"),
            ("base", 660, 394, "freed by the scheduler")]:
        out.append(note % (anchor, x, y, text))
    out.append(r"\end{tikzpicture}")
    return out


def render_figure(line):
    m = re.match(r"!\[([^\]]*)\]\(([^)]+)\)", line)
    alt, src = m.groups()
    if src not in FIGURES:
        sys.exit(f"no LaTeX version of figure {src}; add one to FIGURES")
    caption = alt.split(":")[0]
    return ([r"\begin{figure}[htbp]", r"\centering"] + thread_states_tikz()
            + [r"\caption{" + md_inline(caption) + "}", r"\end{figure}", ""])


def render_body(lines, marks_for):
    """Render the lines of one section. marks_for(n) returns \\index cmds."""
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        if line.startswith("```"):
            j = i + 1
            while not lines[j].startswith("```"):
                j += 1
            pre = "".join(marks_for(k) for k in range(i, j + 1))
            if pre:
                out.append(pre + "%")
            out.append(r"\begin{Verbatim}")
            out += lines[i + 1:j]
            out += [r"\end{Verbatim}", ""]
            i = j + 1
            continue
        if line.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append((i, split_row(lines[i])))
                i += 1
            out += render_table(rows, marks_for)
            continue
        if line.startswith("!["):
            out += render_figure(line)
            i += 1
            continue
        if ITEM_RX.match(line):
            stack = []
            while i < len(lines) and ITEM_RX.match(lines[i]):
                ind, mark, text = ITEM_RX.match(lines[i]).groups()
                kind = "itemize" if mark in "-*" else "enumerate"
                while stack and len(ind) < stack[-1][0]:
                    out.append(r"\end{%s}" % stack.pop()[1])
                if stack and len(ind) == stack[-1][0] and kind != stack[-1][1]:
                    out.append(r"\end{%s}" % stack.pop()[1])
                if not stack or len(ind) > stack[-1][0]:
                    opt = "[label=\\arabic*.]" if kind == "enumerate" else ""
                    out.append(r"\begin{%s}%s" % (kind, opt))
                    stack.append((len(ind), kind))
                out.append(r"\item " + marks_for(i) + md_inline(text))
                i += 1
            while stack:
                out.append(r"\end{%s}" % stack.pop()[1])
            out.append("")
            continue
        para = []
        while (i < len(lines) and lines[i].strip() and not lines[i].startswith(("```", "|", "!["))
               and not ITEM_RX.match(lines[i])):
            para.append(marks_for(i) + md_inline(lines[i].strip()))
            i += 1
        out += [r"\leavevmode" + " ".join(para) if para[0].startswith(r"\index")
                else " ".join(para), ""]
    return out


# ---------------------------------------------------------------------------
# Document
# ---------------------------------------------------------------------------

PREAMBLE = r"""\documentclass[%(fontsize)s, twoside, openright]{book}
\usepackage[%(geometry)s]{geometry}
\usepackage{fontspec}
\setmainfont{TeX Gyre Pagella}
\setsansfont{TeX Gyre Heros}
\setmonofont{DejaVu Sans Mono}[Scale=0.82]
\usepackage{amssymb}
\usepackage{newunicodechar}
\newunicodechar{π}{\ensuremath{\pi}}
\newunicodechar{→}{\ensuremath{\rightarrow}}
\newunicodechar{⇄}{\ensuremath{\rightleftarrows}}
\usepackage{microtype}
\usepackage[dvipsnames]{xcolor}
\usepackage{booktabs, xltabular, array}
\newcolumntype{Y}{>{\raggedright\arraybackslash}X}
\setlength{\tabcolsep}{5pt}
\renewcommand{\arraystretch}{1.15}
\usepackage{enumitem}
\setlist{itemsep=2pt, topsep=4pt}
\usepackage{fvextra}
\fvset{fontsize=\small, breaklines=true, frame=leftline, framerule=0.8pt,
       rulecolor=\color{black!35}, xleftmargin=6pt, framesep=6pt}
\usepackage{tikz}
\usetikzlibrary{arrows.meta}
\usepackage[font=small, labelfont=bf]{caption}
\usepackage{titlesec}
\titleformat{\chapter}[display]{\normalfont\sffamily\bfseries\raggedright}
  {\large\MakeUppercase{\chaptertitlename}\ \thechapter}{8pt}{\Huge}
\titlespacing*{\chapter}{0pt}{30pt}{28pt}
\titleformat{\section}{\normalfont\sffamily\Large\bfseries}{\thesection}{0.8em}{}
\usepackage{fancyhdr}
\pagestyle{fancy}
\fancyhf{}
\fancyhead[LE]{\small\thepage\quad\nouppercase{\leftmark}}
\fancyhead[RO]{\small\nouppercase{\rightmark}\quad\thepage}
\renewcommand{\headrulewidth}{0.4pt}
\renewcommand{\chaptermark}[1]{\markboth{\ifnum\value{chapter}>0
  \ifnum\value{secnumdepth}>-1 \thechapter.\ \fi\fi #1}{}}
\renewcommand{\sectionmark}[1]{\markright{\thesection\ #1}}
\fancypagestyle{plain}{\fancyhf{}\fancyfoot[C]{\small\thepage}
  \renewcommand{\headrulewidth}{0pt}}
\usepackage{emptypage}
\newcommand{\code}[1]{\texttt{#1}}
\setlength{\emergencystretch}{2.5em}
\usepackage[noautomatic]{imakeidx}
\makeindex[intoc, columns=2, options=-s picoos.ist]
\usepackage[hidelinks, bookmarksnumbered]{hyperref}
\hypersetup{pdftitle={%(title)s}, pdfauthor={%(author)s}}
\setcounter{tocdepth}{1}
"""

INDEX_STYLE = r'''headings_flag 1
heading_prefix "\n\\indexspace\n\\textbf{\\sffamily "
heading_suffix "}\\nopagebreak\n"
delim_0 ", "
delim_1 ", "
'''


def title_page(title, author, version, url, when):
    return [
        r"\begin{titlepage}", r"\centering", r"\vspace*{0.25\textheight}",
        r"{\sffamily\bfseries\Huge " + tex_escape(title) + r"\par}",
        r"\vspace{1.5em}",
        r"{\Large An introduction to operating systems on the Raspberry Pi Pico\par}",
        r"\vfill", r"{\Large " + tex_escape(author) + r"\par}", r"\vspace{0.5em}",
        r"{\large Describes picoOS " + tex_escape(version) + r"\par}",
        r"\vspace{0.25\textheight}", r"\end{titlepage}",
        r"\thispagestyle{empty}", r"\vspace*{\fill}", r"\noindent",
        r"\textit{" + tex_escape(title) + r"}\\[2pt]",
        tex_escape(author) + r"\\[2pt]",
        r"Edition for picoOS " + tex_escape(version) + ", typeset " + when + r".\\[8pt]",
        (r"Online edition:\\{\small\url{" + url + "}}") if url else "",
        r"\cleardoublepage", "",
    ]


def convert(md):
    lines = md.rstrip("\n").split("\n")
    title = lines[0].lstrip("# ").strip()
    byline = lines[2] if len(lines) > 2 else ""
    author = byline.split(" · ")[0].strip()
    url_m = re.search(r"\((https?://[^)]+)\)", byline)
    ver_m = re.search(r"describes picoOS (v[\d.]+\d)", md)

    _, sections, _ = toc.number_sections(lines)
    plan = plan_index(sections)

    out = [r"\begin{document}", r"\frontmatter"]
    out += title_page(title, author, ver_m.group(1) if ver_m else "",
                      url_m.group(1) if url_m else "",
                      datetime.date.today().strftime("%B %Y"))
    out += [r"\tableofcontents", ""]

    for sid, stitle, chap, body in sections:
        marks = plan.get(sid, {})
        start = "".join(index_cmd(t) for t in marks.get("start", []))

        def marks_for(n, marks=marks):
            return "".join(index_cmd(t) for t in marks.get(n, []))

        if "." not in sid:                         # a chapter and its opening
            if chap == "P":
                out += [r"\chapter{Preface}", r"\markboth{Preface}{Preface}"]
            elif chap == "A":
                out += [r"\appendix",
                        heading("chapter", stitle.split("—", 1)[-1].strip())]
            else:
                if chap == "1":
                    out.append(r"\mainmatter")
                out.append(heading("chapter", stitle.split("—", 1)[-1].strip()))
        elif chap == "P":
            out += [r"\section*{" + md_inline(stitle) + "}", r"\phantomsection",
                    r"\addcontentsline{toc}{section}{" + md_inline(stitle) + "}"]
        else:
            out.append(heading("section", stitle))
        if start:
            out.append(start + "%")
        out += render_body(body, marks_for)

    out += [r"\backmatter", r"\printindex", r"\end{document}"]
    return title, author, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paper", choices=PAPERS, default="letter")
    ap.add_argument("--tex-only", action="store_true",
                    help="write the .tex file but do not compile it")
    args = ap.parse_args()

    md = toc.strip_generated(toc.BOOK.read_text())
    title, author, body = convert(md)
    pre = PREAMBLE % {"fontsize": BASE_FONT[args.paper], "geometry": PAPERS[args.paper],
                      "title": tex_escape(title), "author": tex_escape(author)}

    OUT_DIR.mkdir(exist_ok=True)
    tex = OUT_DIR / f"{STEM}.tex"
    tex.write_text(pre + "\n" + "\n".join(body) + "\n")
    (OUT_DIR / "picoos.ist").write_text(INDEX_STYLE)
    print(f"wrote {tex.relative_to(HERE.parent.parent)}")
    if args.tex_only:
        return

    for tool in ("lualatex", "makeindex"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found; install TeX Live (see docs/book/README.md)")

    def run(*cmd):
        r = subprocess.run(cmd, cwd=OUT_DIR, capture_output=True, text=True)
        if r.returncode != 0:
            log = (OUT_DIR / f"{STEM}.log")
            tail = log.read_text(errors="replace")[-3000:] if log.exists() else r.stdout[-3000:]
            sys.exit(f"{cmd[0]} failed:\n{tail}")

    latex = ("lualatex", "-interaction=nonstopmode", "-halt-on-error", tex.name)
    run(*latex)                                   # 1: write .toc and .idx
    run("makeindex", "-q", "-s", "picoos.ist", f"{STEM}.idx")
    run(*latex)                                   # 2: contents and index in place
    run("makeindex", "-q", "-s", "picoos.ist", f"{STEM}.idx")
    run(*latex)                                   # 3: page numbers settle

    suffix = "" if args.paper == "letter" else f"-{args.paper}"
    pdf = HERE / f"{STEM}{suffix}.pdf"
    shutil.copyfile(OUT_DIR / f"{STEM}.pdf", pdf)
    log = (OUT_DIR / f"{STEM}.log").read_text(errors="replace")
    missing = sorted(set(re.findall(r"Missing character: There is no (\S+)", log)))
    pages = re.search(r"Output written on .*?\((\d+) pages", log)
    print(f"wrote {pdf.relative_to(HERE.parent.parent)}"
          + (f" ({pages.group(1)} pages)" if pages else ""))
    if missing:
        print("warning: glyphs missing from the fonts:", " ".join(missing))


if __name__ == "__main__":
    main()
