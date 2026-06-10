#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_report_pdf.py  --  render report/report.md to report/report.pdf
=====================================================================

Pure-Python (ReportLab) Markdown->PDF for a focused subset of Markdown
(headings, paragraphs, **bold**, `code`, fenced code blocks, > quotes,
pipe tables, images, ordered/unordered lists, --- rules).

Chinese is rendered with ReportLab's built-in CID font 'STSong-Light', so no
system font installation is required.

Usage:  python scripts/build_report_pdf.py [report/report.md] [report/report.pdf]
"""

import html
import os
import re
import sys

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, Table,
                                TableStyle, Preformatted, Image, HRFlowable,
                                ListFlowable, ListItem)

CJK = "STSong-Light"
pdfmetrics.registerFont(UnicodeCIDFont(CJK))

DOC_WIDTH = A4[0] - 36 * mm  # usable width with our margins

def style(name, size, leading=None, bold=False, **kw):
    return ParagraphStyle(name, fontName=CJK, fontSize=size,
                          leading=leading or size * 1.4, **kw)

S = {
    "h1": style("h1", 20, bold=True, spaceBefore=10, spaceAfter=8, textColor=colors.HexColor("#1b264f")),
    "h2": style("h2", 15, spaceBefore=10, spaceAfter=6, textColor=colors.HexColor("#1b264f")),
    "h3": style("h3", 12.5, spaceBefore=8, spaceAfter=4, textColor=colors.HexColor("#3a3a5a")),
    "body": style("body", 9.7, leading=15),
    "quote": style("quote", 9.2, leading=14, leftIndent=10, textColor=colors.HexColor("#555555"),
                   backColor=colors.HexColor("#f3f4f8"), borderPadding=5),
    "cell": style("cell", 8.3, leading=11.5),
    "cellh": style("cellh", 8.3, leading=11.5, textColor=colors.white),
}
CODE = ParagraphStyle("code", fontName="Courier", fontSize=8, leading=10.5,
                      backColor=colors.HexColor("#f5f5f5"), borderPadding=5,
                      textColor=colors.HexColor("#202020"))


def inline(text):
    """Markdown inline -> ReportLab mini-HTML (XML-escaped)."""
    text = html.escape(text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"`([^`]+?)`", r'<font face="Courier">\1</font>', text)
    return text


def make_table(rows, base_dir):
    cells = []
    header = rows[0]
    body = rows[2:] if len(rows) > 1 and set(rows[1].replace("|", "").strip()) <= set("-: ") else rows[1:]
    def cols(line):
        parts = [c.strip() for c in line.strip().strip("|").split("|")]
        return parts
    hdr = cols(header)
    data = [[Paragraph(inline(c), S["cellh"]) for c in hdr]]
    for line in body:
        data.append([Paragraph(inline(c), S["cell"]) for c in cols(line)])
    ncol = len(hdr)
    cw = DOC_WIDTH / ncol
    t = Table(data, colWidths=[cw] * ncol, repeatRows=1)
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#3a7d44")),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#bbbbbb")),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f4f7f4")]),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("TOPPADDING", (0, 0), (-1, -1), 3),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
    ]))
    return t


def make_image(path, base_dir):
    full = path if os.path.isabs(path) else os.path.join(base_dir, path)
    if not os.path.isfile(full):
        return Paragraph(inline(f"[missing image: {path}]"), S["body"])
    from reportlab.lib.utils import ImageReader
    iw, ih = ImageReader(full).getSize()
    w = min(DOC_WIDTH, iw)
    return Image(full, width=w, height=w * ih / iw)


def parse(md, base_dir):
    flow = []
    lines = md.splitlines()
    i, n = 0, len(lines)
    para = []

    def flush_para():
        if para:
            flow.append(Paragraph(inline(" ".join(para)), S["body"]))
            para.clear()

    while i < n:
        line = lines[i]
        stripped = line.strip()

        # fenced code
        if stripped.startswith("```"):
            flush_para()
            i += 1
            buf = []
            while i < n and not lines[i].strip().startswith("```"):
                buf.append(lines[i]); i += 1
            i += 1
            flow.append(Preformatted("\n".join(buf), CODE))
            flow.append(Spacer(1, 4))
            continue

        # table block
        if stripped.startswith("|"):
            flush_para()
            tb = []
            while i < n and lines[i].strip().startswith("|"):
                tb.append(lines[i]); i += 1
            flow.append(Spacer(1, 2))
            flow.append(make_table(tb, base_dir))
            flow.append(Spacer(1, 6))
            continue

        # image (own line)
        m = re.match(r"!\[(.*?)\]\((.*?)\)", stripped)
        if m:
            flush_para()
            flow.append(Spacer(1, 4))
            flow.append(make_image(m.group(2), base_dir))
            flow.append(Spacer(1, 6))
            i += 1
            continue

        if not stripped:
            flush_para()
            i += 1
            continue

        if stripped.startswith("### "):
            flush_para(); flow.append(Paragraph(inline(stripped[4:]), S["h3"])); i += 1; continue
        if stripped.startswith("## "):
            flush_para(); flow.append(Paragraph(inline(stripped[3:]), S["h2"])); i += 1; continue
        if stripped.startswith("# "):
            flush_para(); flow.append(Paragraph(inline(stripped[2:]), S["h1"])); i += 1; continue

        if stripped == "---":
            flush_para()
            flow.append(Spacer(1, 4))
            flow.append(HRFlowable(width="100%", color=colors.HexColor("#cccccc")))
            flow.append(Spacer(1, 4))
            i += 1; continue

        if stripped.startswith("> "):
            flush_para()
            q = []
            while i < n and lines[i].strip().startswith(">"):
                q.append(lines[i].strip().lstrip(">").strip()); i += 1
            flow.append(Paragraph(inline(" ".join(q)), S["quote"]))
            flow.append(Spacer(1, 4))
            continue

        # lists (with multi-line item continuation)
        if re.match(r"^(\-|\*|\d+\.)\s+", stripped):
            flush_para()
            ordered = bool(re.match(r"^\d+\.", stripped))
            item_texts, cur = [], None
            while i < n:
                ls = lines[i].strip()
                if re.match(r"^(\-|\*|\d+\.)\s+", ls):
                    if cur is not None:
                        item_texts.append(cur)
                    cur = re.sub(r"^(\-|\*|\d+\.)\s+", "", ls)
                elif ls == "" or ls[0] in "#|>" or ls.startswith("```") or ls == "---":
                    break
                else:
                    cur = (cur + " " + ls) if cur else ls
                i += 1
            if cur is not None:
                item_texts.append(cur)
            items = [ListItem(Paragraph(inline(t), S["body"]), leftIndent=12)
                     for t in item_texts]
            flow.append(ListFlowable(items, bulletType="1" if ordered else "bullet",
                                     start="1" if ordered else None))
            flow.append(Spacer(1, 4))
            continue

        para.append(stripped)
        i += 1

    flush_para()
    return flow


def main():
    md_path = sys.argv[1] if len(sys.argv) > 1 else "report/report.md"
    pdf_path = sys.argv[2] if len(sys.argv) > 2 else "report/report.pdf"
    base_dir = os.path.dirname(os.path.abspath(md_path))
    md = open(md_path, encoding="utf-8").read()

    doc = SimpleDocTemplate(pdf_path, pagesize=A4,
                            leftMargin=18 * mm, rightMargin=18 * mm,
                            topMargin=16 * mm, bottomMargin=16 * mm,
                            title="HeteroSTA3D 3D-STA Report")
    doc.build(parse(md, base_dir))
    print(f"wrote {pdf_path}  ({os.path.getsize(pdf_path)//1024} KB)")


if __name__ == "__main__":
    main()
