/* Shared readable-time helpers for every ACT WebView panel (window.ActTime).
 *
 * Mirrors gui_modules/sao_panel_components.py fmt_clock/fmt_dur/fmt_rel/fmt_signed
 * NUMERICALLY EXACTLY (same decimals, same 60s/3600s thresholds, same '--'/'T0')
 * so the Entity(Tk) and WebView surfaces render the same time field identically.
 *
 * Three time quantities, never mixed:
 *   - ABSOLUTE epoch-ms (row/event/point time_ms) -> fmtClock  'HH:MM:SS'
 *   - SIGNED offset/delta (relative_ms, time-vs-first) -> fmtRel/fmtSigned '+2.6s'
 *   - pure DURATION (time_range_ms, span, before_ms) -> fmtDur  '2.6s' / 'M:SS'
 * The numeric magnitude formatter fmt()/_fmt() must NEVER touch a *_time_ms field.
 */
(function () {
    'use strict';

    function pad2(n) { return (n < 10 ? '0' : '') + n; }

    function fmtClock(timeMs, withSeconds) {
        var ms = parseInt(timeMs, 10);
        if (!isFinite(ms) || ms <= 0) return '--';
        var d = new Date(ms);
        var s = pad2(d.getHours()) + ':' + pad2(d.getMinutes());
        if (withSeconds !== false) s += ':' + pad2(d.getSeconds());
        return s;
    }

    function fmtDur(ms) {
        var t = Math.max(0, parseFloat(ms) || 0) / 1000;
        if (t < 60) return t.toFixed(1) + 's';
        if (t < 3600) return Math.floor(t / 60) + ':' + pad2(Math.floor(t % 60));
        return Math.floor(t / 3600) + ':' + pad2(Math.floor((t % 3600) / 60)) + ':' + pad2(Math.floor(t % 60));
    }

    function fmtRel(timeMs, baseMs) {
        var base = parseInt(baseMs, 10) || 0;
        if (!base) return fmtClock(timeMs);
        var delta = (parseInt(timeMs, 10) || 0) - base;
        return (delta >= 0 ? '+' : '-') + fmtDur(Math.abs(delta));
    }

    function fmtSigned(deltaMs) {
        var d = parseInt(deltaMs, 10) || 0;
        if (d === 0) return 'T0';
        return (d > 0 ? '+' : '-') + fmtDur(Math.abs(d));
    }

    window.ActTime = { fmtClock: fmtClock, fmtDur: fmtDur, fmtRel: fmtRel, fmtSigned: fmtSigned };

    // ── 技术术语 -> 人话（与 gui_modules/sao_panel_components.py 的映射一致）──
    var TOPIC_CN = {
        damage: '伤害', heal: '治疗', skill: '技能', actor_skill: '技能',
        monster: '怪物', monster_skill: '怪物技能', boss: 'Boss', boss_state: 'Boss状态',
        boss_mechanic: 'Boss机制', boss_mechanic_skill: 'Boss机制', dungeon: '地牢',
        scene: '场景', death: '死亡', buff: '增益', player_buff: '玩家增益',
        factor_buff: '因子增益', trigger: '触发', timer: '计时', target: '目标',
        log: '日志', event: '事件', shield: '护盾', mitigation: '减伤',
        incoming_damage: '承受伤害', healing: '治疗', ultimate_skill: '终极技',
        environment_skill: '环境技能', field_marker: '场地标记'
    };
    var SOURCE_CN = {
        tcp: '封包', entity: '实体', mem: '内存', memory: '内存', history: '历史',
        replay: '回放', ui: '界面', offline_import: '离线导入', plugin: '插件', unknown: '未知'
    };
    function topic(v, dflt) {
        var t = String(v == null ? '' : v).trim();
        if (!t) return dflt || '事件';
        return TOPIC_CN[t.toLowerCase()] || t;
    }
    function source(v, dflt) {
        var t = String(v == null ? '' : v).trim();
        if (!t) return dflt || '未知';
        return SOURCE_CN[t.toLowerCase()] || t;
    }
    window.ActText = { topic: topic, source: source };
}());
