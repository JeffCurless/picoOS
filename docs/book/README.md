# Learning Operating Systems with picoOS (book)

| File | What it is |
| --- | --- |
| `learning-operating-systems-with-picoos.md` | The book: preface, 16 chapters, appendix, contents and index |
| `thread-states.svg` | Thread-state diagram used in section 4.5 |
| `build_toc_index.py` | Numbers the sections and rebuilds the contents and index |
| `build_pdf.py` | Typesets the book as a print-ready PDF with LaTeX (contents and index with page numbers) |
| `learning-operating-systems-with-picoos.pdf` | The typeset book, US Letter (`-a4.pdf` and `-6x9.pdf` are the other paper sizes) |
| `latex/` | Intermediate LaTeX files from `build_pdf.py`; ignored by git and safe to delete |
| `book-outline.md`, `investigation-notes.md` | Earlier planning notes; not part of the book |

The book is a local copy of the [online edition](https://claude.ai/code/artifact/dac58cfd-0013-4df3-931a-9dfa92107735). Edits made in one are not copied to the other automatically.

## Regenerating after edits

The section numbers, the Contents list and the Index are generated. After any edit to the book, run:

```bash
python3 docs/book/build_toc_index.py
```

The script:

- Removes its own section numbers, anchors, Contents and Index, then regenerates them. Running it again gives identical output.
- Numbers each `###` section `<chapter>.<n>`: `P.<n>` for the preface and `A.<n>` for the appendix. A new or moved section is renumbered automatically.
- Cites section numbers in the Contents and Index rather than page numbers, so both stay correct in any print layout. A bare chapter number such as `15` in the index means that chapter's opening paragraphs. (The PDF built by `build_pdf.py` has a contents and index with real page numbers.)

Rules for editing the book:

- Write headings without numbers: `## Chapter N — Title` for chapters, `### Title` for sections. The script adds the numbers.
- Edit the text between the generated parts only. Do not hand-edit the Contents (between `<!-- toc:start -->` and `<!-- toc:end -->`), the Index (everything after `<!-- index:start -->`), or the `<a id="…"></a>` anchor lines. The next run replaces them.
- Index terms live in `INDEX_TERMS` at the top of the script. Each entry is a label, a regex searched in each section, or a fixed list of section ids for words that appear almost everywhere.
- Run `python3 docs/book/build_toc_index.py --report` to see where each term is found and which terms are too broad. This mode does not change the book.
- If a fixed list names a section that no longer exists, the script stops and names the term. Update that list after renumbering.

## Making the PDF

`build_pdf.py` turns the Markdown book into a PDF for reading or printing. The PDF has:

- a title page and an edition page;
- a table of contents with page numbers (front matter in roman numerals, chapters from page 1);
- an index with page numbers, built from the same `INDEX_TERMS` as the Markdown index;
- running heads (chapter on left pages, section on right), and wider inside margins for binding;
- each chapter starting on a right-hand page.

### Requirements

TeX Live with LuaLaTeX and makeindex. On Debian, Ubuntu or Raspberry Pi OS:

```bash
sudo apt install texlive-luatex texlive-latex-recommended texlive-latex-extra \
    texlive-pictures fonts-texgyre fonts-dejavu-mono
```

The fonts used are TeX Gyre Pagella (text), TeX Gyre Heros (headings) and DejaVu Sans Mono (code), from the `fonts-texgyre` and `fonts-dejavu-mono` packages. pandoc and Inkscape are **not** needed: the script converts the Markdown itself, and the thread-state diagram is redrawn in TikZ inside `build_pdf.py`.

### Build it

From the repository root:

```bash
python3 docs/book/build_toc_index.py          # 1. after editing: refresh numbering and Markdown index
python3 docs/book/build_pdf.py                # 2. US Letter -> docs/book/learning-operating-systems-with-picoos.pdf
python3 docs/book/build_pdf.py --paper a4     #    A4       -> ...-a4.pdf
python3 docs/book/build_pdf.py --paper 6x9    #    6 x 9 in trade paperback -> ...-6x9.pdf
python3 docs/book/build_pdf.py --tex-only     #    write latex/*.tex only, without compiling
```

A build takes about 20 seconds. It runs LuaLaTeX three times with makeindex in between, so the page numbers in the contents and index are final. It prints the page count, and warns if any character is missing from the fonts. If LaTeX fails, the script prints the end of `latex/learning-operating-systems-with-picoos.log`.

The script builds the PDF from the text, so always run `build_toc_index.py` first after an edit. Section numbers in the PDF then match the Markdown copy (for example, 4.5 is "Thread states" in both).

### Printing and binding

- **Print double-sided.** The layout is two-sided: inside and outside margins alternate and chapters open on right-hand pages, with blank left pages where needed. In the print dialog choose *two-sided, flip on long edge* and *actual size* (100%), not *fit to page*. Fitting to the page would shift the binding margin.
- **Pick the paper size before building.** Use `letter` for a US print shop or office printer, `a4` elsewhere, and `6x9` for a print-on-demand paperback. Most such services accept a 6 × 9 in PDF directly.
- **Page count.** Some binders want a multiple of 4 pages; check the count printed by the build (or `pdfinfo <file>.pdf`) and ask the printer whether they pad it.
- **Links.** Links in the PDF are clickable but not coloured, so the printed page shows plain text.
- **Spiral or comb binding** works with the default margins. For perfect (glued) binding of a thick copy, widen `inner=` in `PAPERS` at the top of `build_pdf.py`.

### Changing the book's look

Fonts, margins, heading styles and running heads are in `PREAMBLE` in `build_pdf.py`, and paper sizes are in `PAPERS`. A new figure in the Markdown (`![alt](file.svg)`) needs a LaTeX version: add it to `FIGURES` in `build_pdf.py`, or the build stops and says so.
