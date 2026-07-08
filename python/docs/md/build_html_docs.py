"""Render docs/*.md into a themed static HTML site under docs/html/.

Usage: python build_html_docs.py
"""
from __future__ import annotations

import re
from pathlib import Path

import markdown
from pygments.formatters import HtmlFormatter

DOCS_DIR = Path(__file__).resolve().parent          # python/docs/md/
OUT_DIR = DOCS_DIR.parent / "html"                    # python/docs/html/ (仓库正式输出目录)
ASSETS_DIR = OUT_DIR / "assets"

# (filename, nav label, short description shown on the index page)
PAGES = [
    ("ACT_PLATFORM.md", "平台总览", "架构、事件总线、数据模式、触发器、插件生命周期"),
    ("PLUGIN_SDK.md", "插件开发指南", "PluginContext 完整 API、生命周期、打包分发"),
    ("ACT_UI_PARITY_SDK.md", "声明式 UI 面板", "ctx.ui 构建器、节点类型、主题适配"),
    ("MULTI_LANGUAGE_SCRIPTING.md", "多语言脚本运行时", "Lua / C# / AngelScript / Emma 插件写法"),
    ("GAME_STATE_API.md", "游戏状态 API 参考", "star_resonance_plugin 参考实现：GameStateManager、ctx.mem"),
    ("HYBRID_MEMORY_TCP.md", "游戏数据源", "TCP / 内存 / 混合三种数据获取模式"),
    ("AI_EDITOR.md", "AI Editor 使用指南", "内置 AI 编辑器功能说明"),
    ("AI_EDITOR_MCP.md", "AI Editor MCP Server", "MCP 工具集成与外部 IDE 接入"),
]

MD_EXTENSIONS = [
    "fenced_code",
    "tables",
    "toc",
    "codehilite",
    "sane_lists",
    "attr_list",
    "admonition",
]
MD_EXT_CONFIG = {
    "codehilite": {"guess_lang": False, "css_class": "highlight"},
    "toc": {"anchorlink": False, "permalink": False, "toc_depth": "2-3"},
}

PAGE_TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} · SAO ACT UI 文档</title>
<link rel="stylesheet" href="assets/style.css">
<link rel="stylesheet" href="assets/pygments.css">
</head>
<body>
<header class="site-header">
  <a class="brand" href="index.html"><span class="brand-mark">SAO</span> ACT UI 文档</a>
  <nav class="top-nav">
    {top_nav}
  </nav>
</header>
<div class="layout">
  <aside class="sidebar">
    <div class="sidebar-title">开发者文档</div>
    <ul class="sidebar-nav">
      {side_nav}
    </ul>
  </aside>
  <main class="content">
    <article class="markdown-body">
      {body}
    </article>
  </main>
  <aside class="toc-rail">
    <div class="toc-title">本页目录</div>
    {toc}
  </aside>
</div>
<footer class="site-footer">SAO ACT UI &middot; 平台与插件开发者文档</footer>
</body>
</html>
"""

INDEX_TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SAO ACT UI 文档</title>
<link rel="stylesheet" href="assets/style.css">
</head>
<body>
<header class="site-header">
  <a class="brand" href="index.html"><span class="brand-mark">SAO</span> ACT UI 文档</a>
</header>
<div class="layout layout-index">
  <main class="content content-index">
    <h1>SAO ACT UI 开发者文档</h1>
    <p class="lede">游戏无关的战斗分析平台。平台核心负责事件分发、声明式 UI 渲染、插件生命周期与触发器执行；
       所有游戏特定逻辑由插件提供，内置的 <code>star_resonance_plugin</code>（星痕共鸣）是完整的参考实现。</p>
    <div class="card-grid">
      {cards}
    </div>
  </main>
</div>
<footer class="site-footer">SAO ACT UI &middot; 平台与插件开发者文档</footer>
</body>
</html>
"""

CSS = """
:root {
  --act-viewport-bg: #f8f8f8;
  --act-card-bg: rgba(250, 250, 250, 0.9);
  --act-edge: rgba(186, 190, 196, 0.6);
  --act-text: #33383d;
  --act-muted: #7c828a;
  --act-cyan: #12b6d6;
  --act-cyan-soft: rgba(18, 182, 214, 0.12);
  --act-gold: #b9860f;
  --act-gold-soft: rgba(185, 134, 15, 0.12);
  --act-danger: #d1503a;
  --act-ok: #3a9a4d;
  --act-shadow: rgba(31, 34, 40, 0.10);
  --sidebar-w: 250px;
  --toc-w: 220px;
}

* { box-sizing: border-box; }

html, body {
  margin: 0;
  padding: 0;
  background: var(--act-viewport-bg);
  color: var(--act-text);
  font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei", -apple-system, sans-serif;
  font-size: 15px;
  line-height: 1.7;
}

a { color: var(--act-cyan); text-decoration: none; }
a:hover { text-decoration: underline; }

.site-header {
  position: sticky;
  top: 0;
  z-index: 20;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 24px;
  height: 56px;
  background: #ffffff;
  border-bottom: 1px solid var(--act-edge);
  box-shadow: 0 2px 10px var(--act-shadow);
}

.brand {
  font-weight: 700;
  font-size: 16px;
  color: var(--act-text);
  letter-spacing: 0.02em;
}
.brand-mark {
  display: inline-block;
  padding: 2px 8px;
  margin-right: 8px;
  border-radius: 6px;
  background: linear-gradient(135deg, var(--act-cyan), var(--act-gold));
  color: #fff;
  font-weight: 800;
}

.top-nav a {
  margin-left: 18px;
  color: var(--act-muted);
  font-size: 13px;
}
.top-nav a:hover { color: var(--act-cyan); text-decoration: none; }

.layout {
  display: grid;
  grid-template-columns: var(--sidebar-w) minmax(0, 1fr) var(--toc-w);
  gap: 28px;
  max-width: 1280px;
  margin: 0 auto;
  padding: 28px 24px 80px;
  align-items: start;
}
.layout-index { grid-template-columns: 1fr; }

.sidebar {
  position: sticky;
  top: 76px;
  align-self: start;
}
.sidebar-title {
  font-size: 12px;
  font-weight: 700;
  color: var(--act-muted);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  margin-bottom: 10px;
  padding-left: 10px;
}
.sidebar-nav { list-style: none; margin: 0; padding: 0; }
.sidebar-nav li { margin: 2px 0; }
.sidebar-nav a {
  display: block;
  padding: 8px 10px;
  border-radius: 8px;
  color: var(--act-text);
  font-size: 13.5px;
}
.sidebar-nav a:hover { background: var(--act-cyan-soft); text-decoration: none; color: var(--act-cyan); }
.sidebar-nav a.active {
  background: var(--act-cyan-soft);
  color: var(--act-cyan);
  font-weight: 600;
  border-left: 3px solid var(--act-cyan);
  padding-left: 7px;
}

.content { min-width: 0; }

.toc-rail {
  position: sticky;
  top: 76px;
  align-self: start;
  border-left: 1px solid var(--act-edge);
  padding-left: 16px;
}
.toc-title {
  font-size: 12px;
  font-weight: 700;
  color: var(--act-muted);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  margin-bottom: 8px;
}
.toc-rail .toc { font-size: 12.5px; }
.toc-rail ul { list-style: none; padding-left: 12px; margin: 4px 0; }
.toc-rail > .toc > ul { padding-left: 0; }
.toc-rail a { color: var(--act-muted); }
.toc-rail a:hover { color: var(--act-cyan); text-decoration: none; }

.markdown-body {
  background: #ffffff;
  border: 1px solid var(--act-edge);
  border-radius: 14px;
  padding: 36px 44px;
  box-shadow: 0 4px 18px var(--act-shadow);
}

.markdown-body h1 {
  font-size: 26px;
  margin: 0 0 14px;
  padding-bottom: 14px;
  border-bottom: 2px solid var(--act-cyan);
  color: var(--act-text);
}
.markdown-body h2 {
  font-size: 20px;
  margin: 38px 0 14px;
  padding-left: 10px;
  border-left: 4px solid var(--act-cyan);
  color: var(--act-text);
}
.markdown-body h3 {
  font-size: 16.5px;
  margin: 26px 0 10px;
  color: var(--act-text);
}
.markdown-body h4 {
  font-size: 14.5px;
  margin: 20px 0 8px;
  color: var(--act-muted);
  text-transform: uppercase;
  letter-spacing: 0.03em;
}
.markdown-body p { margin: 10px 0; }
.markdown-body hr {
  border: none;
  border-top: 1px solid var(--act-edge);
  margin: 30px 0;
}

.markdown-body code {
  font-family: "Cascadia Code", Consolas, "SFMono-Regular", Menlo, monospace;
  background: var(--act-cyan-soft);
  color: #0d6a80;
  padding: 1px 5px;
  border-radius: 4px;
  font-size: 0.88em;
}

.markdown-body pre {
  background: #f4f6f7;
  border: 1px solid var(--act-edge);
  border-radius: 10px;
  padding: 14px 16px;
  overflow-x: auto;
  margin: 14px 0;
}
.markdown-body pre code {
  background: none;
  color: inherit;
  padding: 0;
  font-size: 0.85em;
}

.markdown-body blockquote {
  margin: 16px 0;
  padding: 10px 18px;
  background: var(--act-gold-soft);
  border-left: 4px solid var(--act-gold);
  border-radius: 0 8px 8px 0;
  color: #7a5b0c;
}
.markdown-body blockquote p { margin: 4px 0; }

.markdown-body table {
  border-collapse: collapse;
  width: 100%;
  margin: 16px 0;
  font-size: 13.5px;
}
.markdown-body th, .markdown-body td {
  border: 1px solid var(--act-edge);
  padding: 8px 12px;
  text-align: left;
}
.markdown-body th {
  background: var(--act-cyan-soft);
  color: #0d6a80;
  font-weight: 600;
}
.markdown-body tr:nth-child(even) td { background: #fafbfb; }

.markdown-body ul, .markdown-body ol { padding-left: 26px; }
.markdown-body li { margin: 4px 0; }

.markdown-body img { max-width: 100%; }

.markdown-body strong { color: #a0470f; }

/* index page */
.content-index { padding: 20px 0 60px; }
.content-index h1 { font-size: 30px; margin-bottom: 6px; }
.lede { color: var(--act-muted); max-width: 760px; margin-bottom: 30px; }
.card-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 18px;
}
.doc-card {
  display: block;
  background: #fff;
  border: 1px solid var(--act-edge);
  border-radius: 14px;
  padding: 20px 22px;
  box-shadow: 0 4px 14px var(--act-shadow);
  transition: transform 0.12s ease, box-shadow 0.12s ease, border-color 0.12s ease;
}
.doc-card:hover {
  transform: translateY(-2px);
  border-color: var(--act-cyan);
  box-shadow: 0 8px 22px var(--act-shadow);
  text-decoration: none;
}
.doc-card .doc-title {
  font-size: 16px;
  font-weight: 700;
  color: var(--act-text);
  margin-bottom: 6px;
}
.doc-card .doc-desc {
  font-size: 13px;
  color: var(--act-muted);
  line-height: 1.6;
}

.site-footer {
  text-align: center;
  color: var(--act-muted);
  font-size: 12px;
  padding: 20px 0 40px;
}

@media (max-width: 1080px) {
  .layout { grid-template-columns: var(--sidebar-w) minmax(0, 1fr); }
  .toc-rail { display: none; }
}
@media (max-width: 760px) {
  .layout { grid-template-columns: 1fr; padding: 20px 14px 60px; }
  .sidebar { position: static; }
  .markdown-body { padding: 22px 18px; }
}
"""


def slugify(name: str) -> str:
    return name.rsplit(".", 1)[0]


def build_nav_html(current: str) -> tuple[str, str]:
    side_items = []
    top_items = []
    for fname, label, _desc in PAGES:
        href = slugify(fname) + ".html"
        cls = ' class="active"' if fname == current else ""
        side_items.append(f'<li><a href="{href}"{cls}>{label}</a></li>')
    top_items.append('<a href="index.html">全部文档</a>')
    return "\n      ".join(side_items), "\n    ".join(top_items)


def rewrite_md_links(html: str) -> str:
    def _sub(match: "re.Match[str]") -> str:
        href = match.group(1)
        name, _, anchor = href.partition("#")
        new = slugify(name) + ".html"
        if anchor:
            new += "#" + anchor
        return f'href="{new}"'

    return re.sub(r'href="([A-Za-z_]+\.md(?:#[^"]*)?)"', _sub, html)


def main() -> None:
    OUT_DIR.mkdir(exist_ok=True)
    ASSETS_DIR.mkdir(exist_ok=True)

    (ASSETS_DIR / "style.css").write_text(CSS, encoding="utf-8")
    formatter = HtmlFormatter(style="friendly", nowrap=False)
    pygments_css = formatter.get_style_defs(".highlight")
    (ASSETS_DIR / "pygments.css").write_text(pygments_css, encoding="utf-8")

    cards = []
    for fname, label, desc in PAGES:
        src = DOCS_DIR / fname
        text = src.read_text(encoding="utf-8")

        md = markdown.Markdown(extensions=MD_EXTENSIONS, extension_configs=MD_EXT_CONFIG)
        body_html = md.convert(text)
        body_html = rewrite_md_links(body_html)
        toc_html = rewrite_md_links(getattr(md, "toc", ""))

        # first H1 becomes the page title
        h1_match = re.search(r"<h1[^>]*>(.*?)</h1>", body_html)
        title = re.sub(r"<[^>]+>", "", h1_match.group(1)) if h1_match else label

        side_nav, top_nav = build_nav_html(fname)
        page_html = PAGE_TEMPLATE.format(
            title=title,
            top_nav=top_nav,
            side_nav=side_nav,
            body=body_html,
            toc=toc_html or "<p class='toc-empty'>(无子章节)</p>",
        )
        out_path = OUT_DIR / (slugify(fname) + ".html")
        out_path.write_text(page_html, encoding="utf-8")
        print(f"wrote {out_path.relative_to(OUT_DIR.parent)}")

        cards.append(
            f'<a class="doc-card" href="{slugify(fname)}.html">'
            f'<div class="doc-title">{label}</div>'
            f'<div class="doc-desc">{desc}</div>'
            f"</a>"
        )

    index_html = INDEX_TEMPLATE.format(cards="\n      ".join(cards))
    (OUT_DIR / "index.html").write_text(index_html, encoding="utf-8")
    print(f"wrote {(OUT_DIR / 'index.html').relative_to(OUT_DIR.parent)}")


if __name__ == "__main__":
    main()
