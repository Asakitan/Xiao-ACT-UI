(function () {
    'use strict';

    function normalizeTheme(theme) {
        return String(theme || '').toLowerCase() === 'light' ? 'light' : 'dark';
    }

    function applyPanelTheme(theme) {
        var normalized = normalizeTheme(theme);
        var root = document.documentElement;
        root.dataset.actTheme = normalized;
        root.classList.toggle('theme-light', normalized === 'light');
        root.classList.toggle('theme-dark', normalized === 'dark');
        root.style.colorScheme = normalized;
        return normalized;
    }

    window._applyPanelTheme = applyPanelTheme;
    window._applyActPanelTheme = applyPanelTheme;

    function syncFromHost() {
        if (window.pywebview && window.pywebview.api && window.pywebview.api.get_panel_themes) {
            Promise.resolve(window.pywebview.api.get_panel_themes())
                .then(function (themes) {
                    applyPanelTheme(themes && themes.act);
                })
                .catch(function () {
                    applyPanelTheme(document.documentElement.dataset.actTheme || 'dark');
                });
            return;
        }
        applyPanelTheme(document.documentElement.dataset.actTheme || 'dark');
    }

    applyPanelTheme(document.documentElement.dataset.actTheme || 'dark');
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', syncFromHost, { once: true });
    } else {
        setTimeout(syncFromHost, 0);
    }
}());
