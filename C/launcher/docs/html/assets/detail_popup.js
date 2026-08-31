// SAO Auto 用户指南 — 条目详情弹窗。
// 页面里带 data-detail="key" 的内容项点击后弹出 SAO 面板，显示该条目的详细介绍。
// 内容以程序实际行为为准（托盘菜单、快捷键、GPU Hunt、启动参数等）。
(function () {
  'use strict';

  var DETAILS = {
    /* 01 快速开始 */
    q1: {
      section: '01 快速开始', title: '启动 SaoAuto.exe',
      html: '<p>双击安装目录里的 <code>SaoAuto.exe</code>。程序没有欢迎窗口，启动后直接进后台：</p><ul><li>屏幕左上角出现 NerveGear 圆形悬浮按钮。</li><li>系统托盘出现 SAO Auto 图标（如果托盘折叠了，先点「显示隐藏的图标」展开）。</li><li>程序同时注册 <kbd>HOME</kbd> / <kbd>INSERT</kbd> 两个全局快捷键。</li></ul><p>什么都没看到的话，多半是进程没起来：打开任务管理器确认 <code>SaoAuto.exe</code> 在运行。</p>'
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
      section: '02 系统托盘菜单', title: '左键 / 右键托盘图标',
      html: '<p>左键和右键弹的是同一个菜单，没有任何分别。菜单内容：设置、快捷键、关于 / 用户指南、退出，共四项。</p><p>托盘图标是悬浮按钮之外的第二个入口，按钮藏起来之后所有设置都从这里改。</p>'
    },
    t2: {
      section: '02 系统托盘菜单', title: '设置',
      html: '<p>常规设置都在这：启动选项、显示行为、面板主题等。</p><ul><li>启动选项：开机自启、启动后是否直接显示悬浮按钮这一类的开关。</li><li>显示：深浅主题、按钮位置和缩放。</li><li>面板主题：各面板统一的视觉风格。</li></ul><p>改完即时生效，个别选项需要重启程序。</p>'
    },
    t3: {
      section: '02 系统托盘菜单', title: '快捷键',
      html: '<p>显示当前生效的全局快捷键并支持修改。默认两个：</p><ul><li><kbd>HOME</kbd> — 开关主菜单。</li><li><kbd>INSERT</kbd> — 隐藏 / 显示悬浮按钮。</li></ul><p>改键后点保存；如果和游戏或其他软件冲突会有提示。配置文件里残留的 <code>F5 / F9 / F10 / F11</code> 是旧版本兼容项，实际是否生效以这个面板显示为准。</p>'
    },
    t4: {
      section: '02 系统托盘菜单', title: '关于 / 用户指南',
      html: '<p>在 SAO Auto 内打开这份离线手册（存放于程序目录 <code>docs\\html\\index.html</code>）；内置浏览组件不可用时才交给默认浏览器。不联网也能看，本手册不包含任何需要外联的内容。</p>'
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
      html: '<p>游戏画面被按钮挡住时按一下藏起来，再按一下显示回来。部分显示模式下，覆盖层会跟着一起隐藏。</p><p>想改成别的键：右键托盘图标 → 快捷键，选一个游戏没用到的组合保存。</p>'
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
      html: '<p>第一次打开先进设置，把要用的模型服务配好。可以用本地部署的模型，也可以填在线服务的地址和密钥。配置保存在本机，换电脑需要重新配。</p>'
    },
    a2: {
      section: '05 AI Editor', title: '选模型和工作区',
      html: '<p>选好当前对话使用的模型和对话模式（普通问答 / 指令模式等），再确认打开的工作区路径正确——涉及改文件时，AI 只在这个工作区范围内操作。</p>'
    },
    a3: {
      section: '05 AI Editor', title: '对话与改文件',
      html: '<p>在对话框里把要做的说清楚。涉及修改文件的请求，AI 会先列出它计划改动的位置和内容，确认后才写入。不要直接确认没看过的改动。</p>'
    },
    a4: {
      section: '05 AI Editor', title: '备份与隐私',
      html: '<p>重要文件动手前先备份。使用在线服务时，你发上去的内容会离开本机，只发可以外发的内容。各服务如何处理数据以它的隐私政策为准。</p>'
    },
    /* 06 GPU Hunt */
    g1: {
      section: '06 GPU Hunt', title: '填 PID',
      html: '<p>PID 是进程 ID。打开任务管理器 → 详细信息（列「PID」）找到游戏本体进程，把它填到面板顶部的输入框。</p><p>常见错误是填成启动器或其他后台进程——认准游戏本体的进程名。</p>'
    },
    g2: {
      section: '06 GPU Hunt', title: 'Attach 与 Auto Tick',
      html: '<p><strong>Attach</strong>：加载指定的游戏进程，状态变为 <em>attached</em>。</p><p><strong>Auto Tick</strong>：OFF 时手动单帧，点成 ON 后自动以 60Hz 刷新。锁定过程中建议保持 ON。</p>'
    },
    g3: {
      section: '06 GPU Hunt', title: 'Locked 是什么意思',
      html: '<p>状态变为 <em>locked</em> 表示场景相机已经锁定，面板开始持续显示数据。游戏在渲染 3D 画面时，一般几秒内锁定；停在主菜单或加载画面时可能没有结果。</p>'
    },
    g4: {
      section: '06 GPU Hunt', title: 'Invalidate / Detach',
      html: '<p><strong>Invalidate</strong>：切场景、换地图或游戏重启后，点它清掉旧结果并重新定位。</p><p><strong>Detach</strong>：结束本次会话并断开进程。面板不再使用时保持 Detach 即可。</p>'
    },
    /* 07 启动参数 */
    c1: {
      section: '07 启动参数', title: '--safe-mode',
      html: '<p>安全模式会跳过插件和扩展组件，只启动核心界面。怀疑某个扩展导致异常时，用 <code>SaoAuto.exe --safe-mode</code> 验证核心是否正常。</p>'
    },
    c2: {
      section: '07 启动参数', title: '--config=<path>',
      html: '<p>指定启动要用哪个配置文件。不填时用安装目录下的默认配置。适合多套配置切换或把配置放在别处的情况。</p>'
    },
    c3: {
      section: '07 启动参数', title: '--log-level=<lvl>',
      html: '<p>设置日志输出级别，取值 <code>trace / debug / info / warn / error / critical</code>，从啰嗦到干净。排障时常用 <code>--log-level=debug</code>，日志写在程序目录的 logs 下。</p>'
    },
    c4: {
      section: '07 启动参数', title: '--version / -v',
      html: '<p>打开版本信息窗口，关闭窗口后程序退出，不进入正常初始化流程。</p>'
    },
    c5: {
      section: '07 启动参数', title: '--help / -h',
      html: '<p>打开内置帮助窗口，显示更多可用选项。关闭窗口后程序退出。</p>'
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

  /* ── DOM：单例面板 ── */
  var overlay = null;
  function build() {
    overlay = document.createElement('div');
    overlay.className = 'detail-popup';
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');
    overlay.innerHTML =
      '<div class="dp-backdrop" data-close="1"></div>' +
      '<div class="dp-panel" role="document">' +
      '<button type="button" class="dp-close" data-close="1" aria-label="关闭">✕</button>' +
      '<p class="dp-kicker"></p>' +
      '<h3 class="dp-title"></h3>' +
      '<div class="dp-body"></div>' +
      '</div>';
    document.body.appendChild(overlay);

    overlay.addEventListener('click', function (e) {
      var closeTrigger = e.target && e.target.closest ? e.target.closest('[data-close]') : null;
      if (closeTrigger) { close(); }
    });
  }

  function open(key) {
    var item = DETAILS[key];
    if (!item) { return; }
    if (!overlay) { build(); }
    overlay.querySelector('.dp-kicker').textContent = item.section;
    overlay.querySelector('.dp-title').textContent = item.title;
    overlay.querySelector('.dp-body').innerHTML = item.html;
    overlay.classList.remove('closing');
    overlay.classList.add('open');
    document.body.classList.add('dp-locked');
    if (window.saoSfxPlay) {
      window.saoSfxPlay('panel', 0.4);
      if (window.saoSfxUnlock) { window.saoSfxUnlock(); }
    }
  }

  function close() {
    if (!overlay || !overlay.classList.contains('open')) { return; }
    overlay.classList.add('closing');
    overlay.classList.remove('open');
    document.body.classList.remove('dp-locked');
    if (window.saoSfxPlay) { window.saoSfxPlay('alert_close', 0.38); }
    window.setTimeout(function () {
      overlay.classList.remove('closing');
    }, 260);
  }

  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape' && overlay && overlay.classList.contains('open')) {
      close();
    }
  });

  /* 事件代理：点击带 data-detail 的内容项 */
  document.addEventListener('click', function (e) {
    var el = e.target && e.target.closest ? e.target.closest('[data-detail]') : null;
    if (!el) { return; }
    var key = el.getAttribute('data-detail');
    if (DETAILS[key]) { open(key); }
  });

  // 鼠标悬停提示：可点项显示详情光标文案
  document.addEventListener('mouseover', function (e) {
    var el = e.target && e.target.closest ? e.target.closest('[data-detail]') : null;
    if (!el) { return; }
    el.setAttribute('title', '点击查看详细介绍');
  }, { passive: true });
})();