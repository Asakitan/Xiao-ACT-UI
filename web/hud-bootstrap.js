// S189 — HUD page bootstrap.
//
// Each HUD page (hp.html, stamina.html, dps.html, boss_hp.html) loads
// this after bridge.js. It subscribes to the C# event broadcaster
// and dispatches into per-page update functions whose names the
// Python panels already defined (`updateHP`, `updateSTA`, etc).
//
// Pages that don't define a given updater just see no-op for that
// event type. Safe to include in every HUD page; pages that don't
// need any updater (about:blank, panel.html) end up with a tiny
// idle listener.
(function () {
    "use strict";

    if (!window.bridge) {
        if (window.console) console.warn('[hud-bootstrap] bridge not present; skipping');
        return;
    }

    function safe(name) {
        return typeof window[name] === 'function' ? window[name] : null;
    }

    // state.hp → updateHP(current, total, level)
    window.bridge.on('state.hp', function (p) {
        var fn = safe('updateHP');
        if (!fn || !p) return;
        try { fn(p.current || 0, p.max || 0); } catch (e) { /* swallow */ }
    });

    // state.stamina → updateSTA(current, total)
    window.bridge.on('state.stamina', function (p) {
        var fn = safe('updateSTA');
        if (!fn || !p) return;
        try { fn(p.current || 0, p.max || 0); } catch (e) { /* swallow */ }
    });

    // state.burst_ready → showBurstReady(slot) / hideBurstReady()
    window.bridge.on('state.burst_ready', function (p) {
        if (!p) return;
        var show = safe('showBurstReady');
        var hide = safe('hideBurstReady');
        try {
            if (p.ready && show) show(p.slot || 1);
            else if (!p.ready && hide) hide();
        } catch (e) { /* swallow */ }
    });

    // state.dps → updateDps(payload)
    window.bridge.on('state.dps', function (p) {
        var fn = safe('updateDps');
        if (!fn || !p) return;
        try { fn(p); } catch (e) { /* swallow */ }
    });

    // state.bosshp → updateBossHP(payload)
    window.bridge.on('state.bosshp', function (p) {
        var fn = safe('updateBossHP');
        if (!fn || !p) return;
        try { fn(p); } catch (e) { /* swallow */ }
    });

    // state.changed → updateState(full snapshot)
    window.bridge.on('state.changed', function (p) {
        var fn = safe('updateState');
        if (!fn || !p) return;
        try { fn(p); } catch (e) { /* swallow */ }
    });

    // Boot signal: pages that want to know when wiring is done can
    // listen for the 'hud-bridge-ready' DOM event on document.
    try {
        document.dispatchEvent(new CustomEvent('hud-bridge-ready'));
    } catch (e) { /* swallow */ }

    // S190 — pull current snapshot so freshly-loaded pages paint
    // immediately instead of waiting for the next state change. Fires
    // after DOMContentLoaded so the page's update functions are
    // defined; failures are silent (server may not register the
    // command if WebBridgeLifecycle isn't fully up).
    function bootstrapInitialSnapshot() {
        if (!window.bridge || !window.bridge.cmd) return;
        try {
            window.bridge.cmd('state.snapshot', {}).then(function (snapshot) {
                if (!snapshot) return;
                var fn = safe('updateState');
                if (fn) { try { fn(snapshot); } catch (e) { /* swallow */ } }
                // Also synthesize per-channel calls so pages that only
                // implement updateHP / updateSTA still get the first paint.
                if (typeof snapshot.hp_current === 'number') {
                    var fh = safe('updateHP');
                    if (fh) { try { fh(snapshot.hp_current, snapshot.hp_max || 0); } catch (e) { /* swallow */ } }
                }
                if (typeof snapshot.stamina_current === 'number') {
                    var fs = safe('updateSTA');
                    if (fs) { try { fs(snapshot.stamina_current, snapshot.stamina_max || 0); } catch (e) { /* swallow */ } }
                }
            }, function () { /* no-op on failure */ });
        } catch (e) { /* swallow */ }
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', bootstrapInitialSnapshot);
    } else {
        bootstrapInitialSnapshot();
    }

    window.__hudBootstrap = { version: 's190' };
})();
