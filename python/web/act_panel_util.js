(function () {
    'use strict';

    function finite(value, fallback) {
        var number = Number(value);
        return Number.isFinite(number) ? number : fallback;
    }

    function fmtClock(ms) {
        var total = Math.max(0, Math.floor(finite(ms, 0) / 1000));
        var minutes = Math.floor(total / 60);
        var seconds = total % 60;
        return String(minutes).padStart(2, '0') + ':' + String(seconds).padStart(2, '0');
    }

    function fmtDur(ms) {
        var seconds = Math.max(0, finite(ms, 0) / 1000);
        if (seconds < 60) return seconds.toFixed(seconds < 10 ? 1 : 0) + 's';
        var minutes = Math.floor(seconds / 60);
        var rest = Math.floor(seconds % 60);
        return minutes + 'm ' + String(rest).padStart(2, '0') + 's';
    }

    function fmtRel(ms) {
        var seconds = finite(ms, 0) / 1000;
        var sign = seconds < 0 ? '-' : '+';
        seconds = Math.abs(seconds);
        return sign + seconds.toFixed(seconds < 10 ? 1 : 0) + 's';
    }

    function fmtSigned(value, digits) {
        var number = finite(value, 0);
        var sign = number > 0 ? '+' : '';
        return sign + number.toFixed(digits == null ? 0 : digits);
    }

    window.PanelTime = { fmtClock: fmtClock, fmtDur: fmtDur, fmtRel: fmtRel, fmtSigned: fmtSigned };
    window.PanelText = window.PanelText || {};
}());
