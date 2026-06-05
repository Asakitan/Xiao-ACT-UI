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
}());
