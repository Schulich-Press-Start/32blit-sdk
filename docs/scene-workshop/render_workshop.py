"""Build the offline guide: python render_workshop.py <extracted-workshop-folder>

Authoring only: python -m pip install markdown-it-py==4.2.0 Pygments==2.21.0
Readers need only START HERE.html and its neighboring kit files.
"""
from html import escape
from itertools import groupby
from pathlib import Path
import re
import sys

from markdown_it import MarkdownIt
from pygments.lexers import get_lexer_by_name
from pygments.token import Comment, Keyword, Name, Number, Operator, String


LANGUAGES = {"cpp": "C++", "bash": "Bash", "powershell": "PowerShell"}
# Order matters: Pygments puts #include in Comment.Preproc, not Keyword.
STYLES = (
    (Comment.PreprocFile, "string"), (Comment.Preproc, "directive"),
    (Comment, "comment"), (Keyword, "keyword"), (String, "string"),
    (Number, "number"), (Name.Variable, "variable"), (Name.Builtin, "function"),
    (Name.Function, "function"), (Name.Class, "keyword"), (Operator, "operator"),
)


def render_fence(renderer, tokens, index, options, env):
    fence = tokens[index]
    if not fence.content.strip():
        raise ValueError(f"Scene Workshop.md:{fence.map[0] + 1}: empty fenced code block")
    lang = fence.info.partition(" ")[0]
    label = LANGUAGES.get(lang, lang or "Code")
    if lang in LANGUAGES:
        # Raw lexer tokens preserve tabs, leading/trailing whitespace and final newline.
        stream = get_lexer_by_name(lang).get_tokens_unprocessed(fence.content)
        def style(token):
            return next((cls for kind, cls in STYLES if token[1] in kind), None)
        parts = []
        for cls, run in groupby(stream, key=style):
            text = escape("".join(value for _, _, value in run), quote=False)
            parts.append(f'<span class="{cls}">{text}</span>' if cls else text)
        source = "".join(parts)
    else:
        source = escape(fence.content, quote=False)
    return (f'<figure class="snippet"><figcaption>{escape(label)}</figcaption>'
            f'<pre tabindex="0"><code class="language-{escape(lang)}">{source}</code></pre></figure>\n')


LIGHT = """color-scheme: light; --page: #ffffff; --text: #192c3a; --panel: #f0f4f7;
    --border: #a1adb8; --link: #005580; --focus: #884600; --comment: #30613d;
    --keyword: #592681; --string: #8b340d; --number: #83305f; --variable: #07517a;
    --function: #005a60; --directive: #744207; --operator: #44556b;"""
DARK = """color-scheme: dark; --page: #151d27; --text: #edf2f7; --panel: #1e2b39;
    --border: #718498; --link: #91d4ff; --focus: #ffd378; --comment: #9ad19f;
    --keyword: #d5b8ff; --string: #ffb38f; --number: #ffb2d8; --variable: #9bd6ff;
    --function: #85ded9; --directive: #ffd378; --operator: #bac8db;"""
PALETTES = (f":root {{ {LIGHT} }}\n:root[data-theme=dark] {{ {DARK} }}\n"
            f"@media (prefers-color-scheme: dark) {{ :root:not([data-theme]) {{ {DARK} }} }}")
CSS = """
* { box-sizing: border-box; }
body { max-width: 58rem; margin: 0 auto; padding: 1rem clamp(.85rem, 3vw, 1.75rem) 3rem;
    font: 1.0625rem/1.6 system-ui, sans-serif; background: var(--page); color: var(--text); }
h1, h2, h3 { line-height: 1.22; }
h2 { margin: 2.25rem 0 1rem; padding-top: 1rem; border-top: 2px solid var(--border); }
h3 { margin-top: 1.75rem; }
a { color: var(--link); text-underline-offset: .14em; }
:focus-visible { outline: 3px solid var(--focus); outline-offset: 3px; }
.toolbar { display: flex; align-items: center; justify-content: space-between; gap: 1rem; flex-wrap: wrap; }
select { font: inherit; color: var(--text); background: var(--panel); border: 1px solid var(--border);
    padding: .3em .5em; margin-inline-start: .25em; border-radius: .3em; }
nav { background: var(--panel); border-inline-start: .25rem solid var(--link); padding: .7rem 1.1rem; }
nav ul { margin: .2rem 0; padding-inline-start: 1.35rem; }
img { display: block; max-width: 100%; max-height: 32rem; height: auto; }
table { border-collapse: collapse; }
th, td { padding: .4rem .7rem; border: 1px solid var(--border); text-align: start; vertical-align: top; }
th, :not(pre) > code { background: var(--panel); }
code { font: .91em/1.5 ui-monospace, Consolas, monospace; }
:not(pre) > code { padding: .1em .22em; overflow-wrap: anywhere; border-radius: .2em; }
.snippet { margin: 1rem 0; background: var(--panel); border: 1px solid var(--border); border-radius: .35rem; }
.snippet figcaption { border-bottom: 1px solid var(--border); padding: .2rem 1rem; font-size: .85em; font-weight: 650; }
pre { margin: 0; padding: .8rem 1rem; overflow: auto; tab-size: 4; }
.comment { color: var(--comment); font-style: italic; }
.keyword { color: var(--keyword); font-weight: 600; } .directive { color: var(--directive); }
.string { color: var(--string); } .number { color: var(--number); } .variable { color: var(--variable); }
.function { color: var(--function); } .operator { color: var(--operator); }
"""
SCRIPT = """(() => {
  const root = document.documentElement;
  const key = '32blit.pongWorkshop.theme';
  try {
    const saved = localStorage.getItem(key);
    if (saved === 'light' || saved === 'dark') root.dataset.theme = saved;
  } catch (_) { /* Storage may be unavailable for file URLs. */ }
  document.addEventListener('DOMContentLoaded', () => {
    const select = document.getElementById('theme');
    select.value = root.dataset.theme || 'system';
    select.addEventListener('change', () => {
      if (select.value === 'system') delete root.dataset.theme;
      else root.dataset.theme = select.value;
      try {
        if (select.value === 'system') localStorage.removeItem(key);
        else localStorage.setItem(key, select.value);
      } catch (_) { /* The current page still honors the selection. */ }
    });
    select.parentElement.hidden = false;
  });
})();"""


def render(markdown):
    md = MarkdownIt("commonmark", {"html": False}).enable("table")
    md.add_render_rule("fence", render_fence)
    tokens = md.parse(markdown)
    jumps = []
    seen = set()
    first_section = None
    for i, t in enumerate(tokens):
        if t.type != "heading_open" or t.tag == "h1":
            continue
        title = tokens[i + 1].content
        slug = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
        anchor = slug
        n = 2
        while anchor in seen:
            anchor = f"{slug}-{n}"
            n += 1
        seen.add(anchor)
        t.attrSet("id", anchor)
        if t.tag == "h2":
            if first_section is None:
                first_section = i
            jumps.append(f'<li><a href="#{anchor}">{escape(title)}</a></li>')
    nav = '<nav id="workshop-contents" aria-label="Workshop contents"><strong>Jump to</strong><ul>\n'
    nav += "\n".join(jumps) + "\n</ul></nav>\n"
    intro = md.renderer.render(tokens[:first_section], md.options, {})
    sections = md.renderer.render(tokens[first_section:], md.options, {}) if first_section is not None else ""
    return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Scene Workshop — from animated coins to playable Pong</title>
<style>{PALETTES}{CSS}</style>
<script>{SCRIPT}</script></head><body>
<header class="toolbar"><a href="#workshop-contents">Workshop contents</a>
<label hidden>Page theme <select id="theme"><option value="system">Use system setting</option><option value="light">Light</option><option value="dark">Dark</option></select></label></header>
<main>{intro}{nav}{sections}</main></body></html>
'''


if __name__ == "__main__":
    from argparse import ArgumentParser
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("workshop", type=Path, help="Kit directory with Checkpoints and docs/images")
    root = parser.parse_args().workshop
    source = Path(__file__).resolve().parent
    intro = """# Scene Workshop: from animated coins to playable Pong

Open **`Editor/32blit-editor.exe` → File → New Project…**. In the same project, you'll import artwork, animate two coins over a tilemap, write Pong's rules in seven C++ checkpoints, and build a standalone Windows game.
"""
    chapters = [intro]
    for chapter in ("Authoring", "Building", "Gameplay"):
        content = (source / (chapter + ".md")).read_text(encoding="utf-8-sig")
        # Place the three standalone chapter headings under a single guide title.
        content = re.sub(r"^(#+) ", r"#\1 ", content, flags=re.MULTILINE)
        chapters.append(content.replace("](images/", "](docs/images/"))
    markdown = "\n\n".join(chapters)
    try:
        page = render(markdown)
    except ValueError as error:
        sys.exit(str(error))
    (root / "Scene Workshop.md").write_text(markdown, encoding="utf-8", newline="\n")
    (root / "START HERE.html").write_text(page, encoding="utf-8", newline="\n")
