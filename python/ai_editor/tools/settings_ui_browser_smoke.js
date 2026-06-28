#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const { pathToFileURL } = require("url");

function repoRoot() {
  return path.resolve(__dirname, "..", "..", "..");
}

function loadPlaywright() {
  try {
    return require("playwright");
  } catch (firstError) {
    try {
      return require("playwright-core");
    } catch (_secondError) {
      console.log("SKIP settings-ui-browser-smoke playwright unavailable: " + firstError.message);
      process.exit(0);
    }
  }
}

function screenshotPath(root) {
  const outDir = process.env.SAO_SETTINGS_SMOKE_OUT
    ? path.resolve(process.env.SAO_SETTINGS_SMOKE_OUT)
    : path.join(root, "out", "ai_editor_settings_smoke");
  fs.mkdirSync(outDir, { recursive: true });
  return path.join(outDir, "settings.png");
}

async function main() {
  const root = repoRoot();
  const htmlPath = path.join(root, "python", "web", "ai_editor_app.html");
  if (!fs.existsSync(htmlPath)) {
    throw new Error("Missing AI Editor HTML: " + htmlPath);
  }

  const playwright = loadPlaywright();
  let browser;
  try {
    browser = await playwright.chromium.launch({ headless: true });
  } catch (firstLaunchError) {
    try {
      browser = await playwright.chromium.launch({ headless: true, channel: "msedge" });
    } catch (_edgeLaunchError) {
      throw firstLaunchError;
    }
  }

  const page = await browser.newPage({ viewport: { width: 1440, height: 920 } });
  page.on("pageerror", error => {
    throw error;
  });
  await page.addInitScript(() => {
    const ok = value => Promise.resolve(value);
    const config = {
      provider: "openai",
      model: "gpt-4o-mini",
      base_url: "https://api.openai.com/v1",
      api_key: "",
      mode: "agent",
      editor: {
        tabSize: 4,
        insertSpaces: true,
        formatOnSave: true,
        quickSuggestionsDelay: 10
      },
      files: {
        autoSave: "off",
        autoSaveDelay: 1000,
        insertFinalNewline: true
      },
      workspace: {
        auto_detect: true,
        remember_last: true,
        root: "E:/VC/SAO-UI/sao_auto"
      },
      extensions: {
        confirm_install: true,
        diagnostics_enabled: false,
        enabled_contributions: ["commands", "views", "customEditors", "webviews"]
      }
    };
    const apiTarget = {
      load_config: () => ok(config),
      list_tools: () => ok({ tools: [] }),
      get_mode: () => ok({ mode: "agent", permissions: {} }),
      get_chat_controls: () => ok({ ok: true, provider: "openai", model: "gpt-4o-mini", agents: [], workflows: [], providers: [] }),
      list_provider_models: () => ok({ models: [{ id: "gpt-4o-mini", name: "gpt-4o-mini" }], default_model: "gpt-4o-mini" }),
      list_editor_languages: () => ok({ languages: [{ id: "python", name: "Python" }, { id: "javascript", name: "JavaScript" }] }),
      list_editor_themes: () => ok({ themes: [] }),
      list_extension_settings: () => ok({ sections: [], settings: [] }),
      list_extension_runtime_surfaces: () => ok({ surfaces: [] }),
      list_mcp_servers: () => ok({ servers: [] }),
      get_runtime_support_summary: () => ok({ ok: true, diagnostics: [] }),
      get_model_info: () => ok({ max_input: 128000, max_output: 4096, compact_at: 115200 })
    };
    window.pywebview = {
      api: new Proxy(apiTarget, {
        get(target, prop) {
          if (prop in target) return target[prop];
          return () => ok({ ok: true });
        }
      })
    };
  });

  await page.goto(pathToFileURL(htmlPath).href, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => typeof window.openSettings === "function" && typeof window.settingsUiSelfCheckSnapshot === "function", null, { timeout: 15000 });
  await page.evaluate(() => {
    window.config = Object.assign(window.config || {}, {
      provider: "openai",
      model: "gpt-4o-mini",
      base_url: "https://api.openai.com/v1",
      editor: { tabSize: 4, insertSpaces: true, formatOnSave: true },
      files: { autoSave: "off", insertFinalNewline: true },
      extensions: { confirm_install: true, enabled_contributions: ["commands", "views", "webviews", "customEditors"] }
    });
    window.openSettings();
  });
  await page.waitForFunction(() => document.querySelector("#settings-modal.open #settings-nav-filter"), null, { timeout: 15000 });
  await page.waitForTimeout(350);
  const result = await page.evaluate(() => {
    const firstRow = window.settingsVisibleRows && window.settingsVisibleRows()[0];
    if (firstRow) window.settingsHighlightRow(firstRow);
    const navFiltered = window.settingsFilterNavCategories ? window.settingsFilterNavCategories("editor") : null;
    const navCleared = window.settingsClearNavFilter ? window.settingsClearNavFilter() : null;
    const sectionMoved = window.settingsFocusSiblingSection ? window.settingsFocusSiblingSection(1) : false;
    if (firstRow) window.settingsSearchForKey(firstRow.dataset.settingKey || firstRow.dataset.extSettingKey || "editor.tabSize", "user");
    const reviewApplied = window.settingsApplyReviewFilter ? window.settingsApplyReviewFilter("modified") : false;
    const reviewCleared = window.settingsClearReviewFilters ? window.settingsClearReviewFilters() : false;
    const languageSearchApplied = window.settingsSearchModifiedLanguageOverride ? window.settingsSearchModifiedLanguageOverride("python") : false;
    if (window.settingsClearFilters) window.settingsClearFilters();
    const snapshot = window.settingsUiSelfCheckSnapshot();
    const modal = document.querySelector("#settings-modal .settings-modal");
    const rect = modal ? modal.getBoundingClientRect() : null;
    return {
      snapshot,
      navFiltered,
      navCleared,
      sectionMoved,
      modalRect: rect ? { width: rect.width, height: rect.height } : null,
      resultCountText: (document.querySelector("#settings-result-count") || {}).textContent || "",
      filterSummary: (document.querySelector("#settings-filter-summary") || {}).textContent || "",
      detailActions: Array.from(document.querySelectorAll("#settings-current-detail .detail-actions button")).map(btn => btn.textContent),
      reviewActions: Array.from(document.querySelectorAll("#settings-review-bar .review-actions button")).map(btn => btn.textContent),
      languageSuggestions: Array.from(document.querySelectorAll("#settings-language-suggestions [data-settings-language-suggestion]")).map(btn => btn.dataset.settingsLanguageSuggestion),
      sectionActions: Array.from(document.querySelectorAll("#settings-section-context .section-actions button")).map(btn => btn.textContent),
      reviewApplied,
      reviewCleared,
      languageSearchApplied,
      visibleRows: window.settingsVisibleRows ? window.settingsVisibleRows().length : 0
    };
  });

  const shotPath = screenshotPath(root);
  await page.screenshot({ path: shotPath, fullPage: false });
  await browser.close();

  const requiredActions = ["Prev", "Next", "Section", "Filter", "Copy ID", "Copy Value", "Copy JSON", "Use Default", "Use Inherited", "Clear Override", "JSON"];
  const missingDetail = requiredActions.filter(action => !result.detailActions.includes(action));
  const requiredReview = ["Show Modified", "Show Overrides", "Show Errors", "Clear Review"];
  const missingReview = requiredReview.filter(action => !result.reviewActions.includes(action));
  const requiredSection = ["Prev Section", "Next Section", "Search"];
  const missingSection = requiredSection.filter(action => !result.sectionActions.includes(action));
  if (!result.snapshot || result.snapshot.pass !== true) {
    throw new Error("Settings selfcheck failed: " + JSON.stringify(result));
  }
  if (!result.snapshot.hasNavFilter || !result.snapshot.hasResultCount || !result.snapshot.hasNavFilterCount || !result.snapshot.hasSectionContextActions) {
    throw new Error("Settings selfcheck missing UI affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasQueryBox || !result.snapshot.hasScopeControl || !result.snapshot.hasNavHeading || !result.snapshot.hasNavCountPills || !result.snapshot.hasActiveNavRail) {
    throw new Error("Settings selfcheck missing UI affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasDetailValueActions) {
    throw new Error("Settings selfcheck missing row value actions: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasReviewFilterActions || !result.snapshot.hasOverridesFilterToken) {
    throw new Error("Settings selfcheck missing review filter affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasLanguageSuggestions || !result.snapshot.hasLanguageModifiedSearch || !result.languageSearchApplied || !result.languageSuggestions.length) {
    throw new Error("Settings selfcheck missing language override affordances: " + JSON.stringify(result));
  }
  if (!result.navFiltered || result.navFiltered.visible < 1 || !result.navCleared || result.navCleared.visible < result.navFiltered.visible) {
    throw new Error("Settings category filter did not behave as expected: " + JSON.stringify(result));
  }
  if (!result.reviewApplied || !result.reviewCleared || missingReview.length) {
    throw new Error("Settings review filters did not behave as expected: " + JSON.stringify({ result, missingReview }));
  }
  if (!result.resultCountText || missingDetail.length || missingSection.length || !result.visibleRows) {
    throw new Error("Settings smoke missing navigation feedback: " + JSON.stringify({ result, missingDetail, missingSection }));
  }
  console.log("PASS settings-ui-browser-smoke rows=" + result.visibleRows + " screenshot=" + shotPath);
}

main().catch(error => {
  console.error("FAIL settings-ui-browser-smoke");
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
