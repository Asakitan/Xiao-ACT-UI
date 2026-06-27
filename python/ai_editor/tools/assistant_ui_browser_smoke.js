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
      run_workflow: (_id, _input, runId) => ok({
        workflow: "assistant-selfcheck-flow",
        workflowRunId: runId,
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
    "model-popup-configured-models-ready",
    "control-popup-rects-unclipped",
    "agent-popup-configured-agents-ready",
    "provider-session-state-smoke",
    "history-native-affordances-visible",
    "saved-history-native-affordances-visible",
    "workflow-popup-modes-ready",
    "workflow-result-state-rendered",
    "workflow-run-button-active-state",
    "workflow-backend-execution-rendered"
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
