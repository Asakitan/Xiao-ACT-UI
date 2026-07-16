# Generate the offline SAO ACT UI user manual under docs/html/.
# Usage: python build_html_docs.py
from __future__ import annotations

from pathlib import Path

DOCS_DIR = Path(__file__).resolve().parent
OUT_DIR = DOCS_DIR.parent / "html"
ASSETS_DIR = OUT_DIR / "assets"

INDEX_HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light">
  <meta http-equiv="Content-Security-Policy" content="default-src 'self'; style-src 'self'; img-src 'self' data:; script-src 'none'; connect-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'">
  <title>SAO ACT UI 用户手册</title>
  <link rel="stylesheet" href="assets/style.css">
</head>
<body>
  <a class="skip-link" href="#manual">跳到手册正文</a>
  <header class="site-header">
    <a class="brand" href="#top" aria-label="返回手册顶部">
      <span class="brand-mark">SAO</span>
      <span class="brand-copy"><strong>ACT UI</strong><small>USER MANUAL</small></span>
    </a>
    <nav class="header-nav" aria-label="主要章节">
      <a href="#quick-start">快速开始</a><a href="#hotkeys">快捷键</a><a href="#faq">常见问题</a>
    </nav>
    <span class="offline-badge"><i aria-hidden="true"></i>离线手册</span>
  </header>

  <main id="manual">
    <section class="hero" id="top" aria-labelledby="hero-title">
      <div class="hero-copy">
        <p class="eyebrow"><span>01</span> 欢迎连接</p>
        <h1 id="hero-title">你的 SAO 悬浮界面<br><em>从这里开始</em></h1>
        <p class="hero-lede">这是一份面向日常使用的单页手册。启动应用、打开菜单、启用插件与面板，然后按你的习惯调整快捷键。</p>
        <div class="hero-actions">
          <a class="action-primary" href="#quick-start">开始使用 <span aria-hidden="true">→</span></a>
          <a class="action-secondary" href="#hotkeys">查看快捷键</a>
        </div>
        <ul class="hero-facts" aria-label="手册特性"><li>纯本地阅读</li><li>无需网络</li><li>适配手机与桌面</li></ul>
      </div>
      <div class="hero-visual" aria-hidden="true">
        <div class="orbit orbit-outer"></div><div class="orbit orbit-inner"></div>
        <div class="visual-core"><span>LINK</span><strong>START</strong><i></i><small>USER GUIDE</small></div>
        <b class="orbit-label label-top">READY</b><b class="orbit-label label-right">MENU</b><b class="orbit-label label-bottom">PANEL</b>
      </div>
    </section>

    <div class="manual-shell">
      <aside class="chapter-rail">
        <nav aria-label="手册目录">
          <p>操作导航</p>
          <ol>
            <li><a href="#quick-start"><span>01</span>快速开始</a></li>
            <li><a href="#floating-menu"><span>02</span>悬浮按钮与菜单</a></li>
            <li><a href="#hotkeys"><span>03</span>默认快捷键</a></li>
            <li><a href="#panels-plugins"><span>04</span>面板与插件</a></li>
            <li><a href="#ai-editor"><span>05</span>AI Editor</a></li>
            <li><a href="#faq"><span>06</span>常见问题</a></li>
            <li><a href="#privacy"><span>07</span>隐私说明</a></li>
          </ol>
        </nav>
        <div class="rail-tip"><span aria-hidden="true">⌂</span><p><strong>记住 HOME</strong>随时打开或关闭 SAO 菜单。</p></div>
      </aside>

      <article class="manual-content">
        <section class="manual-section" id="quick-start" aria-labelledby="quick-title">
          <header class="section-heading"><span class="section-number">01</span><div><p>GET STARTED</p><h2 id="quick-title">快速开始</h2></div></header>
          <p class="section-intro">第一次使用只需要完成下面四步。不同游戏可用的功能取决于已安装并启用的插件。</p>
          <ol class="step-list">
            <li><span class="step-index">01</span><div><h3>启动应用</h3><p>运行 <kbd>XiaoACTUI.exe</kbd>。启动完成后，屏幕左上角会出现圆形 NerveGear 悬浮按钮。</p></div></li>
            <li><span class="step-index">02</span><div><h3>打开 SAO 菜单</h3><p>左键点击悬浮按钮，或按 <kbd>HOME</kbd>，进入全部用户功能入口。</p></div></li>
            <li><span class="step-index">03</span><div><h3>确认插件</h3><p>进入“插件”，启用与你当前游戏或使用场景对应的插件。</p></div></li>
            <li><span class="step-index">04</span><div><h3>打开面板</h3><p>从菜单或插件入口打开需要的统计、状态或提示面板；按 <kbd>F5</kbd> 启动识别。</p></div></li>
          </ol>
          <aside class="callout callout-cyan"><span class="callout-icon" aria-hidden="true">i</span><div><strong>初次使用建议</strong><p>先保留默认快捷键熟悉操作，再到“设置”中按自己的键位习惯调整。</p></div></aside>
        </section>

        <section class="manual-section" id="floating-menu" aria-labelledby="floating-title">
          <header class="section-heading"><span class="section-number">02</span><div><p>NERVEGEAR CONTROL</p><h2 id="floating-title">悬浮按钮与菜单</h2></div></header>
          <p class="section-intro">NerveGear 按钮是日常操作的起点。菜单展开后，选择分类即可查看对应功能。</p>
          <div class="interaction-board">
            <div class="button-demo" aria-hidden="true"><span class="demo-ring ring-one"></span><span class="demo-ring ring-two"></span><span class="demo-button">NG</span><span class="demo-caption">NERVEGEAR</span></div>
            <dl class="gesture-list">
              <div><dt><span>左键</span>打开菜单</dt><dd>展开 SAO 主菜单，再选择 ACT、插件、设置或关于等入口。</dd></div>
              <div><dt><span>右键</span>快捷设置</dt><dd>从悬浮按钮的快捷菜单进入设置与常用显示控制。</dd></div>
              <div><dt><span>HOME</span>键盘切换</dt><dd>不方便点击按钮时，直接打开或关闭 SAO 菜单。</dd></div>
              <div><dt><span>INSERT</span>隐藏按钮</dt><dd>需要清理画面时隐藏或恢复悬浮按钮；部分显示模式会同时隐藏覆盖层。</dd></div>
            </dl>
          </div>
          <div class="menu-route" aria-label="常用菜单路径">
            <div><span>ACT</span><strong>常用面板与 AI Editor</strong><small>统计、状态与辅助入口</small></div>
            <div><span>插件</span><strong>启用和管理扩展</strong><small>实际项目以已安装内容为准</small></div>
            <div><span>设置</span><strong>显示、快捷键与偏好</strong><small>修改后记得保存</small></div>
            <div><span>关于</span><strong>版本、更新与本手册</strong><small>随时返回查看操作说明</small></div>
          </div>
        </section>

        <section class="manual-section" id="hotkeys" aria-labelledby="hotkeys-title">
          <header class="section-heading"><span class="section-number">03</span><div><p>KEY BINDINGS</p><h2 id="hotkeys-title">默认快捷键</h2></div></header>
          <p class="section-intro">以下是首次启动时的平台默认键位。所有快捷键都可以在“设置”中修改，插件也可能提供自己的快捷键。</p>
          <div class="hotkey-table-wrap">
            <table class="hotkey-table">
              <thead><tr><th scope="col">默认按键</th><th scope="col">功能</th><th scope="col">使用场景</th></tr></thead>
              <tbody>
                <tr><td><kbd>F5</kbd></td><td><strong>启动 / 停止识别</strong></td><td>开始使用或暂时停止当前识别引擎</td></tr>
                <tr><td><kbd>F9</kbd></td><td><strong>切换窗口置顶</strong></td><td>让悬浮窗口保持在其他窗口上方，或恢复普通层级</td></tr>
                <tr><td><kbd>F10</kbd></td><td><strong>隐藏 / 恢复面板</strong></td><td>临时清理画面，同时保留当前面板选择</td></tr>
                <tr><td><kbd>F11</kbd></td><td><strong>打开插件快捷菜单</strong></td><td>快速查看插件并进入常用插件操作</td></tr>
                <tr><td><kbd>INSERT</kbd></td><td><strong>隐藏 / 显示悬浮按钮</strong></td><td>隐藏 NerveGear 按钮；部分显示模式会同时处理覆盖层</td></tr>
                <tr><td><kbd>HOME</kbd></td><td><strong>打开 / 关闭 SAO 菜单</strong></td><td>无需点击悬浮按钮即可切换主菜单</td></tr>
              </tbody>
            </table>
          </div>
          <aside class="callout callout-gold"><span class="callout-icon" aria-hidden="true">!</span><div><strong>键位冲突时</strong><p>打开“设置”中的快捷键区域，换成未被游戏或其他软件占用的组合并保存。</p></div></aside>
        </section>

        <section class="manual-section" id="panels-plugins" aria-labelledby="panels-title">
          <header class="section-heading"><span class="section-number">04</span><div><p>PANELS &amp; PLUGINS</p><h2 id="panels-title">面板与插件</h2></div></header>
          <p class="section-intro">插件决定有哪些功能可用，面板负责把这些功能呈现在屏幕上。先管理插件，再选择需要显示的面板。</p>
          <div class="split-guide">
            <section><span class="guide-tag">PLUGIN</span><h3>插件：决定可用功能</h3><ul><li>按当前游戏或使用场景启用对应插件。</li><li>不使用的插件可以停用，之后仍可重新开启。</li><li>按 <kbd>F11</kbd> 打开插件快捷菜单；完整管理请进入“插件管理面板”。</li><li>插件新增的菜单、面板和快捷键会随启用状态变化。</li></ul></section>
            <section><span class="guide-tag">PANEL</span><h3>面板：显示你关心的信息</h3><ul><li>从 ACT 或插件菜单打开需要的面板。</li><li>不需要时可关闭单个面板，或用 <kbd>F10</kbd> 暂时隐藏全部面板。</li><li>窗口被遮挡时使用 <kbd>F9</kbd> 切换置顶。</li><li>显示内容与选项以当前插件提供的功能为准。</li></ul></section>
          </div>
          <div class="flow-strip" aria-label="推荐操作顺序"><span>打开菜单</span><i aria-hidden="true">→</i><span>启用插件</span><i aria-hidden="true">→</i><span>选择面板</span><i aria-hidden="true">→</i><span>按 F5 启动</span></div>
        </section>

        <section class="manual-section" id="ai-editor" aria-labelledby="ai-title">
          <header class="section-heading"><span class="section-number">05</span><div><p>AI ASSISTANT</p><h2 id="ai-title">AI Editor 普通用户入口</h2></div></header>
          <div class="ai-entry">
            <div class="ai-copy">
              <p class="route-label">打开路径</p><p class="route-path"><span>SAO 菜单</span><b>›</b><span>ACT</span><b>›</b><span>AI Editor (LLM)</span></p>
              <h3>把它当作内置的工作区与 AI 助手</h3>
              <p>普通用户无需了解扩展机制。完成连接设置、选择模型后，就可以新建或打开文件，并在右侧对话区提出问题。</p>
              <ol><li>首次打开后进入设置，完成你要使用的模型服务配置。</li><li>选择模型和对话模式，确认当前打开的工作区。</li><li>在对话框描述目标；涉及文件操作时，先核对目标与变更提示。</li><li>重要内容先自行备份；使用在线服务时，只发送你愿意分享的内容。</li></ol>
            </div>
            <div class="editor-sketch" aria-hidden="true">
              <div class="sketch-title"><i></i><i></i><i></i><span>AI EDITOR</span></div>
              <div class="sketch-body"><div class="sketch-rail"><b></b><b></b><b></b><b></b></div><div class="sketch-code"><span></span><span></span><span></span><span></span><span></span></div><div class="sketch-chat"><strong>ASSISTANT</strong><p></p><p></p><em></em></div></div>
            </div>
          </div>
        </section>

        <section class="manual-section" id="faq" aria-labelledby="faq-title">
          <header class="section-heading"><span class="section-number">06</span><div><p>TROUBLESHOOTING</p><h2 id="faq-title">常见问题</h2></div></header>
          <div class="faq-list">
            <details><summary><span>悬浮按钮不见了，怎样找回来？</span><i aria-hidden="true">+</i></summary><p>先按 <kbd>INSERT</kbd> 恢复悬浮按钮，再按 <kbd>HOME</kbd> 尝试打开菜单。如果应用已退出，请重新启动。</p></details>
            <details><summary><span>按快捷键没有反应怎么办？</span><i aria-hidden="true">+</i></summary><p>检查是否与游戏或其他软件冲突，然后在“设置”中重新绑定并保存。也可以先通过悬浮按钮操作，确认对应功能本身可用。</p></details>
            <details><summary><span>面板已经打开，但没有显示内容？</span><i aria-hidden="true">+</i></summary><p>确认对应插件已启用、当前场景符合插件要求，并按 <kbd>F5</kbd> 启动识别。不同插件的可用条件可能不同。</p></details>
            <details><summary><span>面板被游戏或其他窗口遮住了？</span><i aria-hidden="true">+</i></summary><p>按 <kbd>F9</kbd> 切换窗口置顶。若暂时不想显示面板，按 <kbd>F10</kbd> 隐藏，再按一次恢复。</p></details>
            <details><summary><span>插件功能与手册描述不完全一样？</span><i aria-hidden="true">+</i></summary><p>本页介绍平台通用操作。插件会按版本和使用场景提供不同菜单、面板与快捷键，请以应用内当前显示为准。</p></details>
            <details><summary><span>AI Editor 没有返回内容？</span><i aria-hidden="true">+</i></summary><p>检查所选模型的连接设置；使用在线服务时确认网络可用。仍无响应时，新建对话后重试，并避免一次提交过多内容。</p></details>
          </div>
        </section>

        <section class="manual-section privacy-section" id="privacy" aria-labelledby="privacy-title">
          <header class="section-heading"><span class="section-number">07</span><div><p>PRIVACY</p><h2 id="privacy-title">隐私说明</h2></div></header>
          <div class="privacy-grid">
            <div><span class="privacy-icon">01</span><h3>本地资料由你控制</h3><p>应用的本地配置和运行日志保存在你的设备上；是否保留、检查、分享或删除，由你自行决定。</p></div>
            <div><span class="privacy-icon">02</span><h3>主动选择分享内容</h3><p>使用在线 AI 服务时，只提交你愿意交给所选服务处理的内容，并同时参考该服务自己的隐私说明。</p></div>
            <div><span class="privacy-icon">03</span><h3>用户手册边界</h3><p>本手册只介绍用户可见操作，不包含实现细节或内部资料。</p></div>
          </div>
          <p class="privacy-note">这份页面只帮助用户完成可见界面的日常操作，不承担技术参考或内部实现说明。</p>
        </section>
      </article>
    </div>
  </main>

  <footer class="site-footer"><a class="footer-brand" href="#top"><span>SAO</span> ACT UI</a><p>用户手册 · 本地离线页面</p><a href="#top">返回顶部 ↑</a></footer>
</body>
</html>
"""

_LEGACY_CSS = """
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

CSS = """
:root {
  --bg: #f3f7f8;
  --surface: #ffffff;
  --ink: #18262d;
  --ink-strong: #0b171d;
  --muted: #667780;
  --line: #cbd7db;
  --line-strong: #a9bbc1;
  --cyan: #08a9ce;
  --cyan-deep: #087d9a;
  --cyan-soft: rgba(8, 169, 206, 0.10);
  --gold: #c7942b;
  --gold-deep: #8b6214;
  --gold-soft: rgba(199, 148, 43, 0.12);
  --shadow: 0 22px 60px rgba(20, 49, 59, 0.10);
  --shadow-small: 0 8px 24px rgba(20, 49, 59, 0.08);
  --header-height: 68px;
  --content-width: 1240px;
}

html { scroll-behavior: smooth; scroll-padding-top: calc(var(--header-height) + 28px); }
body {
  background: linear-gradient(rgba(8, 169, 206, 0.035) 1px, transparent 1px), linear-gradient(90deg, rgba(8, 169, 206, 0.035) 1px, transparent 1px), var(--bg);
  background-size: 40px 40px;
  color: var(--ink);
  font-family: "Segoe UI", "PingFang SC", "Microsoft YaHei UI", "Microsoft YaHei", sans-serif;
  font-size: 16px;
  line-height: 1.72;
}
body::before {
  position: fixed; inset: 0; z-index: -1; content: ""; pointer-events: none;
  background: radial-gradient(circle at 12% 14%, rgba(8, 169, 206, 0.09), transparent 24rem), radial-gradient(circle at 88% 48%, rgba(199, 148, 43, 0.08), transparent 28rem);
}
a { color: var(--cyan-deep); text-decoration: none; }
a:hover { text-decoration: none; }
a:focus-visible, summary:focus-visible { outline: 3px solid rgba(8, 169, 206, 0.38); outline-offset: 4px; }
.skip-link { position: fixed; top: 8px; left: 8px; z-index: 100; padding: 10px 16px; background: var(--ink-strong); color: #fff; transform: translateY(-150%); transition: transform 160ms ease; }
.skip-link:focus { transform: translateY(0); }

.site-header {
  position: sticky; top: 0; z-index: 40; display: grid; grid-template-columns: 1fr auto 1fr; align-items: center;
  min-height: var(--header-height); height: auto; padding: 0 max(24px, calc((100vw - var(--content-width)) / 2));
  border-bottom: 1px solid rgba(169, 187, 193, 0.72); background: rgba(250, 253, 253, 0.92); backdrop-filter: blur(18px); box-shadow: 0 4px 24px rgba(25, 55, 65, 0.06);
}
.brand { display: inline-flex; align-items: center; justify-self: start; color: var(--ink-strong); font-size: inherit; letter-spacing: normal; }
.brand-mark { display: grid; width: 48px; height: 34px; place-items: center; margin: 0; padding: 0; border-radius: 0; background: linear-gradient(135deg, var(--cyan-deep), var(--cyan)); clip-path: polygon(10% 0, 100% 0, 90% 100%, 0 100%); color: #fff; font-size: 13px; font-weight: 900; letter-spacing: 0.13em; }
.brand-copy { display: grid; margin-left: 10px; line-height: 1.05; }
.brand-copy strong { font-size: 15px; letter-spacing: 0.06em; }
.brand-copy small { margin-top: 4px; color: var(--muted); font-size: 9px; font-weight: 700; letter-spacing: 0.2em; }
.header-nav { display: flex; gap: 30px; }
.header-nav a { position: relative; padding: 22px 0 19px; color: var(--muted); font-size: 13px; font-weight: 700; }
.header-nav a::after { position: absolute; right: 50%; bottom: 13px; left: 50%; height: 2px; background: var(--cyan); content: ""; transition: right 160ms ease, left 160ms ease; }
.header-nav a:hover { color: var(--ink-strong); }
.header-nav a:hover::after { right: 0; left: 0; }
.offline-badge { display: inline-flex; align-items: center; justify-self: end; gap: 8px; color: var(--muted); font-size: 12px; font-weight: 700; }
.offline-badge i { width: 7px; height: 7px; border-radius: 50%; background: #38a65b; box-shadow: 0 0 0 5px rgba(56, 166, 91, 0.11); }

.hero { position: relative; display: grid; grid-template-columns: minmax(0, 1.16fr) minmax(360px, 0.84fr); min-height: 650px; max-width: var(--content-width); margin: 0 auto; padding: 92px 28px 88px; overflow: hidden; }
.hero::after { position: absolute; right: 27%; bottom: 46px; left: 28px; height: 1px; background: linear-gradient(90deg, var(--cyan), var(--line), transparent); content: ""; }
.hero-copy { position: relative; z-index: 2; align-self: center; }
.eyebrow { display: flex; align-items: center; gap: 12px; margin: 0 0 20px; color: var(--cyan-deep); font-size: 12px; font-weight: 800; letter-spacing: 0.18em; }
.eyebrow span, .section-number { display: inline-grid; place-items: center; background: linear-gradient(135deg, var(--cyan-deep), var(--cyan)); clip-path: polygon(0 0, 100% 0, 82% 100%, 0 100%); color: #fff; font-weight: 900; }
.eyebrow span { width: 34px; height: 22px; font-size: 10px; letter-spacing: 0; }
.hero h1 { margin: 0; color: var(--ink-strong); font-size: clamp(48px, 6vw, 78px); font-weight: 760; letter-spacing: -0.055em; line-height: 1.08; }
.hero h1 em { position: relative; color: var(--cyan-deep); font-style: normal; }
.hero h1 em::after { position: absolute; right: -8px; bottom: 1px; left: 4px; z-index: -1; height: 12px; background: rgba(8, 169, 206, 0.13); content: ""; }
.hero-lede { max-width: 680px; margin: 28px 0 0; color: var(--muted); font-size: 18px; line-height: 1.9; }
.hero-actions { display: flex; flex-wrap: wrap; gap: 12px; margin-top: 34px; }
.action-primary, .action-secondary { display: inline-flex; min-height: 48px; align-items: center; justify-content: center; padding: 0 24px; font-size: 14px; font-weight: 800; transition: transform 150ms ease, box-shadow 150ms ease; }
.action-primary { gap: 22px; background: linear-gradient(135deg, var(--cyan-deep), var(--cyan)); clip-path: polygon(0 0, calc(100% - 13px) 0, 100% 50%, calc(100% - 13px) 100%, 0 100%); color: #fff; box-shadow: 0 12px 26px rgba(8, 125, 154, 0.22); }
.action-secondary { border: 1px solid var(--line-strong); background: rgba(255, 255, 255, 0.64); color: var(--ink); }
.action-primary:hover, .action-secondary:hover { transform: translateY(-2px); box-shadow: var(--shadow-small); }
.hero-facts { display: flex; flex-wrap: wrap; gap: 20px; margin: 34px 0 0; padding: 0; color: var(--muted); font-size: 12px; font-weight: 700; list-style: none; }
.hero-facts li { display: flex; align-items: center; gap: 8px; }
.hero-facts li::before { width: 5px; height: 5px; background: var(--gold); content: ""; transform: rotate(45deg); }

.hero-visual { position: relative; align-self: center; justify-self: end; width: min(38vw, 440px); aspect-ratio: 1; }
.hero-visual::before, .hero-visual::after { position: absolute; inset: 4%; border: 1px solid rgba(8, 169, 206, 0.18); content: ""; transform: rotate(45deg); }
.hero-visual::after { inset: 19%; border-color: rgba(199, 148, 43, 0.28); }
.orbit { position: absolute; border: 1px solid var(--line-strong); border-radius: 50%; }
.orbit::before, .orbit::after { position: absolute; width: 11px; height: 11px; border: 3px solid var(--bg); border-radius: 50%; background: var(--cyan); box-shadow: 0 0 0 1px var(--cyan); content: ""; }
.orbit-outer { inset: 7%; border-style: dashed; animation: orbit-spin 42s linear infinite; }
.orbit-outer::before { top: 10%; left: 16%; }
.orbit-outer::after { right: 10%; bottom: 17%; background: var(--gold); box-shadow: 0 0 0 1px var(--gold); }
.orbit-inner { inset: 23%; border-color: rgba(8, 169, 206, 0.52); }
.orbit-inner::before { top: -7px; left: calc(50% - 5px); }
.orbit-inner::after { right: calc(50% - 5px); bottom: -7px; }
.visual-core { position: absolute; inset: 34%; display: flex; flex-direction: column; align-items: center; justify-content: center; border: 1px solid rgba(8, 169, 206, 0.42); background: rgba(255, 255, 255, 0.88); box-shadow: 0 0 0 12px rgba(255, 255, 255, 0.48), var(--shadow-small); clip-path: polygon(18% 0, 82% 0, 100% 18%, 100% 82%, 82% 100%, 18% 100%, 0 82%, 0 18%); }
.visual-core span, .visual-core small { color: var(--muted); font-size: clamp(7px, 1vw, 10px); font-weight: 800; letter-spacing: 0.22em; }
.visual-core strong { color: var(--cyan-deep); font-size: clamp(19px, 3vw, 34px); letter-spacing: 0.08em; line-height: 1.25; }
.visual-core i { width: 42%; height: 2px; margin: 7px 0; background: linear-gradient(90deg, transparent, var(--gold), transparent); }
.orbit-label { position: absolute; padding: 4px 9px; background: var(--bg); color: var(--muted); font-size: 9px; font-weight: 900; letter-spacing: 0.16em; }
.label-top { top: 5%; left: 45%; } .label-right { top: 48%; right: 2%; } .label-bottom { bottom: 4%; left: 43%; }
@keyframes orbit-spin { to { transform: rotate(360deg); } }

.manual-shell { display: grid; grid-template-columns: 250px minmax(0, 1fr); gap: 54px; max-width: var(--content-width); margin: 0 auto; padding: 24px 28px 110px; }
.chapter-rail { position: sticky; top: calc(var(--header-height) + 26px); align-self: start; }
.chapter-rail nav { padding: 24px 20px 20px; border: 1px solid var(--line); background: rgba(255, 255, 255, 0.68); box-shadow: var(--shadow-small); clip-path: polygon(0 0, calc(100% - 18px) 0, 100% 18px, 100% 100%, 0 100%); }
.chapter-rail nav > p { margin: 0 0 16px; color: var(--muted); font-size: 10px; font-weight: 900; letter-spacing: 0.2em; }
.chapter-rail ol { display: grid; gap: 2px; margin: 0; padding: 0; list-style: none; }
.chapter-rail a { display: grid; grid-template-columns: 30px 1fr; align-items: center; padding: 9px 8px; border-left: 2px solid transparent; color: var(--ink); font-size: 13px; font-weight: 700; transition: border-color 150ms ease, background 150ms ease, color 150ms ease; }
.chapter-rail a span { color: var(--line-strong); font-size: 9px; font-weight: 900; }
.chapter-rail a:hover { border-left-color: var(--cyan); background: var(--cyan-soft); color: var(--cyan-deep); }
.rail-tip { display: grid; grid-template-columns: 34px 1fr; gap: 12px; margin-top: 14px; padding: 16px; border: 1px solid rgba(199, 148, 43, 0.36); background: var(--gold-soft); }
.rail-tip > span { display: grid; width: 32px; height: 32px; place-items: center; background: var(--gold); color: #fff; font-weight: 900; }
.rail-tip p { margin: 0; color: var(--gold-deep); font-size: 11px; line-height: 1.55; }
.rail-tip strong { display: block; margin-bottom: 2px; font-size: 12px; }
.manual-content { min-width: 0; }
.manual-section { position: relative; margin-bottom: 94px; padding-top: 18px; }
.manual-section::after { position: absolute; right: 0; bottom: -46px; left: 0; height: 1px; background: linear-gradient(90deg, var(--line), transparent); content: ""; }
.manual-section:last-child::after { display: none; }
.section-heading { display: flex; align-items: center; gap: 18px; margin-bottom: 20px; }
.section-number { width: 54px; height: 46px; font-size: 13px; letter-spacing: 0.08em; }
.section-heading p { margin: 0 0 2px; color: var(--cyan-deep); font-size: 9px; font-weight: 900; letter-spacing: 0.2em; }
.section-heading h2 { margin: 0; color: var(--ink-strong); font-size: clamp(28px, 4vw, 42px); letter-spacing: -0.035em; line-height: 1.2; }
.section-intro { max-width: 800px; margin: 0 0 30px; color: var(--muted); font-size: 17px; }
kbd { display: inline-block; min-width: 2.2em; padding: 2px 8px; border: 1px solid var(--line-strong); border-bottom-width: 3px; border-radius: 5px; background: #fff; color: var(--ink-strong); font-family: "Cascadia Mono", Consolas, monospace; font-size: 0.86em; font-weight: 800; line-height: 1.55; text-align: center; white-space: nowrap; }

.step-list { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 1px; margin: 0; padding: 1px; background: var(--line); box-shadow: var(--shadow); list-style: none; }
.step-list li { position: relative; display: grid; grid-template-columns: 46px 1fr; gap: 14px; min-height: 176px; padding: 28px 26px; background: var(--surface); }
.step-list li::after { position: absolute; right: 0; bottom: 0; width: 24px; height: 24px; border-right: 2px solid var(--cyan); border-bottom: 2px solid var(--cyan); content: ""; opacity: 0; transition: opacity 160ms ease; }
.step-list li:hover::after { opacity: 1; }
.step-index { display: grid; width: 40px; height: 30px; place-items: center; background: var(--cyan-soft); color: var(--cyan-deep); font-size: 11px; font-weight: 900; }
.step-list h3 { margin: 0 0 8px; color: var(--ink-strong); font-size: 18px; }
.step-list p { margin: 0; color: var(--muted); font-size: 14px; }
.callout { display: grid; grid-template-columns: 44px 1fr; gap: 16px; margin-top: 22px; padding: 18px 22px; border-left: 3px solid; }
.callout-cyan { border-color: var(--cyan); background: var(--cyan-soft); }
.callout-gold { border-color: var(--gold); background: var(--gold-soft); }
.callout-icon { display: grid; width: 38px; height: 38px; place-items: center; border: 1px solid currentColor; border-radius: 50%; color: var(--cyan-deep); font-family: Georgia, serif; font-size: 18px; font-weight: 800; }
.callout-gold .callout-icon { color: var(--gold-deep); }
.callout strong { color: var(--ink-strong); font-size: 14px; }
.callout p { margin: 3px 0 0; color: var(--muted); font-size: 13px; }

.interaction-board { display: grid; grid-template-columns: 280px 1fr; min-height: 390px; border: 1px solid var(--line); background: var(--surface); box-shadow: var(--shadow); }
.button-demo { position: relative; display: grid; place-items: center; overflow: hidden; border-right: 1px solid var(--line); background: linear-gradient(rgba(255, 255, 255, 0.86), rgba(255, 255, 255, 0.86)), repeating-linear-gradient(0deg, var(--cyan-soft) 0 1px, transparent 1px 22px); }
.demo-ring { position: absolute; border: 1px solid rgba(8, 169, 206, 0.34); border-radius: 50%; }
.ring-one { width: 190px; height: 190px; } .ring-two { width: 136px; height: 136px; border-style: dashed; }
.demo-button { position: relative; z-index: 2; display: grid; width: 78px; height: 78px; place-items: center; border: 5px solid #e5f6fa; border-radius: 50%; background: linear-gradient(145deg, #24bfdd, #087d9a); color: #fff; font-size: 18px; font-weight: 900; letter-spacing: 0.12em; box-shadow: 0 14px 32px rgba(8, 125, 154, 0.28), inset 0 0 0 2px rgba(255, 255, 255, 0.48); }
.demo-caption { position: absolute; bottom: 50px; color: var(--muted); font-size: 9px; font-weight: 900; letter-spacing: 0.26em; }
.gesture-list { display: grid; align-content: center; margin: 0; padding: 24px 34px; }
.gesture-list > div { padding: 17px 0; border-bottom: 1px solid var(--line); }
.gesture-list > div:last-child { border-bottom: 0; }
.gesture-list dt { color: var(--ink-strong); font-size: 15px; font-weight: 800; }
.gesture-list dt span { display: inline-block; min-width: 62px; margin-right: 12px; color: var(--cyan-deep); font-size: 11px; letter-spacing: 0.08em; }
.gesture-list dd { margin: 4px 0 0 74px; color: var(--muted); font-size: 13px; }
.menu-route { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; margin-top: 16px; }
.menu-route > div { min-height: 150px; padding: 18px; border-top: 2px solid var(--cyan); background: rgba(255, 255, 255, 0.72); box-shadow: var(--shadow-small); }
.menu-route span { color: var(--cyan-deep); font-size: 10px; font-weight: 900; letter-spacing: 0.12em; }
.menu-route strong, .menu-route small { display: block; }
.menu-route strong { margin-top: 12px; color: var(--ink-strong); font-size: 14px; line-height: 1.5; }
.menu-route small { margin-top: 8px; color: var(--muted); font-size: 11px; line-height: 1.55; }

.hotkey-table-wrap { overflow-x: auto; border: 1px solid var(--line); background: var(--surface); box-shadow: var(--shadow); }
.hotkey-table { width: 100%; min-width: 680px; border-collapse: collapse; }
.hotkey-table th, .hotkey-table td { padding: 18px 22px; border-bottom: 1px solid var(--line); text-align: left; }
.hotkey-table th { background: #eaf3f5; color: var(--muted); font-size: 10px; font-weight: 900; letter-spacing: 0.14em; }
.hotkey-table tbody tr:last-child td { border-bottom: 0; }
.hotkey-table tbody tr:hover { background: var(--cyan-soft); }
.hotkey-table td:first-child { width: 130px; }
.hotkey-table td:nth-child(2) { width: 230px; color: var(--ink-strong); font-size: 14px; }
.hotkey-table td:nth-child(3) { color: var(--muted); font-size: 13px; }

.split-guide { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }
.split-guide section { min-height: 330px; padding: 32px; border: 1px solid var(--line); background: var(--surface); box-shadow: var(--shadow-small); clip-path: polygon(0 0, calc(100% - 22px) 0, 100% 22px, 100% 100%, 0 100%); }
.split-guide section:first-child { border-top: 3px solid var(--cyan); }
.split-guide section:last-child { border-top: 3px solid var(--gold); }
.guide-tag { color: var(--cyan-deep); font-size: 10px; font-weight: 900; letter-spacing: 0.2em; }
.split-guide section:last-child .guide-tag { color: var(--gold-deep); }
.split-guide h3 { margin: 12px 0 20px; color: var(--ink-strong); font-size: 21px; }
.split-guide ul { display: grid; gap: 13px; margin: 0; padding: 0; color: var(--muted); font-size: 14px; list-style: none; }
.split-guide li { position: relative; padding-left: 18px; }
.split-guide li::before { position: absolute; top: 0.72em; left: 0; width: 6px; height: 6px; background: var(--cyan); content: ""; transform: rotate(45deg); }
.split-guide section:last-child li::before { background: var(--gold); }
.flow-strip { display: flex; align-items: center; justify-content: center; gap: 15px; margin-top: 16px; padding: 16px 20px; border: 1px solid var(--line); background: rgba(255, 255, 255, 0.64); color: var(--ink); font-size: 12px; font-weight: 800; }
.flow-strip i { color: var(--cyan); font-size: 18px; font-style: normal; }

.ai-entry { display: grid; grid-template-columns: minmax(0, 1.04fr) minmax(330px, 0.96fr); overflow: hidden; border: 1px solid #25363d; background: #142229; color: #dce8eb; box-shadow: 0 24px 60px rgba(7, 20, 25, 0.23); }
.ai-copy { padding: 38px 38px 42px; }
.route-label { margin: 0 0 8px; color: #6cd2e8; font-size: 10px; font-weight: 900; letter-spacing: 0.2em; }
.route-path { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; margin: 0 0 28px; }
.route-path span { padding: 5px 9px; border: 1px solid #3b545e; background: rgba(255, 255, 255, 0.04); color: #eff8fa; font-size: 11px; font-weight: 800; }
.route-path b { color: #6cd2e8; }
.ai-copy h3 { margin: 0 0 14px; color: #fff; font-size: 24px; line-height: 1.35; }
.ai-copy > p:not(.route-label, .route-path) { color: #9eb2ba; font-size: 14px; }
.ai-copy ol { display: grid; gap: 10px; margin: 22px 0 0; padding-left: 22px; color: #b9c9ce; font-size: 13px; }
.editor-sketch { align-self: center; margin: 34px 34px 34px 0; border: 1px solid #3e555e; background: #0d191e; box-shadow: 0 18px 42px rgba(0, 0, 0, 0.32); }
.sketch-title { display: flex; align-items: center; gap: 6px; height: 36px; padding: 0 12px; border-bottom: 1px solid #30434b; }
.sketch-title i { width: 7px; height: 7px; border-radius: 50%; background: #3e555e; }
.sketch-title i:first-child { background: #08a9ce; } .sketch-title i:nth-child(2) { background: #c7942b; }
.sketch-title span { margin-left: auto; color: #68808a; font-size: 7px; font-weight: 900; letter-spacing: 0.16em; }
.sketch-body { display: grid; grid-template-columns: 34px 1fr 42%; min-height: 300px; }
.sketch-rail { display: grid; align-content: start; justify-items: center; gap: 14px; padding-top: 18px; border-right: 1px solid #283b42; }
.sketch-rail b { width: 11px; height: 11px; border: 1px solid #4b626b; }
.sketch-rail b:first-child { border-color: #08a9ce; background: rgba(8, 169, 206, 0.2); }
.sketch-code { display: grid; align-content: start; gap: 18px; padding: 32px 22px; }
.sketch-code span { height: 6px; border-radius: 4px; background: #33484f; }
.sketch-code span:nth-child(2) { width: 74%; background: #18596a; } .sketch-code span:nth-child(3) { width: 86%; } .sketch-code span:nth-child(4) { width: 58%; background: #685627; } .sketch-code span:nth-child(5) { width: 70%; }
.sketch-chat { padding: 20px 16px; border-left: 1px solid #283b42; background: #111f25; }
.sketch-chat strong { color: #6cd2e8; font-size: 7px; letter-spacing: 0.15em; }
.sketch-chat p { height: 44px; margin: 16px 0 10px; border-left: 2px solid #08a9ce; background: #192c33; }
.sketch-chat p:nth-child(3) { width: 78%; height: 25px; border-color: #c7942b; }
.sketch-chat em { display: block; height: 34px; margin-top: 62px; border: 1px solid #3b5159; }

.faq-list { border-top: 1px solid var(--line-strong); }
.faq-list details { border-bottom: 1px solid var(--line-strong); background: rgba(255, 255, 255, 0.38); }
.faq-list details[open] { background: rgba(255, 255, 255, 0.82); box-shadow: var(--shadow-small); }
.faq-list summary { display: flex; align-items: center; justify-content: space-between; gap: 20px; padding: 20px 22px; color: var(--ink-strong); cursor: pointer; font-size: 15px; font-weight: 800; list-style: none; }
.faq-list summary::-webkit-details-marker { display: none; }
.faq-list summary i { display: grid; width: 28px; height: 28px; flex: 0 0 auto; place-items: center; border: 1px solid var(--line-strong); color: var(--cyan-deep); font-size: 18px; font-style: normal; font-weight: 400; transition: transform 160ms ease, background 160ms ease; }
.faq-list details[open] summary i { background: var(--cyan); color: #fff; transform: rotate(45deg); }
.faq-list details > p { max-width: 790px; margin: -2px 70px 0 22px; padding: 0 0 22px; color: var(--muted); font-size: 14px; }

.privacy-section { margin-bottom: 0; padding: 42px; border: 1px solid rgba(8, 169, 206, 0.26); background: linear-gradient(135deg, rgba(8, 169, 206, 0.07), transparent 40%), rgba(255, 255, 255, 0.82); box-shadow: var(--shadow); clip-path: polygon(0 0, calc(100% - 28px) 0, 100% 28px, 100% 100%, 0 100%); }
.privacy-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 1px; margin-top: 28px; background: var(--line); }
.privacy-grid > div { min-height: 220px; padding: 24px; background: rgba(255, 255, 255, 0.94); }
.privacy-icon { display: inline-grid; width: 34px; height: 26px; place-items: center; background: var(--cyan-soft); color: var(--cyan-deep); font-size: 10px; font-weight: 900; }
.privacy-grid h3 { margin: 20px 0 9px; color: var(--ink-strong); font-size: 16px; }
.privacy-grid p { margin: 0; color: var(--muted); font-size: 13px; }
.privacy-note { margin: 20px 0 0; padding: 14px 18px; border-left: 3px solid var(--gold); background: var(--gold-soft); color: var(--gold-deep); font-size: 12px; font-weight: 700; }

.site-footer { display: grid; grid-template-columns: 1fr auto 1fr; align-items: center; gap: 20px; min-height: 120px; padding: 24px max(28px, calc((100vw - var(--content-width)) / 2)); border-top: 1px solid var(--line-strong); background: #e9eff1; color: var(--muted); font-size: 12px; }
.footer-brand { justify-self: start; color: var(--ink-strong); font-size: 14px; font-weight: 900; letter-spacing: 0.06em; }
.footer-brand span { margin-right: 7px; color: var(--cyan-deep); }
.site-footer p { margin: 0; } .site-footer > a:last-child { justify-self: end; font-weight: 800; }

@media (max-width: 1060px) {
  .hero { grid-template-columns: minmax(0, 1.2fr) minmax(300px, 0.8fr); }
  .manual-shell { grid-template-columns: 210px minmax(0, 1fr); gap: 30px; }
  .menu-route { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .ai-entry { grid-template-columns: 1fr; }
  .editor-sketch { margin: 0 34px 34px; }
  .privacy-grid { grid-template-columns: 1fr; }
  .privacy-grid > div { min-height: auto; }
}

@media (max-width: 820px) {
  :root { --header-height: 60px; }
  .site-header { grid-template-columns: 1fr auto; padding: 0 18px; }
  .header-nav { display: none; }
  .hero { grid-template-columns: 1fr; min-height: auto; padding: 72px 22px 58px; }
  .hero::after { right: 22px; left: 22px; }
  .hero-visual { width: min(78vw, 390px); margin: 48px auto 10px; justify-self: center; }
  .manual-shell { display: block; padding: 20px 18px 80px; }
  .chapter-rail { position: static; margin-bottom: 52px; }
  .chapter-rail nav { overflow-x: auto; }
  .chapter-rail nav > p, .rail-tip { display: none; }
  .chapter-rail ol { display: flex; width: max-content; }
  .chapter-rail a { padding: 10px 14px; border-left: 0; border-bottom: 2px solid transparent; }
  .chapter-rail a:hover { border-bottom-color: var(--cyan); }
  .step-list, .split-guide { grid-template-columns: 1fr; }
  .interaction-board { grid-template-columns: 1fr; }
  .button-demo { min-height: 300px; border-right: 0; border-bottom: 1px solid var(--line); }
  .privacy-section { padding: 32px 24px; }
}

@media (max-width: 560px) {
  body { font-size: 15px; }
  .brand-copy, .offline-badge i { display: none; }
  .hero { padding-top: 54px; }
  .hero h1 { font-size: clamp(42px, 13vw, 60px); }
  .hero-lede { font-size: 16px; }
  .hero-facts { gap: 9px 16px; }
  .manual-section { margin-bottom: 76px; }
  .section-heading { align-items: flex-start; }
  .section-number { width: 46px; height: 40px; }
  .step-list li { grid-template-columns: 1fr; min-height: auto; padding: 24px 20px; }
  .gesture-list { padding: 14px 20px; }
  .gesture-list dd { margin-left: 0; }
  .menu-route { grid-template-columns: 1fr; }
  .menu-route > div { min-height: auto; }
  .split-guide section { min-height: auto; padding: 26px 22px; }
  .flow-strip { flex-direction: column; } .flow-strip i { transform: rotate(90deg); }
  .ai-copy { padding: 30px 24px; }
  .editor-sketch { margin: 0 18px 22px; }
  .sketch-body { grid-template-columns: 30px 1fr; }
  .sketch-chat { display: none; }
  .faq-list summary { padding: 18px 14px; }
  .faq-list details > p { margin-right: 20px; margin-left: 14px; }
  .privacy-section { padding: 28px 18px; }
  .site-footer { grid-template-columns: 1fr; justify-items: center; text-align: center; }
  .footer-brand, .site-footer > a:last-child { justify-self: center; }
}

@media (prefers-reduced-motion: reduce) {
  html { scroll-behavior: auto; }
  *, *::before, *::after { animation-duration: 0.01ms !important; animation-iteration-count: 1 !important; transition-duration: 0.01ms !important; }
}

@media print {
  body { background: #fff; }
  .site-header, .hero-visual, .chapter-rail, .hero-actions, .site-footer { display: none; }
  .hero, .manual-shell { display: block; max-width: none; padding: 24px; }
  .hero { min-height: auto; }
  .manual-section { break-inside: avoid; }
  .step-list, .interaction-board, .hotkey-table-wrap, .split-guide section, .ai-entry, .privacy-section { box-shadow: none; }
}
"""


LEGACY_OUTPUTS = (
    "ACT_PLATFORM.html",
    "ACT_UI_PARITY_SDK.html",
    "AI_EDITOR.html",
    "AI_EDITOR_MCP.html",
    "GAME_STATE_API.html",
    "HYBRID_MEMORY_TCP.html",
    "MULTI_LANGUAGE_SCRIPTING.html",
    "PLUGIN_SDK.html",
    "assets/pygments.css",
)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    ASSETS_DIR.mkdir(parents=True, exist_ok=True)

    for relative_path in LEGACY_OUTPUTS:
        legacy_path = OUT_DIR / relative_path
        if legacy_path.is_file():
            legacy_path.unlink()
            print(f"removed {legacy_path.relative_to(OUT_DIR.parent)}")

    index_path = OUT_DIR / "index.html"
    style_path = ASSETS_DIR / "style.css"
    index_path.write_text(INDEX_HTML, encoding="utf-8")
    style_path.write_text(CSS, encoding="utf-8")
    print(f"wrote {index_path.relative_to(OUT_DIR.parent)}")
    print(f"wrote {style_path.relative_to(OUT_DIR.parent)}")


if __name__ == "__main__":
    main()
