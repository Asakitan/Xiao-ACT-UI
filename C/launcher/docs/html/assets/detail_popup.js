// SAO Auto 用户指南 — 条目详情弹窗。
// 页面里带 data-detail="key" 的内容项点击后弹出 SAO 面板，显示该条目的详细介绍。
// 内容以程序实际行为为准（托盘菜单、快捷键、GPU Hunt、启动参数等）。
(function () {
  'use strict';

  var DETAILS = {
    /* 01 快速开始 */
    q1: {
      section: '01 快速开始', title: '启动 SaoAuto.exe',
      html: '<p>双击安装目录里的 <code>SaoAuto.exe</code>。正常界面启动后播放 Link Start，随后进入主界面；首次使用还会打开本指南。</p><ul><li>屏幕右下角出现 NerveGear 圆形悬浮按钮。</li><li>系统托盘出现 SAO Auto 图标（如果托盘折叠了，先展开隐藏图标）。</li><li>默认使用 <kbd>HOME</kbd> / <kbd>INSERT</kbd> 两个全局快捷键。</li></ul><p>启动动画不等同于插件或在线 AI 服务已经连接；请以各面板的实际状态为准。</p>'
    },
    q2: {
      section: '01 快速开始', title: '打开主菜单',
      html: '<p>两种方式，效果一样：</p><ul><li>左键点击悬浮按钮。</li><li>按 <kbd>HOME</kbd>。</li></ul><p>菜单会围绕悬浮按钮展开。<kbd>INSERT</kbd> 管的是按钮本身显隐，不是开菜单。所有已启用功能和工具都从主菜单的入口页面进。</p>'
    },
    q3: {
      section: '01 快速开始', title: '启用插件',
      html: '<p>菜单里进「插件」页面，里面是所有已安装的插件。</p><ul><li>每个插件对应一个游戏或一组功能，把你正在玩的游戏对应的插件启用。</li><li>不用的插件保持停用，不会吃资源，之后随时可以再启用。</li><li>插件启用/停用后，它提供的菜单项、面板和快捷键会立即增减。</li></ul><p>界面上有插件自述和版本信息，先看一眼再启用。</p>'
    },
    q4: {
      section: '01 快速开始', title: '打开面板',
      html: '<p>面板是插件用来显示数据的小窗口，从主菜单或插件界面打开。多数游戏类面板需要按顺序做：</p><ul><li>先把游戏跑起来，进到有画面的场景。</li><li>再点插件界面上提供的「启动识别」一类的按钮。</li><li>数据能否读出取决于插件和游戏的匹配，出问题按插件提示排查。</li></ul>'
    },
    /* 02 托盘菜单 */
    t1: {
      section: '02 系统托盘菜单', title: '左键单击 / 右键托盘图标',
      html: '<p>左键单击和右键点击都会弹出 SAO Auto 菜单。菜单内容：设置、快捷键、关于 / 用户指南、退出，共四项。</p><p>托盘图标是悬浮按钮之外的第二个入口，按钮藏起来之后所有设置都从这里改。</p>'
    },
    t2: {
      section: '02 系统托盘菜单', title: '左键双击托盘图标',
      html: '<p>左键双击托盘图标打开 Settings（设置）。通过概览、外观、行为、音频、高级和配置分类查看对应内容；宽屏左右排列，窄屏上下排列。</p><p>切换分类不丢弃草稿。底部“应用更改”保存当前更改，“丢弃草稿”恢复已提交设置；不是只保存或丢弃当前分类。</p>'
    },
    t3: {
      section: '02 系统托盘菜单', title: '快捷键',
      html: '<p>每个快捷键使用独立卡片显示组合、状态和操作。默认两个：</p><ul><li><kbd>HOME</kbd> — 开关主菜单。</li><li><kbd>INSERT</kbd> — 隐藏 / 显示悬浮按钮。</li></ul><p>点击“录入组合键”再按下新组合，成功后自动保存并显示“已保存”；发生冲突或保存失败时显示相应提示。“恢复默认”只重置该项。</p>'
    },
    t4: {
      section: '02 系统托盘菜单', title: '关于 / 用户指南',
      html: '<p>这份离线手册显示在 SAO Auto 当前界面中（资源位于 <code>docs\\html\\index.html</code>），不会另开指南窗口或浏览器。加载失败时可在当前界面重试；右上角关闭按钮返回原来的面板。</p>'
    },
    t5: {
      section: '02 系统托盘菜单', title: '退出',
      html: '<p>彻底关闭 SAO Auto：</p><ul><li>托盘图标移除。</li><li>所有面板关闭。</li><li>全局快捷键注销。</li></ul><p>下次使用双击 <code>SaoAuto.exe</code> 即可，配置都会保留。</p>'
    },
    /* 03 快捷键 */
    h1: {
      section: '03 全局快捷键', title: 'HOME — 开关主菜单',
      html: '<p>按一下展开主菜单，再按一下收起。任何程序在前台时都有效。</p><p>菜单会围绕悬浮按钮的位置展开；如果按钮被 <kbd>INSERT</kbd> 藏起来了，先按 <kbd>INSERT</kbd> 把它叫出来，再按 <kbd>HOME</kbd>。</p>'
    },
    h2: {
      section: '03 全局快捷键', title: 'INSERT — 显隐悬浮按钮',
      html: '<p>画面被按钮挡住时按一下藏起来，再按一下显示回来。部分显示模式下，覆盖层会跟着一起隐藏。</p><p>想改成别的键：右键托盘图标 → 快捷键，录入新组合后确认卡片显示“已保存”。</p>'
    },
    /* 04 面板和插件 */
    p0: {
      section: '04 面板和插件', title: '插件是什么',
      html: '<p>插件是功能的来源：每个插件完成一类事（游戏数据读取、自动化脚本运行等）。SAO Auto 本体不直接提供这些功能，它负责把插件管理起来。</p><ul><li>启用：加载插件、注册它带的菜单/面板/快捷键。</li><li>停用：全部撤掉，直到你再次启用。</li></ul>'
    },
    p1: {
      section: '04 面板和插件', title: '按游戏启用插件',
      html: '<p>装好的插件会在「插件」页面列出，玩哪个游戏就启用哪个。多个插件可以同时启用，但同一款游戏一般只有一个对应插件。</p>'
    },
    p2: {
      section: '04 面板和插件', title: '停用不用的插件',
      html: '<p>停用后插件不加载、不占资源。已停用的插件在列表里保持原样，随时可以重新启用，配置不会丢。</p>'
    },
    p3: {
      section: '04 面板和插件', title: '启停影响什么',
      html: '<p>启用：插件声明的菜单入口、面板、快捷键出现在系统里。</p><p>停用：这些入口立刻消失，已经打开的面板由插件自行处理关闭。</p>'
    },
    p4: {
      section: '04 面板和插件', title: '插件的统一管理',
      html: '<p>主菜单 →「插件」页面。启用/停用、看说明和版本、检查运行状态都在这一处。各插件自己的配置在各自界面里。</p>'
    },
    pn0: {
      section: '04 面板和插件', title: '面板是什么',
      html: '<p>面板是插件展示数据的窗口，最典型的是游戏面板（血条、坐标、状态等）。面板不产生数据，只显示数据——数据来自插件。</p><ul><li>面板从主菜单或插件界面打开。</li><li>单个面板可以独立关闭。</li><li>每块面板的位置和大小通常可以拖动调整。</li></ul>'
    },
    pn1: {
      section: '04 面板和插件', title: '打开面板',
      html: '<p>路径一：主菜单里直接找面板入口。</p><p>路径二：进对应的插件界面，从它的面板列表打开。</p><p>两个入口开出来的是同一块面板。</p>'
    },
    pn2: {
      section: '04 面板和插件', title: '关掉单个面板',
      html: '<p>关闭不需要的面板只影响这一块窗口，插件和它的其他面板照常运行。</p>'
    },
    pn3: {
      section: '04 面板和插件', title: '面板被游戏挡住',
      html: '<p>多数面板自带置顶（always-on-top）和透明选项，在面板自己的设置里改。也可以：</p><ul><li>按 <kbd>HOME</kbd> 收起主菜单减少遮挡。</li><li>把暂时不用的覆盖层关掉。</li></ul>'
    },
    pn4: {
      section: '04 面板和插件', title: '面板显示什么',
      html: '<p>具体字段由对应插件决定：不同游戏、不同插件的面板内容不一样。以插件界面和面板上的实际显示为准。</p>'
    },
    /* 05 AI Editor */
    a1: {
      section: '05 AI Editor', title: '配置模型服务',
      html: '<p>第一次打开先进入“编辑器设置”，确认模型服务和基础偏好。先选择系统、工作区或插件范围，再选择分类；点击“应用”保存到当前范围。关闭设置面板只保留草稿，不代表已经保存。</p>'
    },
    a2: {
      section: '05 AI Editor', title: '选模型和工作区',
      html: '<p>回到 SAO AI Editor 主面板，按当前界面选择需要的子面板。需要调整编辑与文件行为时，从 Settings → Editor & Files 进入相应设置。</p>'
    },
    a3: {
      section: '05 AI Editor', title: '对话与改文件',
      html: '<p>AI Editor 使用完整 HTML 界面，在文件区编辑内容，在助手区对话，并通过“工具与状态”查看检查器、历史、日志与工具。</p><p>打开文件和另存为使用当前界面内的文件选择面板；保存到已有文件需要明确确认覆盖。后端未连接时显示实际离线或错误状态。</p>'
    },
    a4: {
      section: '05 AI Editor', title: '备份与隐私',
      html: '<p>重要文件动手前先备份。使用在线服务时，你发上去的内容会离开本机，只发可以外发的内容。各服务如何处理数据以它的隐私政策为准。</p>'
    },
    /* 06 GPU Hunt */
    g1: {
      section: '06 GPU Hunt', title: '打开 GPU Hunt 子面板',
      html: '<p>先从 SAO 菜单 → Tools → AI Editor (LLM) 打开 SAO AI Editor，再按当前界面查找 GPU Hunt 子面板。它是否可用，以面板当前显示为准。</p>'
    },
    g2: {
      section: '06 GPU Hunt', title: '查看当前入口',
      html: '<p>GPU Hunt 作为子面板提供时，连接、定位和刷新控件以当前面板显示为准。本指南不展开内部工具步骤。</p>'
    },
    g3: {
      section: '06 GPU Hunt', title: '查看当前状态',
      html: '<p>面板显示可用或锁定状态时，按当前界面提供的提示查看结果。状态字段和结果内容可能随版本或当前场景变化。</p>'
    },
    g4: {
      section: '06 GPU Hunt', title: '场景或目标变化后',
      html: '<p>场景或目标变化后，按子面板当前提供的刷新、重置或关闭提示处理。若当前界面没有对应控件，以面板的实际状态提示为准。</p>'
    },
    /* 07 启动参数 */
    c1: {
      section: '07 启动参数', title: '--safe-mode',
      html: '<p>安全模式会跳过插件和扩展组件，只启动核心界面。怀疑某个扩展导致异常时，用 <code>SaoAuto.exe --safe-mode</code> 验证核心是否正常。</p>'
    },
    c2: {
      section: '07 启动参数', title: '--config=<path>',
      html: '<p>指定启动时读取的 provider 配置文件路径，例如 SaoAuto.provider.json。它用于 provider、更新和插件等启动配置，不等同于普通 Settings 面板使用的用户设置文件。</p>'
    },
    c3: {
      section: '07 启动参数', title: '--log-level=<lvl>',
      html: '<p>设置日志输出级别，取值 <code>trace / debug / info / warn / error / critical</code>，从啰嗦到干净。排障时常用 <code>--log-level=debug</code>，日志写在程序目录的 logs 下。</p>'
    },
    c4: {
      section: '07 启动参数', title: '--version / -v',
      html: '<p>向终端或重定向的输出文件写入版本信息后退出，不打开新窗口，也不进入正常初始化流程；没有终端时写入调试输出。</p>'
    },
    c5: {
      section: '07 启动参数', title: '--help / -h',
      html: '<p>向终端或重定向的输出文件写入内置帮助后退出，不打开新窗口；没有终端时写入调试输出。</p>'
    },
    /* 09 隐私说明 */
    v1: {
      section: '09 隐私说明', title: '数据保存在本机',
      html: '<p>配置、日志、面板布局等数据都写在本机：</p><ul><li>配置文件在程序目录（<code>settings.json</code>、<code>SaoAuto.provider.json</code> 等）。</li><li>日志在程序目录 <code>logs</code> 下。</li></ul><p>程序不主动上传这些内容；保留、查看或删除都由你自己决定。</p>'
    },
    v2: {
      section: '09 隐私说明', title: '联网范围清楚可控',
      html: '<p>正常启动可能连接 SAO Auto 服务完成许可校验，并检查是否有可用更新；这些请求不上传本机配置或日志。</p><p>使用在线 AI 时，还会向所选服务发送你主动提交的内容。各服务如何保存和使用数据，以其隐私说明为准。这份用户指南本身完全离线。</p>'
    },
    v3: {
      section: '09 隐私说明', title: '手册只覆盖操作',
      html: '<p>本手册只写用户能接触到的操作：怎么开程序、菜单是什么、按钮和常用参数怎么用。实现细节和内部数据结构不在这里讨论。</p>'
    }
  };

  var overlay = null;
  var closeTimer = 0;
  var focusTimer = 0;
  var activeTrigger = null;
  var savedScroll = null;
  var isolatedNodes = [];

  function isOpen() {
    return overlay && overlay.classList.contains('visible');
  }

  function setTriggerExpanded(trigger, expanded) {
    if (trigger && trigger.nodeType === 1) {
      trigger.setAttribute('aria-expanded', expanded ? 'true' : 'false');
    }
  }

  function build() {
    overlay = document.createElement('div');
    overlay.id = 'detail-popup';
    overlay.className = 'detail-overlay detail-popup';
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    overlay.setAttribute('aria-hidden', 'true');
    overlay.setAttribute('aria-labelledby', 'dp-title');
    overlay.setAttribute('aria-describedby', 'dp-body');

    var backdrop = document.createElement('div');
    backdrop.className = 'dp-backdrop';
    backdrop.setAttribute('data-close', '1');
    var panel = document.createElement('div');
    panel.className = 'dp-panel';
    panel.setAttribute('role', 'document');
    panel.tabIndex = -1;
    var closeButton = document.createElement('button');
    closeButton.type = 'button';
    closeButton.className = 'dp-close';
    closeButton.setAttribute('data-close', '1');
    closeButton.setAttribute('aria-label', '关闭');
    closeButton.textContent = '✕';
    var kicker = document.createElement('p');
    kicker.className = 'dp-kicker';
    var title = document.createElement('h3');
    title.id = 'dp-title';
    title.className = 'dp-title';
    var body = document.createElement('div');
    body.id = 'dp-body';
    body.className = 'dp-body';
    panel.appendChild(closeButton);
    panel.appendChild(kicker);
    panel.appendChild(title);
    panel.appendChild(body);
    overlay.appendChild(backdrop);
    overlay.appendChild(panel);
    document.body.appendChild(overlay);

    overlay.addEventListener('click', function (e) {
      var closeTrigger = e.target && e.target.closest ? e.target.closest('[data-close]') : null;
      if (closeTrigger) { close(); }
    });
  }

  function renderBody(markup) {
    var body = overlay.querySelector('.dp-body');
    var parsed = new DOMParser().parseFromString(markup, 'text/html');
    while (body.firstChild) { body.removeChild(body.firstChild); }
    Array.prototype.slice.call(parsed.body.childNodes).forEach(function (node) {
      body.appendChild(document.importNode(node, true));
    });
  }

  function focusDialog() {
    if (!isOpen()) { return; }
    var closeButton = overlay.querySelector('.dp-close');
    if (closeButton) {
      try {
        closeButton.focus({ preventScroll: true });
      } catch (error) {
        closeButton.focus();
      }
    }
  }

  function focusableElements() {
    var candidates = Array.prototype.slice.call(
      overlay.querySelectorAll('a[href], area[href], button, input, select, textarea, [contenteditable="true"], [tabindex]'));
    return candidates.filter(function (element) {
      var style = window.getComputedStyle(element);
      return !element.disabled && element.getAttribute('tabindex') !== '-1' &&
        element.getAttribute('aria-hidden') !== 'true' && style.visibility !== 'hidden' &&
        style.display !== 'none' && element.getClientRects().length > 0;
    });
  }

  function isolatePage() {
    if (isolatedNodes.length > 0) { return; }
    Array.prototype.slice.call(document.body.children).forEach(function (node) {
      if (node === overlay || node.tagName === 'SCRIPT') { return; }
      isolatedNodes.push({
        node: node,
        hadInert: node.hasAttribute('inert'),
        ariaHidden: node.getAttribute('aria-hidden')
      });
      node.setAttribute('inert', '');
      node.setAttribute('aria-hidden', 'true');
    });
  }

  function restorePage() {
    isolatedNodes.forEach(function (state) {
      if (!state.hadInert) { state.node.removeAttribute('inert'); }
      if (state.ariaHidden === null) {
        state.node.removeAttribute('aria-hidden');
      } else {
        state.node.setAttribute('aria-hidden', state.ariaHidden);
      }
    });
    isolatedNodes.length = 0;
  }

  function open(key, trigger) {
    var item = DETAILS[key];
    if (!item) { return; }
    if (!overlay) { build(); }
    window.clearTimeout(closeTimer);
    closeTimer = 0;
    overlay.classList.remove('closing');
    if (!isOpen()) {
      savedScroll = {
        x: window.scrollX || window.pageXOffset || 0,
        y: window.scrollY || window.pageYOffset || 0
      };
    }
    setTriggerExpanded(activeTrigger, false);
    activeTrigger = trigger || activeTrigger;
    overlay.querySelector('.dp-kicker').textContent = item.section;
    overlay.querySelector('.dp-title').textContent = item.title;
    renderBody(item.html);
    overlay.setAttribute('aria-hidden', 'false');
    if (activeTrigger) {
      activeTrigger.setAttribute('aria-controls', 'detail-popup');
      setTriggerExpanded(activeTrigger, true);
    }
    overlay.classList.add('visible');
    document.body.classList.add('dp-locked');
    isolatePage();
    window.clearTimeout(focusTimer);
    focusTimer = window.setTimeout(function () {
      focusTimer = 0;
      focusDialog();
    }, 0);
    if (window.saoSfxPlay) {
      window.saoSfxPlay('panel', 0.4);
      if (window.saoSfxUnlock) { window.saoSfxUnlock(); }
    }
  }

  function close() {
    if (!isOpen()) { return; }
    var trigger = activeTrigger;
    var restore = savedScroll;
    window.clearTimeout(focusTimer);
    focusTimer = 0;
    overlay.classList.add('closing');
    overlay.classList.remove('visible');
    overlay.setAttribute('aria-hidden', 'true');
    document.body.classList.remove('dp-locked');
    restorePage();
    setTriggerExpanded(trigger, false);
    activeTrigger = null;
    savedScroll = null;
    if (trigger && document.contains(trigger)) {
      try {
        trigger.focus({ preventScroll: true });
      } catch (error) {
        trigger.focus();
      }
    }
    if (restore) { window.scrollTo(restore.x, restore.y); }
    if (window.saoSfxPlay) { window.saoSfxPlay('alert_close', 0.38); }
    window.clearTimeout(closeTimer);
    closeTimer = window.setTimeout(function () {
      if (overlay && !isOpen()) { overlay.classList.remove('closing'); }
      closeTimer = 0;
    }, 260);
  }

  document.addEventListener('keydown', function (e) {
    if (!isOpen()) { return; }
    if (e.key === 'Escape') {
      e.preventDefault();
      close();
      return;
    }
    if (e.key !== 'Tab') { return; }
    var focusable = focusableElements();
    if (focusable.length === 0) {
      e.preventDefault();
      overlay.querySelector('.dp-panel').focus();
      return;
    }
    var first = focusable[0];
    var last = focusable[focusable.length - 1];
    if (e.shiftKey && (document.activeElement === first || !overlay.contains(document.activeElement))) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (document.activeElement === last || !overlay.contains(document.activeElement))) {
      e.preventDefault();
      first.focus();
    }
  });

  document.addEventListener('focusin', function (e) {
    if (isOpen() && !overlay.contains(e.target)) { focusDialog(); }
  });

  Array.prototype.slice.call(document.querySelectorAll('[data-detail]')).forEach(function (trigger) {
    var key = trigger.getAttribute('data-detail');
    if (!DETAILS[key]) { return; }
    var descriptionGroup = trigger.tagName === 'DIV' && trigger.parentElement &&
      trigger.parentElement.tagName === 'DL';
    if (trigger.matches('li, tr, dt, dd, td, th') || descriptionGroup) {
      var container = trigger;
      if (trigger.tagName === 'TR') { container = trigger.cells[trigger.cells.length - 1]; }
      else if (descriptionGroup) { container = trigger.querySelector('dd'); }
      else if (trigger.tagName === 'LI') { container = trigger.querySelector('div') || trigger; }
      if (!container) { return; }
      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'detail-trigger';
      button.textContent = '查看详情';
      button.setAttribute('data-detail', key);
      container.appendChild(button);
      trigger.removeAttribute('data-detail');
      trigger = button;
    }
    if (DETAILS[key]) { trigger.setAttribute('aria-label', DETAILS[key].title); }
    if (trigger.tagName !== 'BUTTON') { trigger.setAttribute('role', 'button'); }
    trigger.setAttribute('tabindex', '0');
    trigger.setAttribute('aria-haspopup', 'dialog');
    trigger.setAttribute('aria-expanded', 'false');
    trigger.setAttribute('aria-controls', 'detail-popup');
  });

  document.addEventListener('click', function (e) {
    var trigger = e.target && e.target.closest ? e.target.closest('[data-detail]') : null;
    if (!trigger) { return; }
    var key = trigger.getAttribute('data-detail');
    if (DETAILS[key]) { open(key, trigger); }
  });

  document.addEventListener('keydown', function (e) {
    var trigger = e.target && e.target.closest ? e.target.closest('[data-detail]') : null;
    if (!trigger || (e.key !== 'Enter' && e.key !== ' ')) { return; }
    if (trigger.tagName === 'BUTTON') { return; }
    var key = trigger.getAttribute('data-detail');
    if (DETAILS[key]) {
      e.preventDefault();
      open(key, trigger);
    }
  });

  document.addEventListener('mouseover', function (e) {
    var trigger = e.target && e.target.closest ? e.target.closest('[data-detail]') : null;
    if (!trigger) { return; }
    trigger.setAttribute('title', '点击查看详细介绍');
  }, { passive: true });
})();