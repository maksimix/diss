# -*- coding: utf-8 -*-
"""Собирает веб-страницу с математикой расчёта из Методы_и_формулы.md.

Markdown остаётся единственным источником: страница и PDF описывают одно и то же,
и расходиться им негде. Формулы набираются MathJax'ом прямо в браузере.

Запуск:  python docs/make_math_page.py
Результат: docs/Математика_расчёта.html
"""
import html
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "Методы_и_формулы.md")
TARGET = os.path.join(HERE, "Математика_расчёта.html")

# Формулы вынимаются из текста до разбора Markdown и возвращаются после: иначе
# подчёркивания индексов (k_x, f_c) Markdown принял бы за курсив и порвал бы
# формулу пополам.
MATH_TOKEN = "\x01MATH%d\x01"
# Тот же вид, но для поиска заполнителя целой строкой.
MATH_TOKEN_RE = "\x01MATH\\d+\x01"


def extract_math(text, store):
    def take(match):
        store.append(match.group(0))
        return MATH_TOKEN % (len(store) - 1)

    text = re.sub(r"\$\$.*?\$\$", take, text, flags=re.DOTALL)
    text = re.sub(r"(?<!\$)\$[^$\n]+\$(?!\$)", take, text)
    return text


def restore_math(text, store):
    for index, formula in enumerate(store):
        # Формула уходит в HTML как есть: MathJax разбирает её сам, поэтому
        # экранируются только угловые скобки, ломающие разметку.
        safe = formula.replace("<", "&lt;").replace(">", "&gt;")
        text = text.replace(MATH_TOKEN % index, safe)
    return text


def inline_markup(text):
    """Жирный, курсив, код и ссылки внутри абзаца."""
    text = html.escape(text, quote=False)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', text)
    return text


def slugify(title, used):
    slug = re.sub(r"[^\w\-]+", "-", title.lower(), flags=re.UNICODE).strip("-")
    slug = slug or "section"
    candidate = slug
    number = 2
    while candidate in used:
        candidate = "%s-%d" % (slug, number)
        number += 1
    used.add(candidate)
    return candidate


def convert(markdown):
    math_store = []
    markdown = extract_math(markdown, math_store)

    lines = markdown.split("\n")
    out = []
    toc = []
    used_slugs = set()
    index = 0

    while index < len(lines):
        line = lines[index]
        stripped = line.strip()

        if not stripped:
            index += 1
            continue

        # Горизонтальная черта.
        if re.fullmatch(r"-{3,}", stripped):
            out.append("<hr>")
            index += 1
            continue

        # Заголовок: попутно собирается оглавление.
        heading = re.match(r"^(#{1,6})\s+(.*)$", stripped)
        if heading:
            level = len(heading.group(1))
            title = heading.group(2).strip()
            slug = slugify(re.sub(r"[$\\{}]", "", title), used_slugs)
            out.append('<h%d id="%s">%s</h%d>' % (level, slug, inline_markup(title), level))
            if level in (1, 2, 3):
                toc.append((level, slug, title))
            index += 1
            continue

        # Блочная формула, оставшаяся отдельной строкой-заполнителем.
        if re.fullmatch(MATH_TOKEN_RE, stripped):
            out.append('<div class="formula">%s</div>' % stripped)
            index += 1
            continue

        # Таблица: строка заголовка, разделитель, дальше данные.
        if stripped.startswith("|") and index + 1 < len(lines) and \
                re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[index + 1]):
            header = [cell.strip() for cell in stripped.strip("|").split("|")]
            out.append("<table><thead><tr>")
            out.extend("<th>%s</th>" % inline_markup(cell) for cell in header)
            out.append("</tr></thead><tbody>")
            index += 2
            while index < len(lines) and lines[index].strip().startswith("|"):
                cells = [cell.strip() for cell in lines[index].strip().strip("|").split("|")]
                out.append("<tr>")
                out.extend("<td>%s</td>" % inline_markup(cell) for cell in cells)
                out.append("</tr>")
                index += 1
            out.append("</tbody></table>")
            continue

        # Список.
        if re.match(r"^[-*+]\s+", stripped) or re.match(r"^\d+[.)]\s+", stripped):
            ordered = bool(re.match(r"^\d+[.)]\s+", stripped))
            tag = "ol" if ordered else "ul"
            out.append("<%s>" % tag)
            while index < len(lines):
                item = lines[index].strip()
                match = re.match(r"^(?:[-*+]|\d+[.)])\s+(.*)$", item)
                if not match:
                    break
                text = match.group(1)
                index += 1
                # Продолжение пункта: строки с отступом относятся к нему же.
                while index < len(lines) and lines[index].startswith(("  ", "\t")) and \
                        lines[index].strip() and \
                        not re.match(r"^\s*(?:[-*+]|\d+[.)])\s+", lines[index]):
                    text += " " + lines[index].strip()
                    index += 1
                out.append("<li>%s</li>" % inline_markup(text))
            out.append("</%s>" % tag)
            continue

        # Обычный абзац: склеивается до пустой строки.
        paragraph = [stripped]
        index += 1
        while index < len(lines) and lines[index].strip() and \
                not lines[index].strip().startswith(("#", "|", "-", "*")) and \
                not re.fullmatch(MATH_TOKEN_RE, lines[index].strip()):
            paragraph.append(lines[index].strip())
            index += 1
        out.append("<p>%s</p>" % inline_markup(" ".join(paragraph)))

    body = "\n".join(out)
    body = restore_math(body, math_store)

    toc_html = ["<nav class=\"toc\"><div class=\"toc-title\">Содержание</div><ul>"]
    for level, slug, title in toc:
        toc_html.append('<li class="lvl%d"><a href="#%s">%s</a></li>'
                        % (level, slug, html.escape(title, quote=False)))
    toc_html.append("</ul></nav>")
    return body, "\n".join(toc_html)


PAGE = """<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Математика расчёта — EM Waveguide Studio</title>
<script>
window.MathJax = {
  tex: {inlineMath: [['$', '$']], displayMath: [['$$', '$$']]},
  options: {skipHtmlTags: ['script', 'noscript', 'style', 'textarea', 'pre', 'code']},
  startup: {pageReady: function () {
    return MathJax.startup.defaultPageReady().then(function () {
      var note = document.getElementById('offline-note');
      if (note) { note.style.display = 'none'; }
    });
  }}
};
</script>
<script id="MathJax-script" async src="https://cdn.jsdelivr.net/npm/mathjax@3/es5/tex-mml-chtml.js"></script>
<style>
:root { --blue: #1b5b96; --ink: #24303a; --muted: #6b7680; --line: #dde3e9; }
* { box-sizing: border-box; }
body { margin: 0; background: #f0f2f4; color: var(--ink);
       font-family: "Segoe UI", Roboto, Arial, sans-serif; font-size: 16px; line-height: 1.62; }
header { background: var(--blue); color: #fff; padding: 26px 32px; }
header h1 { margin: 0 0 4px; font-size: 25px; font-weight: 600; }
header .sub { color: #cfe0ef; font-size: 14px; }
.wrap { max-width: 1220px; margin: 0 auto; display: flex; gap: 26px;
        align-items: flex-start; padding: 24px 20px 60px; }
main { flex: 1; min-width: 0; background: #fff; border: 1px solid var(--line);
       border-radius: 6px; padding: 26px 34px 40px; overflow-x: auto; }
.toc { position: sticky; top: 18px; width: 288px; flex: 0 0 288px; background: #fff;
       border: 1px solid var(--line); border-radius: 6px; padding: 14px 16px;
       max-height: calc(100vh - 40px); overflow-y: auto; }
.toc-title { color: var(--blue); font-weight: 600; margin-bottom: 8px; }
.toc ul { list-style: none; margin: 0; padding: 0; font-size: 14px; }
.toc li { margin: 2px 0; }
.toc li.lvl1 { font-weight: 600; margin-top: 8px; }
.toc li.lvl3 { padding-left: 14px; color: var(--muted); }
.toc a { color: var(--ink); text-decoration: none; }
.toc a:hover { color: var(--blue); text-decoration: underline; }
h1, h2, h3, h4 { color: var(--blue); line-height: 1.25; scroll-margin-top: 16px; }
h1 { font-size: 24px; margin: 26px 0 12px; }
h2 { font-size: 20px; margin: 30px 0 10px; padding-bottom: 6px; border-bottom: 1px solid var(--line); }
h3 { font-size: 17px; margin: 22px 0 8px; }
p { margin: 10px 0; }
code { background: #f3f5f7; border: 1px solid #e4e8ec; border-radius: 3px;
       padding: 1px 5px; font-family: Consolas, "Courier New", monospace; font-size: 92%; }
hr { border: none; border-top: 1px solid var(--line); margin: 26px 0; }
table { border-collapse: collapse; margin: 14px 0; width: 100%; font-size: 15px; }
th, td { border: 1px solid var(--line); padding: 7px 10px; text-align: left; vertical-align: top; }
th { background: #eef2f6; color: #40525f; }
tbody tr:nth-child(even) { background: #f8fafb; }
.formula { margin: 14px 0; padding: 10px 14px; background: #f8fafb;
           border-left: 3px solid var(--blue); border-radius: 0 4px 4px 0; overflow-x: auto; }
#offline-note { margin: 0 0 16px; padding: 10px 14px; background: #fff6e0;
                border: 1px solid #e8d08a; border-radius: 4px; color: #6b5a20; font-size: 14px; }
@media (max-width: 980px) { .wrap { display: block; } .toc { position: static; width: auto; margin-bottom: 18px; } }
</style>
</head>
<body>
<header>
  <h1>Математика расчёта</h1>
  <div class="sub">EM Waveguide Studio · формулы соответствуют коду один в один</div>
</header>
<div class="wrap">
__TOC__
<main>
<p id="offline-note">Формулы набираются библиотекой MathJax из интернета. Без сети они
останутся в исходной записи TeX — смысл сохраняется, вид проще.</p>
__BODY__
</main>
</div>
</body>
</html>
"""


def main():
    if not os.path.exists(SOURCE):
        sys.stderr.write("Не найден исходный документ: %s\n" % SOURCE)
        return 1
    with open(SOURCE, encoding="utf-8") as handle:
        markdown = handle.read()

    body, toc = convert(markdown)
    # Заголовок страницы задан в шаблоне, первый H1 документа дублировал бы его.
    body = re.sub(r"^<h1 [^>]*>.*?</h1>\s*", "", body, count=1, flags=re.DOTALL)

    page = PAGE.replace("__TOC__", toc).replace("__BODY__", body)
    with open(TARGET, "w", encoding="utf-8") as handle:
        handle.write(page)
    print("Готово: %s (%d КБ)" % (TARGET, len(page.encode("utf-8")) // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
