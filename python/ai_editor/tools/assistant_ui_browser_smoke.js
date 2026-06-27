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
      console.log("SKIP assistant-ui-browser-smoke playwright unavailable: " + firstError.message);
      process.exit(0);
    }
  }
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
    const apiTarget = {
      load_config: () => ok({ provider: "OpenAI", model: "gpt-4o-mini", mode: "agent" }),
      get_chat_controls: () => ok({
        ok: true,
        provider: "OpenAI",
        model: "gpt-4o-mini",
        mode: "agent",
        active_chat_provider: "chat",
        active_agent_id: "",
        agents: [{ id: "default", name: "Default" }, { id: "reviewer", name: "Reviewer" }],
        models: [
          { id: "gpt-4o-mini", name: "gpt-4o-mini", provider: "OpenAI", max_input: 128000 },
          { id: "custom-endpoint-model", name: "custom-endpoint-model", custom: true, max_input: 64000 }
        ],
        custom_models: {
          "custom-endpoint-model": { max_input: 64000, max_output: 4096 }
        },
        workflows: [{
          id: "assistant-selfcheck-flow",
          name: "Assistant Selfcheck Flow",
          steps: [
            { agent: "default", label: "Inspect", prompt: "Inspect {{input}}", output_var: "inspection" },
            { agent: "reviewer", label: "Review", prompt: "Review {{inspection}}", output_var: "review" }
          ]
        }],
        providers: [{ id: "chat", name: "Chat" }]
      }),
      run_workflow: (_id, _input, runId, metadata) => ok({
        workflow: "assistant-selfcheck-flow",
        workflowRunId: runId,
        workflowMode: metadata && metadata.workflowMode,
        workflowMethod: metadata && metadata.workflowMethod,
        sessionResource: metadata && metadata.sessionResource,
        inputPreview: metadata && metadata.inputPreview,
        workflowAgents: metadata && metadata.workflowAgents,
        steps: [
          { step: 0, label: "Inspect", agent: "default", output_var: "inspection", output: "inspection ok" },
          { step: 1, label: "Review", agent: "reviewer", output_var: "review", output: "review ok" }
        ],
        final_output: "review ok"
      }),
      cancel_workflow: runId => ok({ ok: true, workflowRunId: runId, cancelled: true, count: 1 }),
      provider_cancel: provider => ok({ ok: true, provider, cancelled: true })
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
  await page.waitForFunction(() => typeof window.runAssistantUiSelfCheck === "function", null, { timeout: 15000 });
  const result = await page.evaluate(async () => window.runAssistantUiSelfCheck({ cleanup: true }));
  await browser.close();

  if (!result || result.ok !== true) {
    throw new Error("Assistant UI selfcheck failed: " + JSON.stringify(result));
  }
  const required = [
    "composer-layout-present",
    "composer-input-history-ready",
    "composer-edit-target-highlight-ready",
    "composer-attachment-summary-ready",
    "composer-queue-state-ready",
    "model-popup-configured-models-ready",
    "model-popup-custom-endpoint-models-ready",
    "model-popup-custom-endpoint-selection-ready",
    "composer-native-selects-hidden",
    "mode-popup-custom-control-ready",
    "control-popup-rects-unclipped",
    "agent-popup-configured-agents-ready",
    "visual-composer-fill-ready",
    "visual-controls-one-row-ready",
    "visual-no-old-control-boxes-ready",
    "visual-popup-surfaces-ready",
    "visual-model-custom-endpoint-ready",
    "visual-workflow-mode-control-ready",
    "provider-session-state-smoke",
    "history-native-affordances-visible",
    "saved-history-native-affordances-visible",
    "native-only-session-parts-persist",
    "change-set-state-restores-and-persists",
    "action-result-state-restores-and-persists",
    "fork-lineage-history-visible",
    "saved-history-action-result-markers-visible",
    "fork-lineage-chip-visible",
    "fork-branch-navigation-visible",
    "saved-history-preview-ready",
    "saved-history-preview-restore-modal-ready",
    "restored-history-banner-ready",
    "restored-history-banner-navigation-ready",
    "restored-history-transcript-groups-ready",
    "restored-history-banner-keyboard-aria-ready",
    "native-response-card-navigation-ready",
    "native-response-card-keyboard-ready",
    "message-footer-target-actions-ready",
    "message-footer-target-focus-ready",
    "fork-branch-compare-visible",
    "fork-branch-compare-detail-ready",
    "fork-branch-compare-summary-ready",
    "fork-branch-compare-delta-grid-ready",
    "workflow-popup-modes-ready",
    "workflow-run-card-status-summary",
    "workflow-run-method-session-rendered",
    "workflow-result-state-rendered",
    "workflow-result-metadata-rendered",
    "workflow-run-button-active-state",
    "workflow-backend-execution-rendered",
    "workflow-backend-metadata-payload-ready"
  ];
  const checks = Array.isArray(result.checks) ? result.checks : [];
  const byName = new Map(checks.map(check => [check.name, check]));
  const missing = required.filter(name => !byName.get(name) || byName.get(name).pass !== true);
  if (missing.length) {
    throw new Error("Missing passing checks: " + missing.join(", "));
  }
  console.log("PASS assistant-ui-browser-smoke checks=" + checks.length);
}

main().catch(error => {
  console.error("FAIL assistant-ui-browser-smoke");
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
