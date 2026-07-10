#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const { pathToFileURL } = require("url");

function repoRoot() {
  return path.resolve(__dirname, "..", "..", "..");
}

function loadPythonPlaywrightFallback(nativeError) {
  try {
    return require("../browser_smoke_python_bridge");
  } catch (fallbackError) {
    if (String(process.env.SAO_ALLOW_BROWSER_SMOKE_SKIP || "").trim() === "1") {
      console.warn("SKIP settings-ui-browser-smoke playwright unavailable; explicitly allowed by SAO_ALLOW_BROWSER_SMOKE_SKIP=1: " + fallbackError.message);
      process.exit(0);
    }
    throw new Error(
      "Playwright is required for settings-ui-browser-smoke release validation. " +
      "Install playwright/playwright-core or Python Playwright with a runnable Chromium/Edge, " +
      "or set SAO_ALLOW_BROWSER_SMOKE_SKIP=1 only for an explicit local-development skip. " +
      "Node runtime error: " + nativeError.message + ". Python fallback error: " + fallbackError.message
    );
  }
}

function loadPlaywright() {
  if (String(process.env.SAO_BROWSER_SMOKE_FORCE_PYTHON || "").trim() === "1") {
    return loadPythonPlaywrightFallback(new Error("native Node Playwright bypassed by SAO_BROWSER_SMOKE_FORCE_PYTHON=1"));
  }
  try {
    return require("playwright");
  } catch (firstError) {
    try {
      return require("playwright-core");
    } catch (_secondError) {
      return loadPythonPlaywrightFallback(firstError);
    }
  }
}

async function launchBrowser(playwright, allowPythonFallback = true) {
  let chromiumError;
  try {
    return await playwright.chromium.launch({ headless: true });
  } catch (error) {
    chromiumError = error;
  }
  try {
    return await playwright.chromium.launch({ headless: true, channel: "msedge" });
  } catch (edgeError) {
    if (allowPythonFallback && (!playwright._runtime || playwright._runtime.kind !== "python-playwright")) {
      const nativeError = new Error(
        "Node Playwright loaded but Chromium and Edge could not launch: " +
        chromiumError.message + " | " + edgeError.message
      );
      return launchBrowser(loadPythonPlaywrightFallback(nativeError), false);
    }
    throw new Error(
      "Playwright could not launch Chromium or Edge: " +
      chromiumError.message + " | " + edgeError.message
    );
  }
}

function screenshotPath(root) {
  const outDir = process.env.SAO_SETTINGS_SMOKE_OUT
    ? path.resolve(process.env.SAO_SETTINGS_SMOKE_OUT)
    : path.join(root, "out", "ai_editor_settings_smoke");
  fs.mkdirSync(outDir, { recursive: true });
  return path.join(outDir, "settings.png");
}

async function installSettingsAccessibilityFixture(page) {
  await page.addInitScript(() => {
    const ok = value => Promise.resolve(value);
    const config = {
      provider: "openai",
      model: "gpt-4o-mini",
      base_url: "https://api.openai.com/v1",
      api_key: "",
      mode: "agent",
      editor: { tabSize: 4, insertSpaces: true, formatOnSave: true },
      files: { autoSave: "off", autoSaveDelay: 1000, insertFinalNewline: true },
      workspace: { auto_detect: true, remember_last: true, root: "E:/VC/SAO-UI/sao_auto" },
      extensions: { confirm_install: true, enabled_contributions: ["commands", "views", "webviews"] }
    };
    const apiTarget = {
      load_config: () => ok(config),
      list_tools: () => ok({ tools: [] }),
      get_mode: () => ok({ mode: "agent", permissions: {} }),
      get_chat_controls: () => ok({ ok: true, provider: "openai", model: "gpt-4o-mini", agents: [], workflows: [], providers: [] }),
      list_provider_models: () => ok({ models: [{ id: "gpt-4o-mini", name: "gpt-4o-mini" }], default_model: "gpt-4o-mini" }),
      list_editor_languages: () => ok({ languages: [{ id: "python", name: "Python" }] }),
      list_editor_themes: () => ok({ themes: [] }),
      list_extension_settings: () => ok({ configurations: [], languageDefaults: [] }),
      list_extension_runtime_surfaces: () => ok({ surfaces: [] }),
      list_mcp_servers: () => ok({ servers: [] }),
      get_runtime_support_summary: () => ok({ ok: true, diagnostics: [] }),
      get_model_info: () => ok({ max_input: 128000, max_output: 4096, compact_at: 115200 }),
      editor_language_provider: payload => ok(
        String(payload?.kind || "") === "formattingProviders"
          ? {
              ok: true,
              providers: [
                { providerId: "formatter.alpha", displayName: "Formatter Alpha", capabilities: ["document"] },
                { providerId: "formatter.beta", displayName: "Formatter Beta", capabilities: ["document"] }
              ]
            }
          : { ok: true, items: [], providers: [] }
      )
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
}

async function runSettingsAccessibilityViewportCheck(browser, htmlUrl, viewport) {
  const page = await browser.newPage({ viewport });
  page.on("pageerror", error => { throw error; });
  await installSettingsAccessibilityFixture(page);
  await page.goto(htmlUrl, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(
    () => typeof window.openSettings === "function",
    null,
    { timeout: 15000 }
  );
  await page.evaluate(() => {
    try {
      localStorage.setItem("sao.aiEditor.settings.details.v1", "closed");
      localStorage.setItem("sao.aiEditor.settings.density.v1", "comfortable");
      localStorage.setItem("sao.aiEditor.settings.view.v1", JSON.stringify({ mode: "simple" }));
    } catch (_error) {}
    const opener = document.createElement("button");
    opener.id = "settings-a11y-opener";
    opener.type = "button";
    opener.textContent = "Open settings accessibility probe";
    opener.onclick = () => window.openSettings();
    document.body.appendChild(opener);
    opener.focus();
    opener.click();
  });
  await page.waitForFunction(
    () => document.querySelector("#settings-modal")?.dataset.settingsHydration === "hydrated",
    null,
    { timeout: 15000 }
  );
  await page.waitForTimeout(120);
  const openState = await page.evaluate(() => {
    const modal = document.querySelector("#settings-modal");
    const compact = matchMedia("(max-width: 900px)").matches;
    const initialFocus = document.activeElement;
    const focusables = Array.from(modal.querySelectorAll(
      'a[href],button,input,select,textarea,summary,[contenteditable="true"],[tabindex]:not([tabindex="-1"])'
    )).filter(element => {
      if (element.disabled || element.getAttribute("aria-disabled") === "true") return false;
      if (element.closest('[inert],[aria-hidden="true"]')) return false;
      const style = getComputedStyle(element);
      const rect = element.getBoundingClientRect();
      return style.display !== "none" && style.visibility !== "hidden" && rect.width > 0 && rect.height > 0 && element.tabIndex >= 0;
    });
    const first = focusables[0] || null;
    const last = focusables[focusables.length - 1] || null;
    const dispatchTab = (target, shiftKey) => {
      if (!target) return false;
      target.focus({ preventScroll: true });
      target.dispatchEvent(new KeyboardEvent("keydown", {
        key: "Tab", shiftKey: !!shiftKey, bubbles: true, cancelable: true, composed: true
      }));
      return true;
    };
    dispatchTab(last, false);
    const forwardWrapped = !!first && document.activeElement === first;
    dispatchTab(first, true);
    const reverseWrapped = !!last && document.activeElement === last;
    const controlName = id => {
      const control = document.getElementById(id);
      if (!control) return { id, present: false, name: "", labelledby: "", describedby: "" };
      const labelledby = String(control.getAttribute("aria-labelledby") || "").trim();
      const referenced = labelledby.split(/\s+/).filter(Boolean)
        .map(ref => document.getElementById(ref)?.textContent?.trim() || "").filter(Boolean).join(" ");
      const labels = control.labels ? Array.from(control.labels).map(label => label.textContent.trim()).filter(Boolean).join(" ") : "";
      return {
        id,
        present: true,
        name: String(control.getAttribute("aria-label") || referenced || labels || "").trim(),
        labelCount: control.labels ? control.labels.length : 0,
        labelledby,
        describedby: String(control.getAttribute("aria-describedby") || "").trim()
      };
    };
    const backgroundRoots = Array.from(document.body.children)
      .filter(node => node !== modal && !["SCRIPT", "STYLE", "LINK"].includes(node.tagName));
    const tabbableSelector = 'a[href],button,input,select,textarea,summary,[contenteditable="true"],[tabindex]:not([tabindex="-1"])';
    const uninertBackgroundRoots = backgroundRoots.filter(node => {
      if (node.hasAttribute("inert")) return false;
      const candidates = [
        ...(node.matches?.(tabbableSelector) ? [node] : []),
        ...Array.from(node.querySelectorAll?.(tabbableSelector) || [])
      ];
      return candidates.some(element => !element.disabled && element.tabIndex >= 0);
    });
    const controls = ["s-provider", "s-apikey", "s-baseurl", "sk-openai", "sk-anthropic", "sk-deepseek", "s-model"].map(controlName);
    return {
      viewport: { width: innerWidth, height: innerHeight },
      compact,
      initialFocusId: initialFocus?.id || "",
      initialFocusInside: !!initialFocus?.closest?.("#settings-modal"),
      expectedInitialFocusId: compact ? "settings-mobile-nav-toggle" : "settings-search",
      focusableCount: focusables.length,
      firstFocusId: first?.id || "",
      lastFocusText: String(last?.getAttribute?.("aria-label") || last?.textContent || "").trim(),
      forwardWrapped,
      reverseWrapped,
      backgroundRootCount: backgroundRoots.length,
      uninertBackgroundRootCount: uninertBackgroundRoots.length,
      uninertBackgroundRoots: uninertBackgroundRoots.map(node => ({
        tag: node.tagName,
        id: node.id || "",
        className: String(node.className || "").slice(0, 80)
      })),
      controls,
      nativeLabelsPreferred: ["s-provider", "s-baseurl"].every(id => {
        const control = controls.find(item => item.id === id);
        return !!control && control.labelCount > 0 && !control.labelledby;
      }),
      dynamicDescriptionsReady: ["s-provider", "s-baseurl", "s-model"].every(id => !!document.getElementById(id)?.getAttribute("aria-describedby"))
    };
  });
  const formatterPortalState = await page.evaluate(async () => {
    const opener = document.querySelector('[onclick*="pickSettingsDefaultFormatter"]');
    const palette = document.querySelector("#cmd-palette");
    if (!opener || !palette) return { available: false };
    opener.focus({ preventScroll: true });
    const pending = window.pickSettingsDefaultFormatter("s-editor-default-formatter");
    await new Promise(resolve => setTimeout(resolve, 0));
    const item = palette.querySelector(".cmd-item");
    const during = {
      open: palette.classList.contains("open"),
      inert: palette.hasAttribute("inert"),
      focusInside: palette.contains(document.activeElement),
      itemCount: palette.querySelectorAll(".cmd-item").length
    };
    item?.click();
    const selected = await pending;
    await new Promise(resolve => setTimeout(resolve, 0));
    return {
      available: true,
      during,
      selected,
      selectedValue: document.querySelector("#s-editor-default-formatter")?.value || "",
      focusRestored: document.activeElement === opener,
      inertRestored: palette.hasAttribute("inert"),
      portalMarkerCleared: !palette.dataset.settingsModalPortalActive
    };
  });
  await page.evaluate(() => {
    const modal = document.querySelector("#settings-modal");
    const target = document.activeElement?.closest?.("#settings-modal") ? document.activeElement : modal;
    target?.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true, composed: true }));
  });
  await page.waitForTimeout(80);
  let closedState = await page.evaluate(() => ({
    modalOpen: document.querySelector("#settings-modal")?.classList.contains("open") || false,
    focusId: document.activeElement?.id || "",
    backgroundMarkers: document.querySelectorAll("[data-settings-background-inert='1']").length
  }));
  if (closedState.modalOpen) {
    await page.evaluate(() => {
      document.activeElement?.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true, composed: true }));
    });
    await page.waitForTimeout(80);
    closedState = await page.evaluate(() => ({
      modalOpen: document.querySelector("#settings-modal")?.classList.contains("open") || false,
      focusId: document.activeElement?.id || "",
      backgroundMarkers: document.querySelectorAll("[data-settings-background-inert='1']").length
    }));
  }
  return { ...openState, formatterPortalState, closedState };
}

async function runSettingsAccessibilityViewportMatrix(browser, htmlUrl) {
  const results = [];
  for (const viewport of [
    { width: 1440, height: 920 },
    { width: 800, height: 600 },
    { width: 600, height: 400 }
  ]) {
    results.push(await runSettingsAccessibilityViewportCheck(browser, htmlUrl, viewport));
  }
  return results;
}

async function main() {
  const root = repoRoot();
  const htmlPath = path.join(root, "python", "web", "ai_editor_app.html");
  if (!fs.existsSync(htmlPath)) {
    throw new Error("Missing AI Editor HTML: " + htmlPath);
  }

  const playwright = loadPlaywright();
  const browser = await launchBrowser(playwright);

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
    const extensionSettingsFixture = {
      configurations: [{
        extension_id: "selftest.settings-pack",
        display_name: "Selftest Settings",
        title: "Selftest",
        id: "selftest.settings",
        properties: {
          "selftest.mode": {
            type: "string",
            title: "Selftest Mode",
            enum: ["auto", "manual"],
            enumItemLabels: ["Auto", "Manual"],
            markdownEnumDescriptions: ["Use automatic behavior.", "Use manual behavior."],
            default: "auto",
            scope: "resource",
            tags: ["preview"]
          },
          "selftest.options": {
            type: "object",
            title: "Selftest Options",
            default: { enabled: true },
            required: ["level"],
            properties: {
              enabled: { type: "boolean", default: true, description: "Enabled" },
              level: { type: "integer", default: 2, minimum: 1, maximum: 5, description: "Level" }
            },
            additionalProperties: false,
            scope: "resource"
          }
        },
        defaults: {
          "selftest.mode": "auto",
          "selftest.options": { enabled: true }
        },
        values: {
          "selftest.mode": "manual",
          "selftest.options": { enabled: true, level: 2 }
        },
        modified: {
          "selftest.mode": true,
          "selftest.options": true
        },
        targets: {
          "selftest.mode": "workspace",
          "selftest.options": "workspace"
        },
        targetScopedValues: {
          "selftest.mode": { workspace: "manual" },
          "selftest.options": { workspace: { enabled: true, level: 2 } }
        }
      }],
      languageDefaults: [{
        extension_id: "selftest.settings-pack",
        display_name: "Selftest Settings",
        language: "python",
        override: "[python]",
        defaults: { "editor.formatOnSave": true },
        values: { "editor.formatOnSave": false },
        modified: { "editor.formatOnSave": true },
        schemas: { "editor.formatOnSave": { type: "boolean", title: "Format On Save", scope: "language-overridable" } },
        targets: { "editor.formatOnSave": "workspace" },
        targetScopedValues: { "editor.formatOnSave": { workspace: false } }
      }]
    };
    const apiTarget = {
      load_config: () => ok(config),
      list_tools: () => ok({ tools: [] }),
      get_mode: () => ok({ mode: "agent", permissions: {} }),
      get_chat_controls: () => ok({ ok: true, provider: "openai", model: "gpt-4o-mini", agents: [], workflows: [], providers: [] }),
      list_provider_models: () => ok({ models: [{ id: "gpt-4o-mini", name: "gpt-4o-mini" }], default_model: "gpt-4o-mini" }),
      list_editor_languages: () => ok({ languages: [{ id: "python", name: "Python" }, { id: "javascript", name: "JavaScript" }] }),
      list_editor_themes: () => ok({ themes: [] }),
      list_extension_settings: () => ok(extensionSettingsFixture),
      list_extension_runtime_surfaces: () => ok({ surfaces: [] }),
      list_mcp_servers: () => ok({ servers: [] }),
      get_runtime_support_summary: () => ok({ ok: true, diagnostics: [] }),
      get_model_info: () => ok({ max_input: 128000, max_output: 4096, compact_at: 115200 }),
      set_extension_setting: () => ok({ ok: true, target: "workspace", targetScopedValues: {} }),
      reset_extension_setting: () => ok({ ok: true, target: "workspace", targetScopedValues: {} }),
      set_extension_language_setting: () => ok({ ok: true, target: "workspace", targetScopedValues: {} }),
      reset_extension_language_setting: () => ok({ ok: true, target: "workspace", targetScopedValues: {} })
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
    try {
      localStorage.setItem("sao.aiEditor.settings.details.v1", "closed");
      localStorage.setItem("sao.aiEditor.settings.density.v1", "comfortable");
      localStorage.setItem("sao.aiEditor.settings.view.v1", JSON.stringify({ mode: "simple" }));
    } catch (_error) {}
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
    const modal = document.querySelector("#settings-modal .settings-modal");
    const personalNav = document.querySelector("[data-settings-personal-nav='1']");
    const simpleSnapshot = window.settingsUiSelfCheckSnapshot();
    const simpleState = {
      mode: String((document.querySelector("#settings-modal") || {}).dataset?.settingsMode || ""),
      detailsOpen: !!(modal && modal.classList.contains("settings-details-open")),
      personalNavVisible: !!(personalNav && getComputedStyle(personalNav).display !== "none")
    };
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
    const insightButton = document.querySelector("#ext-settings-insight [data-ext-settings-insight-token]");
    if (insightButton) insightButton.click();
    if (window.extensionSettingPreheatLazyRows) window.extensionSettingPreheatLazyRows(document.querySelector("#ext-settings-container"), 24);
    document.querySelectorAll("[data-ext-lazy-hydrate='1']").forEach(node => {
      if (typeof node._extensionSettingHydrate === "function") node._extensionSettingHydrate();
    });
    const enumFilterButton = Array.from(document.querySelectorAll(".ext-setting-enum-choice-actions button")).find(btn => btn.textContent === "Filter");
    const enumUseButton = Array.from(document.querySelectorAll(".ext-setting-enum-choice-actions button")).find(btn => btn.textContent === "Use");
    const structuredHint = document.querySelector(".ext-setting-structured-hint");
    const schemaDetails = document.querySelector(".ext-setting-schema-details summary");
    if (enumFilterButton) enumFilterButton.click();
    if (enumUseButton) enumUseButton.click();
    if (schemaDetails) schemaDetails.click();
    const extensionSearchValue = (document.querySelector("#settings-search") || {}).value || "";
    if (window.settingsClearFilters) window.settingsClearFilters();
    const advancedModeButton = document.querySelector('[data-settings-mode="advanced"]');
    if (advancedModeButton) advancedModeButton.click();
    const detailsToggle = document.querySelector("#settings-details-toggle");
    if (detailsToggle && !(modal && modal.classList.contains("settings-details-open"))) detailsToggle.click();
    const detailedRows = Array.from(document.querySelectorAll(".settings-field.builtin-setting")).filter(row => {
      const style = getComputedStyle(row);
      return style.display !== "none" && !row.classList.contains("settings-mode-hidden") && !!row.querySelector("input,select,textarea");
    });
    const detailedRow = detailedRows.find(row => row.classList.contains("modified")) || detailedRows[0];
    if (detailedRow && window.settingsHighlightRow) window.settingsHighlightRow(detailedRow);
    const detailedSnapshot = window.settingsUiSelfCheckSnapshot();
    const detailedState = {
      mode: String((document.querySelector("#settings-modal") || {}).dataset?.settingsMode || ""),
      detailsOpen: !!(modal && modal.classList.contains("settings-details-open")),
      personalNavVisible: (() => {
        const currentPersonalNav = document.querySelector("[data-settings-personal-nav='1']");
        return !!(currentPersonalNav && getComputedStyle(currentPersonalNav).display !== "none");
      })()
    };
    const snapshot = { ...simpleSnapshot };
    [
      "hasDetailValueActions",
      "hasDetailSaveRevertActions",
      "hasDetailTargetNote",
      "hasDetailValueMatrix",
      "hasDetailValueMatrixRows",
      "hasDetailValueMatrixActions",
      "hasDetailCopyLinkAction",
      "hasDetailBreadcrumb",
      "hasDefaultVisiblePersonalNav"
    ].forEach(key => { snapshot[key] = detailedSnapshot[key]; });
    snapshot.pass = simpleSnapshot.pass === true && detailedSnapshot.pass === true;
    const rect = modal ? modal.getBoundingClientRect() : null;
    return {
      snapshot,
      simpleSnapshot,
      detailedSnapshot,
      simpleState,
      detailedState,
      detailedRow: detailedRow ? {
        key: detailedRow.dataset.settingKey || detailedRow.dataset.extSettingKey || "",
        className: detailedRow.className,
        inputId: (detailedRow.querySelector("input,select,textarea") || {}).id || ""
      } : null,
      navFiltered,
      navCleared,
      sectionMoved,
      modalRect: rect ? { width: rect.width, height: rect.height } : null,
      resultCountText: (document.querySelector("#settings-result-count") || {}).textContent || "",
      filterSummary: (document.querySelector("#settings-filter-summary") || {}).textContent || "",
      detailActions: Array.from(document.querySelectorAll("#settings-current-detail .detail-actions button")).map(btn => btn.textContent),
      reviewActions: Array.from(document.querySelectorAll("#settings-review-bar .review-actions button")).map(btn => btn.textContent),
      languageSuggestions: Array.from(document.querySelectorAll("#settings-language-suggestions [data-settings-language-suggestion]")).map(btn => btn.dataset.settingsLanguageSuggestion),
      extensionInsightTokens: Array.from(document.querySelectorAll("#ext-settings-insight [data-ext-settings-insight-token]")).map(btn => btn.dataset.extSettingsInsightToken),
      extensionEnumActions: Array.from(document.querySelectorAll(".ext-setting-enum-choice-actions button")).map(btn => btn.textContent),
      hasStructuredHints: !!structuredHint,
      hasSchemaDetails: !!schemaDetails,
      performanceStatus: (document.querySelector("#ext-settings-perf") || {}).textContent || "",
      extensionSearchValue,
      sectionActions: Array.from(document.querySelectorAll("#settings-section-context .section-actions button")).map(btn => btn.textContent),
      reviewApplied,
      reviewCleared,
      languageSearchApplied,
      visibleRows: window.settingsVisibleRows ? window.settingsVisibleRows().length : 0
    };
  });

  result.restoredState = await page.evaluate(() => {
    const modal = document.querySelector("#settings-modal .settings-modal");
    const detailsToggle = document.querySelector("#settings-details-toggle");
    if (detailsToggle && modal && modal.classList.contains("settings-details-open")) detailsToggle.click();
    const simpleModeButton = document.querySelector('[data-settings-mode="simple"]');
    if (simpleModeButton) simpleModeButton.click();
    const currentPersonalNav = document.querySelector("[data-settings-personal-nav='1']");
    return {
      mode: String((document.querySelector("#settings-modal") || {}).dataset?.settingsMode || ""),
      detailsOpen: !!(modal && modal.classList.contains("settings-details-open")),
      personalNavVisible: !!(currentPersonalNav && getComputedStyle(currentPersonalNav).display !== "none")
    };
  });
  const shotPath = screenshotPath(root);
  await page.screenshot({ path: shotPath, fullPage: false });
  result.accessibilityMatrix = await runSettingsAccessibilityViewportMatrix(
    browser,
    pathToFileURL(htmlPath).href
  );
  await browser.close();

  const requiredActions = ["Prev", "Next", "Section", "Filter", "Copy ID", "Copy Link", "Copy Value", "Copy JSON", "Revert Setting", "Use Default", "Use Inherited", "Clear Override", "JSON"];
  const missingDetail = requiredActions.filter(action => !result.detailActions.includes(action));
  const requiredReview = ["Show Modified", "Show Overrides", "Show Errors", "Clear Review"];
  const missingReview = requiredReview.filter(action => !result.reviewActions.includes(action));
  const requiredSection = ["Prev Section", "Next Section", "Search"];
  const missingSection = requiredSection.filter(action => !result.sectionActions.includes(action));
  if (!result.simpleSnapshot || result.simpleSnapshot.pass !== true || result.simpleState.mode !== "simple" || result.simpleState.detailsOpen || result.simpleState.personalNavVisible) {
    throw new Error("Settings Simple mode is not calm by default: " + JSON.stringify({ snapshot: result.simpleSnapshot, state: result.simpleState }));
  }
  if (!result.simpleSnapshot.hasDefaultHiddenInlineDetailsToggle || !result.simpleSnapshot.hasCalmDefaultSettingsMode || result.simpleSnapshot.hasDefaultVisiblePersonalNav) {
    throw new Error("Settings Simple mode did not hide advanced disclosure surfaces: " + JSON.stringify(result.simpleSnapshot));
  }
  if (!result.detailedSnapshot || result.detailedSnapshot.pass !== true || result.detailedState.mode !== "advanced" || !result.detailedState.detailsOpen || !result.detailedState.personalNavVisible) {
    throw new Error("Settings Advanced details mode is not reachable: " + JSON.stringify({ snapshot: result.detailedSnapshot, state: result.detailedState }));
  }
  if (!result.restoredState || result.restoredState.mode !== "simple" || result.restoredState.detailsOpen || result.restoredState.personalNavVisible) {
    throw new Error("Settings did not restore the calm Simple mode after detailed validation: " + JSON.stringify(result.restoredState));
  }
  if (!result.snapshot || result.snapshot.pass !== true) {
    throw new Error("Settings selfcheck failed: " + JSON.stringify(result));
  }
  if (!result.snapshot.hasNavFilter || !result.snapshot.hasResultCount || !result.snapshot.hasNavFilterCount || !result.snapshot.hasSectionContextActions) {
    throw new Error("Settings selfcheck missing UI affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasQueryBox || !result.snapshot.hasScopeControl || !result.snapshot.hasNavHeading || !result.snapshot.hasNavCountPills || !result.snapshot.hasActiveNavRail) {
    throw new Error("Settings selfcheck missing UI affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasBuiltSettingHead || !result.snapshot.hasOnDemandSettingValueDetails) {
    throw new Error("Settings selfcheck missing row hierarchy affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasWorkspaceSizedModal || !result.snapshot.hasSyncedSettingsViewport || !result.snapshot.hasVisibleSettingsTop || !result.snapshot.hasVisibleSettingsFooter || !result.snapshot.hasSettingsFrameFitsViewport || !result.snapshot.hasSettingsSafeHeightCap || !result.snapshot.hasSettingsAutoCompactFit || !result.snapshot.hasVisualViewportResizeSync || !result.snapshot.hasSettingsDvhHeightGuard || !result.snapshot.hasWideSettingsEditor || !result.snapshot.hasSettingsEditorShell || !result.snapshot.hasWideCategoryNav) {
    throw new Error("Settings selfcheck missing VS Code-like editor shell: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasStableSettingsScrollbars || !result.snapshot.hasScrollableSettingsColumns) {
    throw new Error("Settings selfcheck missing contained scroll columns: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasSettingsInspector || !result.snapshot.hasInspectorDetail || !result.snapshot.hasLabelsInsideRows || !result.snapshot.hidesOriginalLabels) {
    throw new Error("Settings selfcheck missing inspector or row label structure: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasBuiltSettingGridHead || !result.snapshot.hasBuiltSettingRowDivider || !result.snapshot.hasTargetCountChips || !result.snapshot.hasTargetSummaryScopeCounts) {
    throw new Error("Settings selfcheck missing Settings Editor row and scope affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasQuietSearchHints || !result.snapshot.hasHelpOpenSearchHints || !result.snapshot.hasConditionalReviewBar) {
    throw new Error("Settings selfcheck missing calm workbench disclosure states: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasCollapsedSecondaryActions || !result.snapshot.hasVsCodeGroupHeadings || !result.snapshot.hasLeanSettingControls || !result.snapshot.hasQuietInspectorFocusDeck || !result.snapshot.hasStatusbarFooter) {
    throw new Error("Settings selfcheck missing calmer VS Code layout refinements: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasIconOnlySettingsToolbar || !result.snapshot.hasInlineDetailsToggle || !result.snapshot.hasHiddenDefaultTitlebar || !result.snapshot.hasPrimaryJsonOnlyDefaultToolbar || !result.snapshot.hasCalmDefaultSettingsMode || !result.snapshot.hasVsCodeWideCategoryNav || !result.snapshot.hasMoreComfortableSettingControls) {
    throw new Error("Settings selfcheck missing VS Code-like default interaction refinements: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasQuietDefaultSettingsToolbar || !result.snapshot.hasDetailsOnlyJumpbar || !result.snapshot.hasSidebarQuickSettingsActions || !result.snapshot.hasCompactSearchScopeStack || !result.snapshot.hasNoTopSettingsBands || !result.snapshot.hasSingleSearchTopBand) {
    throw new Error("Settings selfcheck missing quiet VS Code-like top layout refinements: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasSearchOnlyTopWorkbench || !result.snapshot.hasHiddenTopSearchCaption || !result.snapshot.hasWideSingleSettingsSearch || !result.snapshot.hasHiddenDefaultResultNav) {
    throw new Error("Settings selfcheck still has noisy top workbench bands: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasSidebarSettingsSearch || !result.snapshot.hasNoFullWidthSettingsSearchbar) {
    throw new Error("Settings selfcheck still uses a full-width top settings search bar: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasSidebarSettingsCommandbar || !result.snapshot.hasNoVisibleTopSettingsBar) {
    throw new Error("Settings selfcheck still exposes noisy top settings commands: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasVsCodeListSettingFlow || !result.snapshot.hasSettingCategoryPrefix || !result.snapshot.hasRowHeadControlActivation || !result.snapshot.hasQuietCleanDirtyState) {
    throw new Error("Settings selfcheck missing VS Code-like setting row flow refinements: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasExtensionVirtualSummary || !result.snapshot.hasExtensionVirtualActions) {
    throw new Error("Settings selfcheck missing extension virtualization controls: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasExtensionQuickFilters || !result.snapshot.hasExtensionQueryHistory || !result.snapshot.hasExtensionSortControl || !result.snapshot.hasExtensionSectionActions) {
    throw new Error("Settings selfcheck missing extension interaction controls: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasDetailValueActions || !result.snapshot.hasDetailSaveRevertActions || !result.snapshot.hasDetailTargetNote || !result.snapshot.hasDetailValueMatrix || !result.snapshot.hasDetailValueMatrixRows || !result.snapshot.hasDetailValueMatrixActions) {
    throw new Error("Settings selfcheck missing row value actions: " + JSON.stringify({ snapshot: result.snapshot, detailedRow: result.detailedRow, detailActions: result.detailActions }));
  }
  if (!result.snapshot.hasDetailCopyLinkAction || !result.snapshot.hasExperienceBar || !result.snapshot.hasExperienceScopeChip || !result.snapshot.hasRowImpactSummary || !result.snapshot.hasSuggestedMatchesHost) {
    throw new Error("Settings selfcheck missing humanized context affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasSettingsPersonalNav || !result.snapshot.hasDefaultVisiblePersonalNav || !result.snapshot.hasDetailBreadcrumb) {
    throw new Error("Settings selfcheck missing VS Code-style personal navigation or breadcrumb affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasReviewFilterActions || !result.snapshot.hasOverridesFilterToken) {
    throw new Error("Settings selfcheck missing review filter affordances: " + JSON.stringify(result.snapshot));
  }
  if (!result.snapshot.hasLanguageSuggestions || !result.snapshot.hasLanguageModifiedSearch || !result.languageSearchApplied || !result.languageSuggestions.length) {
    throw new Error("Settings selfcheck missing language override affordances: " + JSON.stringify(result));
  }
  if (!result.extensionInsightTokens.length || !result.extensionEnumActions.includes("Use") || !result.extensionEnumActions.includes("Filter") || !result.hasStructuredHints || !result.hasSchemaDetails) {
    throw new Error("Settings extension affordances missing: " + JSON.stringify(result));
  }
  if (!result.snapshot.hasExtensionPerformanceStatus || !result.snapshot.extensionPerformance || !result.snapshot.extensionPerformance.rows || !result.performanceStatus) {
    throw new Error("Settings extension performance status missing: " + JSON.stringify(result));
  }
  if (result.extensionSearchValue.indexOf("@value:") < 0) {
    throw new Error("Settings extension enum filter did not update search: " + JSON.stringify(result));
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
  const invalidAccessibilityViewports = (result.accessibilityMatrix || []).filter(item =>
    !item.initialFocusInside ||
    item.initialFocusId !== item.expectedInitialFocusId ||
    !item.forwardWrapped ||
    !item.reverseWrapped ||
    item.focusableCount < 2 ||
    item.backgroundRootCount < 1 ||
    item.uninertBackgroundRootCount !== 0 ||
    !item.nativeLabelsPreferred ||
    !item.dynamicDescriptionsReady ||
    item.controls.some(control => !control.present || !control.name) ||
    !item.formatterPortalState?.available ||
    !item.formatterPortalState.during?.open ||
    item.formatterPortalState.during?.inert ||
    !item.formatterPortalState.during?.focusInside ||
    item.formatterPortalState.during?.itemCount < 2 ||
    item.formatterPortalState.selected !== true ||
    item.formatterPortalState.selectedValue !== "formatter.alpha" ||
    !item.formatterPortalState.focusRestored ||
    !item.formatterPortalState.inertRestored ||
    !item.formatterPortalState.portalMarkerCleared ||
    !item.closedState ||
    item.closedState.modalOpen ||
    item.closedState.focusId !== "settings-a11y-opener" ||
    item.closedState.backgroundMarkers !== 0
  );
  if (invalidAccessibilityViewports.length) {
    throw new Error("Settings modal accessibility viewport regression: " + JSON.stringify(invalidAccessibilityViewports));
  }
  console.log(
    "PASS settings-ui-browser-smoke rows=" + result.visibleRows +
    " a11y-viewports=" + result.accessibilityMatrix.length +
    " screenshot=" + shotPath
  );
}

main().catch(error => {
  console.error("FAIL settings-ui-browser-smoke");
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
