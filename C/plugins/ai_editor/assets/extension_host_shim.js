// SAO AI Editor extension host shim.
//
// Loaded as the entry script of the Node.js runtime spawned from the C++
// ExtensionHost.  Provides a minimal `vscode` module polyfill that routes
// every API surface back to the SAO NativeRuntime through the parent's
// stdio JSON-RPC channel.  Extensions can then `require('vscode')` and
// run unmodified for the surface this shim covers.
//
// Framing: `Content-Length: <n>\r\n\r\n<utf8 JSON>` in both directions.
// Requests use monotonic ids; every host-initiated request expects a
// matching `id` back.

'use strict';

const path = require('path');
const fs = require('fs');
const Module = require('module');
const process_ = require('process');
const { AsyncLocalStorage } = require('async_hooks');

const stdin = process_.stdin;
const stdout = process_.stdout;

const state = {
    nextId: 1,
    pending: new Map(),
    transportClosed: false,
    transportCloseError: null,
    transportExitScheduled: false,
    writeQueue: [],
    writeBytes: 0,
    writeActive: false,
    handlers: new Map(),
    commands: new Map(),
    commandRegistrations: new Map(),
    outputChannels: new Map(),
    extensions: new Map(),
    disposables: [],
    initialized: false,
    // WebView surface: providers register by viewId, panels track by generated id.
    // Each record keeps the mock webview + emitters so the host can drive
    // resolveView / postToView from the SAO side through the native bridge.
    webviewViewProviders: new Map(),
    webviewPanels: new Map(),
    webviewViews: new Map(),
    nextPanelId: 1,
    treeDataProviders: new Map(),
    treeViews: new Map(),
    treeProviderEpochs: new Map(),
    treeProviderTombstones: new Map(),
};

const kMaximumFrameBytes = 8 * 1024 * 1024;
const kMaximumQueuedWriteBytes = 8 * 1024 * 1024;
const kIpcClosedStatus = -112;
const kMaximumTreeChildren = 500;
const kMaximumTreeDepth = 64;
const kMaximumTreeLabelBytes = 64 * 1024;
const kMaximumTreePayloadBytes = 512 * 1024;
const kMaximumTreeHandles = 32768;
const kMaximumTreeProviders = 256;
const kMaximumTreeProviderIdentities = 1024;
const kMaximumTreeViewIdBytes = 256;
const kTreeCallbackTimeoutMs = 3000;
const kDisposableTimeoutMs = 3000;
const kMaximumExtensionMainBytes = 1024 * 1024;
const kMaximumWebviewMessageBytes = 4 * 1024 * 1024;
const kMaximumWebviewInventoryBytes = 1024 * 1024;
const kMaximumWebviewStringBytes = 1024 * 1024;
const kMaximumWebviewNodes = 16384;
const kMaximumWebviewDepth = 64;
const kMaximumWebviewRegistrations = 256;
const treeErrorMarker = Symbol('sao.treeError');

function boundedJsonSnapshot(value, maximumBytes = kMaximumWebviewMessageBytes) {
    let serialized;
    let snapshot;
    try {
        serialized = JSON.stringify(value);
        if (serialized === undefined ||
            Buffer.byteLength(serialized, 'utf8') > maximumBytes) {
            return { ok: false, value: null, bytes: 0 };
        }
        snapshot = JSON.parse(serialized);
    } catch (_) {
        return { ok: false, value: null, bytes: 0 };
    }
    const pending = [{ value: snapshot, depth: 0 }];
    let nodes = 0;
    while (pending.length) {
        const current = pending.pop();
        if (++nodes > kMaximumWebviewNodes ||
            current.depth > kMaximumWebviewDepth) {
            return { ok: false, value: null, bytes: 0 };
        }
        const item = current.value;
        if (typeof item === 'string') {
            if (Buffer.byteLength(item, 'utf8') > kMaximumWebviewStringBytes) {
                return { ok: false, value: null, bytes: 0 };
            }
        } else if (Array.isArray(item)) {
            for (let index = item.length - 1; index >= 0; --index) {
                pending.push({ value: item[index], depth: current.depth + 1 });
            }
        } else if (item && typeof item === 'object') {
            const entries = Object.entries(item);
            for (let index = entries.length - 1; index >= 0; --index) {
                const [key, child] = entries[index];
                if (Buffer.byteLength(key, 'utf8') > 1024) {
                    return { ok: false, value: null, bytes: 0 };
                }
                pending.push({ value: child, depth: current.depth + 1 });
            }
        }
    }
    return {
        ok: true,
        value: snapshot,
        bytes: Buffer.byteLength(serialized, 'utf8'),
    };
}

function transportError(message, status = kIpcClosedStatus, data = undefined) {
    const error = new Error(String(message || 'extension host transport closed'));
    error.status = status;
    error.data = data && typeof data === 'object'
        ? Object.assign({}, data) : {};
    if (!Number.isInteger(error.data.status)) {
        error.data.status = status;
    }
    return error;
}

function rejectPending(error) {
    for (const [id, pending] of Array.from(state.pending.entries())) {
        state.pending.delete(id);
        try { pending.reject(error); } catch (_) { /* observer owns errors */ }
    }
}

function closeTransport(message, status = kIpcClosedStatus, data = undefined) {
    if (state.transportClosed) return;
    state.transportClosed = true;
    state.transportCloseError = transportError(message, status, data);
    state.writeQueue = [];
    state.writeBytes = 0;
    rejectPending(state.transportCloseError);
    try { stdin.destroy(); } catch (_) { /* transport is already closed */ }
    if (!state.transportExitScheduled) {
        state.transportExitScheduled = true;
        setImmediate(() => process_.exit(1));
    }
}

function encodeFrame(payload) {
    const body = Buffer.from(JSON.stringify(payload), 'utf8');
    if (body.length > kMaximumFrameBytes) {
        throw transportError('extension host frame exceeds the maximum size',
            -207);
    }
    const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'utf8');
    return Buffer.concat([header, body]);
}

function flushWrites() {
    if (state.transportClosed || state.writeActive) return;
    const item = state.writeQueue.shift();
    if (!item) return;
    state.writeActive = true;
    try {
        const accepted = stdout.write(item.frame, (error) => {
            state.writeActive = false;
            state.writeBytes = Math.max(0, state.writeBytes - item.frame.length);
            if (state.transportClosed) return;
            if (error) {
                closeTransport(`extension host stdout closed: ${error.message}`);
                return;
            }
            flushWrites();
        });
        if (!accepted) {
            // The write callback is the completion barrier for the queued
            // chunk; do not start another chunk until it fires.
        }
    } catch (err) {
        state.writeActive = false;
        state.writeBytes -= item.frame.length;
        closeTransport(`extension host stdout write failed: ${err.message}`);
    }
}

function send(payload) {
    try {
        if (state.transportClosed) return false;
        const frame = encodeFrame(payload);
        if (state.writeBytes + frame.length > kMaximumQueuedWriteBytes) {
            closeTransport('extension host stdout backpressure limit exceeded',
                -207);
            return false;
        }
        state.writeQueue.push({ frame });
        state.writeBytes += frame.length;
        flushWrites();
        return true;
    } catch (err) {
        closeTransport(`extension host send failed: ${err.message}`,
            err.status || -207, err.data);
        return false;
    }
}

function callHost(method, params) {
    if (state.transportClosed) {
        return Promise.reject(state.transportCloseError ||
            transportError(`host transport closed while calling ${method}`));
    }
    const id = state.nextId++;
    return new Promise((resolve, reject) => {
        state.pending.set(id, { resolve, reject });
        if (!send({ jsonrpc: '2.0', id, method, params: params || {} })) {
            const pending = state.pending.get(id);
            if (pending) {
                state.pending.delete(id);
                reject(state.transportCloseError || transportError(
                    `host transport closed while calling ${method}`));
            }
        }
    });
}

stdout.on('error', (error) => {
    closeTransport(`extension host stdout error: ${error.message}`);
});
stdout.on('close', () => {
    closeTransport('extension host stdout closed');
});

// ----- vscode module polyfill --------------------------------------------

const activationStorage = new AsyncLocalStorage();
const disposalBarrier = Symbol('sao.disposalBarrier');

function runWithActivation(scope, callback) {
    return scope ? activationStorage.run(scope, callback) : callback();
}

function requireActivationScope(apiName) {
    const scope = activationStorage.getStore();
    if (!scope || !scope.extensionId ||
        !Number.isSafeInteger(scope.generation) || scope.generation <= 0 ||
        (scope.phase !== 'active' && scope.phase !== 'committed')) {
        throw new Error(`${apiName} must run in an active extension generation`);
    }
    return scope;
}

function reportDisposalFailure(error) {
    const message = error && error.message ? error.message : String(error);
    process_.stderr.write(`[shim] disposable cleanup failed: ${message}\n`);
    return String(message || 'disposable cleanup failed').slice(0, 1024);
}

async function runWithDeadline(callback, deadline, message) {
    let timer = null;
    try {
        return await Promise.race([
            Promise.resolve().then(callback),
            new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error(message)),
                    Math.max(0, deadline - Date.now()));
            }),
        ]);
    } finally {
        if (timer !== null) clearTimeout(timer);
    }
}

async function settleDisposable(value, deadline = Date.now() +
    kDisposableTimeoutMs) {
    let disposalResult;
    const errors = [];
    try {
        disposalResult = value.dispose();
    } catch (error) {
        errors.push(reportDisposalFailure(error));
    }
    const pending = [];
    if (disposalResult && typeof disposalResult.then === 'function') {
        pending.push(Promise.resolve(disposalResult));
    }
    if (value[disposalBarrier] && value[disposalBarrier] !== disposalResult) {
        pending.push(Promise.resolve(value[disposalBarrier]));
    }
    if (!pending.length) return errors;
    let timer = null;
    try {
        const results = await Promise.race([
            Promise.allSettled(pending),
            new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error(
                    'disposable cleanup timed out')),
                    Math.max(0, deadline - Date.now()));
            }),
        ]);
        for (const result of results) {
            if (result.status === 'rejected') {
                errors.push(reportDisposalFailure(result.reason));
            }
        }
    } catch (error) {
        errors.push(reportDisposalFailure(error));
    } finally {
        if (timer !== null) clearTimeout(timer);
    }
    return errors;
}

function trackActivationDisposableFor(scope, value) {
    if (!scope || !value || typeof value.dispose !== 'function' ||
        scope.seen.has(value)) {
        return value;
    }
    if (scope.phase === 'failed' || scope.phase === 'deactivating' ||
        scope.phase === 'deactivated') {
        scope.seen.add(value);
        try {
            Promise.resolve(value.dispose()).catch(reportDisposalFailure);
        } catch (error) {
            reportDisposalFailure(error);
        }
        return value;
    }
    if (scope.phase === 'committed' && scope.subscriptions) {
        scope.seen.add(value);
        if (!scope.subscriptions.includes(value)) {
            Array.prototype.push.call(scope.subscriptions, value);
        }
        return value;
    }
    if (scope.phase !== 'active' && scope.phase !== 'rolling-back') {
        return value;
    }
    scope.seen.add(value);
    scope.journal.push(value);
    return value;
}

function trackActivationDisposable(value) {
    return trackActivationDisposableFor(activationStorage.getStore(), value);
}

function makeActivationSubscriptions(scope) {
    const subscriptions = [];
    subscriptions.push = function (...items) {
        const accepted = [];
        for (const item of items) {
            if (this.includes(item)) continue;
            const phase = scope.phase;
            trackActivationDisposableFor(scope, item);
            if (phase === 'active' || phase === 'committed') {
                if (!this.includes(item)) accepted.push(item);
            }
        }
        return Array.prototype.push.apply(this, accepted);
    };
    return subscriptions;
}

async function rollbackActivation(scope) {
    scope.phase = 'rolling-back';
    const deadline = Date.now() + kDisposableTimeoutMs;
    const errors = [];
    while (scope.journal.length > 0) {
        const item = scope.journal.pop();
        errors.push(...await settleDisposable(item, deadline));
    }
    scope.phase = 'failed';
    return errors;
}

const disposable = (dispose) => {
    let disposed = false;
    const value = {
        dispose() {
            if (disposed) return value[disposalBarrier];
            disposed = true;
            return dispose ? dispose() : undefined;
        },
    };
    return trackActivationDisposable(value);
};

const Uri = {
    file(fsPath) {
        const p = String(fsPath || '').replace(/\\/g, '/');
        return {
            scheme: 'file',
            path: p.startsWith('/') ? p : '/' + p,
            fsPath: fsPath,
            toString() { return `file://${this.path}`; },
        };
    },
    parse(value) {
        const raw = String(value || '');
        const schemeSep = raw.indexOf(':');
        const scheme = schemeSep < 0 ? 'unknown' : raw.slice(0, schemeSep);
        const rest = schemeSep < 0 ? raw : raw.slice(schemeSep + 1);
        const p = rest.startsWith('//') ? rest.slice(2) : rest;
        const parts = p.split('/');
        const authority = parts.shift() || '';
        const pathOnly = '/' + parts.join('/');
        return {
            scheme,
            authority,
            path: pathOnly,
            fsPath: pathOnly,
            toString() { return `${scheme}://${authority}${pathOnly}`; },
        };
    },
};

const EventEmitter = (function () {
    return class {
        constructor() { this._listeners = new Map(); }
        get event() {
            return (listener) => {
                this._listeners.set(listener, activationStorage.getStore());
                return disposable(() => this._listeners.delete(listener));
            };
        }
        fire(value) {
            for (const [listener, scope] of Array.from(this._listeners)) {
                try { runWithActivation(scope, () => listener(value)); }
                catch (e) { /* noop */ }
            }
        }
        async fireAsync(value) {
            for (const [listener, scope] of Array.from(this._listeners)) {
                await Promise.resolve().then(
                    () => runWithActivation(scope, () => listener(value)));
            }
        }
        get hasListeners() { return this._listeners.size > 0; }
        dispose() { this._listeners.clear(); }
    };
})();

const neverCancellationDisposable = Object.freeze({ dispose() {} });
const neverCancellationToken = Object.freeze({
    isCancellationRequested: false,
    onCancellationRequested() { return neverCancellationDisposable; },
});

const workspace = {
    workspaceFolders: [],
    async openTextDocument(uri) {
        const target = typeof uri === 'string' ? uri : (uri && uri.fsPath) || '';
        const result = await callHost('vscode.workspace.readTextDocument', { path: target });
        return {
            uri: Uri.file(target),
            fileName: target,
            languageId: guessLanguage(target),
            version: 1,
            getText() { return result.content || ''; },
            lineCount: (result.content || '').split('\n').length,
        };
    },
    async findFiles(pattern, exclude, maxResults) {
        const list = await callHost('vscode.workspace.findFiles', {
            pattern: String(pattern || '*'),
            limit: maxResults || 200,
            recursive: true,
        });
        return (list.entries || []).map((e) => Uri.file(e.name));
    },
    async textSearch(options) {
        const list = await callHost('vscode.workspace.textSearch', options || {});
        return list.results || [];
    },
    getConfiguration(section) {
        return {
            async get(key, defaultValue) {
                const config = await callHost('vscode.workspace.getConfiguration', { section });
                if (key === undefined) return config;
                return (config && Object.prototype.hasOwnProperty.call(config, key))
                    ? config[key] : defaultValue;
            },
            async update(key, value) {
                await callHost('vscode.workspace.updateConfiguration', {
                    section: section || key,
                    value: value,
                });
                return true;
            },
        };
    },
    onDidChangeConfiguration: new EventEmitter().event,
    onDidChangeWorkspaceFolders: new EventEmitter().event,
    onDidOpenTextDocument: new EventEmitter().event,
    onDidCloseTextDocument: new EventEmitter().event,
    onDidSaveTextDocument: new EventEmitter().event,
    fs: {
        async readFile(uri) {
            const p = typeof uri === 'string' ? uri : (uri && uri.fsPath) || '';
            const result = await callHost('vscode.workspace.readTextDocument', { path: p });
            return Buffer.from(result.content || '', 'utf8');
        },
        async writeFile(uri, content) {
            const p = typeof uri === 'string' ? uri : (uri && uri.fsPath) || '';
            const text = Buffer.isBuffer(content)
                ? content.toString('utf8')
                : String(content || '');
            await callHost('vscode.workspace.writeTextDocument', { path: p, content: text });
        },
    },
};

// ----- webview mock helpers ----------------------------------------------
//
// The SAO C++ side (webview_bridge.{h,cpp}) spawns a WebView2 window and pumps
// PostWebMessageAsJson/WebMessageReceived.  The mock objects here give the
// extension the same host-routed surface: html / options are stored,
// postMessage waits for the native bridge result, and onDidReceiveMessage is a plain
// EventEmitter the host can drive via the `webview.postToView` handler below.

function makeMockWebview(target) {
    // `target` describes how the host should identify this webview when
    // routing messages: either {panelId} or {viewId}.
    const messageEmitter = new EventEmitter();
    const webview = {
        html: '',
        options: {},
        cspSource: 'sao-webview:',
        async postMessage(message) {
            try {
                const snapshot = boundedJsonSnapshot(message);
                if (!snapshot.ok) return false;
                const result = await callHost(
                    'vscode.webview.postMessage',
                    Object.assign({}, target, { message: snapshot.value }));
                return Boolean(result && result.accepted === true);
            } catch (err) {
                process_.stderr.write(`[shim] webview.postMessage failed: ${err.message}\n`);
                return false;
            }
        },
        onDidReceiveMessage: messageEmitter.event,
        asWebviewUri(uri) { return uri; },
    };
    return { webview, messageEmitter };
}

function makeMockPanel(panelId, viewType, title, showOptions, options, scope) {
    const owner = {
        panelId,
        extensionId: scope.extensionId,
        generation: scope.generation,
    };
    const { webview, messageEmitter } = makeMockWebview(owner);
    const optionsSnapshot = boundedJsonSnapshot(
        options || {}, kMaximumWebviewInventoryBytes);
    if (!optionsSnapshot.ok || !optionsSnapshot.value ||
        typeof optionsSnapshot.value !== 'object' ||
        Array.isArray(optionsSnapshot.value)) {
        throw new Error('webview panel options exceed the inventory budget');
    }
    webview.options = optionsSnapshot.value;
    const disposeEmitter = new EventEmitter();
    const viewStateEmitter = new EventEmitter();
    const requestedColumn = Number(showOptions && showOptions.viewColumn);
    const viewColumn = Number.isSafeInteger(requestedColumn)
        ? requestedColumn : vscodeModule.ViewColumn.One;
    const panel = {
        panelId,
        viewType,
        title,
        webview,
        active: true,
        visible: true,
        viewColumn,
        onDidDispose: disposeEmitter.event,
        onDidChangeViewState: viewStateEmitter.event,
        reveal(_viewColumn, _preserveFocus) {
            panel.visible = true;
            panel.active = true;
            viewStateEmitter.fire({ webviewPanel: panel });
        },
        dispose() {
            if (!state.webviewPanels.has(panelId)) return;
            state.webviewPanels.delete(panelId);
            panel[disposalBarrier] = callHost(
                'vscode.window.disposeWebviewPanel', { panelId })
                .catch((err) => {
                    process_.stderr.write(
                        `[shim] webviewPanel.dispose failed: ${err.message}\n`);
                });
            try { disposeEmitter.fire(); } catch (e) { /* noop */ }
        },
    };
    state.webviewPanels.set(panelId, {
        panel, webview, messageEmitter, disposeEmitter, viewStateEmitter,
        extensionId: scope.extensionId,
        generation: scope.generation,
        activation: scope,
    });
    return panel;
}

function treeDragAndDropMetadata(record) {
    const result = {
        canDrag: false,
        canDrop: false,
        dragMimeTypes: [],
        dropMimeTypes: [],
    };
    try {
        const controller = record && (record.dragAndDropController ||
            record.provider && (record.provider.dragAndDropController ||
                record.provider));
        result.canDrag = Boolean(controller &&
            typeof controller.handleDrag === 'function');
        result.canDrop = Boolean(controller &&
            typeof controller.handleDrop === 'function');
        const mimeTypes = values => Array.isArray(values)
            ? values.slice(0, 64).map(value => String(value)).filter(value =>
                Buffer.byteLength(value, 'utf8') <= 256 &&
                !/[\u0000-\u001f\u007f]/.test(value))
            : [];
        result.dragMimeTypes = mimeTypes(
            controller && controller.dragMimeTypes);
        result.dropMimeTypes = mimeTypes(
            controller && controller.dropMimeTypes);
    } catch (_) {
        return {
            canDrag: false,
            canDrop: false,
            dragMimeTypes: [],
            dropMimeTypes: [],
        };
    }
    return result;
}

function treeResponse(record, fields) {
    const result = Object.assign({
        ok: false,
        available: Boolean(record && record.active),
        applied: false,
        items: [],
        errorCode: '',
        error: '',
        viewVersion: record ? record.viewVersion : 0,
    }, fields || {});
    if (!Array.isArray(result.items)) result.items = [];
    if (record) {
        result.viewId = record.viewId;
        result.extensionId = record.extensionId;
        result.generation = record.generation;
        result.title = record.title || '';
        result.description = record.description || '';
        result.message = record.message || '';
        result.badge = record.badge || null;
        result.visible = record.visible === true;
        result.dragAndDrop = treeDragAndDropMetadata(record);
    }
    return result;
}

function boundedTreeErrorText(value, maximum, fallback) {
    const source = String(value || fallback)
        .replace(/[\u0000-\u001f\u007f]/g, ' ');
    let result = '';
    let bytes = 0;
    for (const character of source) {
        const size = Buffer.byteLength(character, 'utf8');
        if (bytes + size > maximum) break;
        result += character;
        bytes += size;
    }
    return result || fallback;
}

function treeFailure(record, errorCode, error, available) {
    const cleanCode = boundedTreeErrorText(
        errorCode, 128, 'TREE_CALLBACK_FAILED');
    const cleanError = boundedTreeErrorText(
        error, 4096, 'tree provider callback failed');
    return treeResponse(record, {
        ok: false,
        available: available === undefined
            ? Boolean(record && record.active) : Boolean(available),
        applied: false,
        items: [],
        errorCode: cleanCode,
        error: cleanError,
    });
}

function treeError(code, message) {
    const cleanCode = boundedTreeErrorText(
        code, 128, 'TREE_CALLBACK_FAILED');
    const cleanMessage = boundedTreeErrorText(message, 4096, cleanCode);
    const error = new Error(cleanMessage);
    error.treeCode = cleanCode;
    error[treeErrorMarker] = true;
    return error;
}

function advanceTreeFence(value, label) {
    if (!Number.isSafeInteger(value) || value < 0 ||
        value >= Number.MAX_SAFE_INTEGER) {
        throw treeError('TREE_GENERATION_EXHAUSTED',
            `${label || 'tree generation'} is exhausted`);
    }
    return value + 1;
}

async function invokeTreeCallback(callback, args, errorCode, scope) {
    let timer = null;
    try {
        return await Promise.race([
            Promise.resolve().then(
                () => runWithActivation(scope, () => callback(...args))),
            new Promise((_, reject) => {
                timer = setTimeout(() => reject(treeError(
                    'TREE_CALLBACK_TIMEOUT', 'tree provider callback timed out')),
                    kTreeCallbackTimeoutMs);
            }),
        ]);
    } catch (error) {
        if (error && error[treeErrorMarker]) throw error;
        throw treeError(errorCode || 'TREE_CALLBACK_FAILED',
            error && error.message ? error.message : String(error));
    } finally {
        if (timer !== null) clearTimeout(timer);
    }
}

function treeText(value) {
    if (typeof value === 'string') return value;
    if (typeof value === 'number' || typeof value === 'boolean') return String(value);
    if (value && typeof value === 'object') {
        if (typeof value.label === 'string') return value.label;
        if (typeof value.value === 'string') return value.value;
    }
    return '';
}

function boundedTreeText(value, field) {
    const text = treeText(value);
    if (Buffer.byteLength(text, 'utf8') > kMaximumTreeLabelBytes ||
        /[\u0000-\u001f\u007f]/.test(text)) {
        throw treeError('TREE_LABEL_BUDGET_EXCEEDED',
            `${field || 'tree text'} exceeds the text limit`);
    }
    return text;
}

function setTreeMetadataText(record, key, value, field) {
    const text = boundedTreeText(value, field);
    const values = {
        title: record.title || '',
        description: record.description || '',
        message: record.message || '',
        badge: record.badge && record.badge.tooltip || '',
    };
    values[key] = text;
    const bytes = Object.values(values).reduce(
        (total, item) => total + Buffer.byteLength(item, 'utf8'), 0);
    if (bytes > kMaximumTreeLabelBytes) {
        throw treeError('TREE_LABEL_BUDGET_EXCEEDED',
            'tree view metadata exceeds the 64 KiB aggregate limit');
    }
    const changed = record[key] !== text;
    record[key] = text;
    return changed;
}

function notifyTreeMetadataChanged(record) {
    if (!record || !record.active) return;
    callHost('sao.host.log', {
        kind: 'extension_tree_changed',
        viewId: record.viewId,
        extensionId: record.extensionId,
        generation: record.generation,
        viewVersion: record.viewVersion,
    }).catch(error => {
        process_.stderr.write(
            `[shim] tree metadata notification failed: ${error.message}\n`);
    });
}

function treeUri(value) {
    if (!value) return '';
    if (typeof value === 'string') return value;
    if (typeof value.toString === 'function' &&
        value.toString !== Object.prototype.toString) {
        try { return String(value.toString()); } catch (_) { return ''; }
    }
    return String(value.fsPath || value.path || '');
}

function treeIcon(iconPath) {
    if (!iconPath) return null;
    if (typeof iconPath === 'string') return { path: iconPath };
    if (iconPath.id) {
        return {
            kind: 'theme',
            id: String(iconPath.id),
            color: iconPath.color && iconPath.color.id
                ? String(iconPath.color.id) : undefined,
        };
    }
    if (typeof iconPath === 'object') {
        const light = treeUri(iconPath.light);
        const dark = treeUri(iconPath.dark);
        if (light || dark) return { light, dark };
    }
    if (iconPath.fsPath || iconPath.path ||
        (typeof iconPath.toString === 'function' &&
         iconPath.toString !== Object.prototype.toString)) {
        return { path: treeUri(iconPath) };
    }
    return null;
}

function treeCheckbox(raw) {
    if (raw === undefined || raw === null) return null;
    if (typeof raw === 'number' || typeof raw === 'boolean') {
        return { isChecked: raw === true || raw === 1, tooltip: '' };
    }
    if (typeof raw !== 'object') return null;
    const stateValue = raw.state !== undefined ? raw.state
        : (raw.isChecked !== undefined ? raw.isChecked : raw.checked);
    return {
        isChecked: stateValue === true || stateValue === 1,
        tooltip: treeText(raw.tooltip),
    };
}

function addTreeLabelBytes(budget, value) {
    const text = String(value || '');
    const bytes = Buffer.byteLength(text, 'utf8');
    if (/[\u0000-\u001f\u007f]/.test(text) ||
        budget.bytes + bytes > kMaximumTreeLabelBytes) {
        throw treeError('TREE_LABEL_BUDGET_EXCEEDED',
            'tree labels exceed the 64 KiB aggregate limit');
    }
    budget.bytes += bytes;
}

function resetTreeHandles(record) {
    record.handles.clear();
    record.objectHandles = new WeakMap();
    record.primitiveHandles = new Map();
    record.nextHandle = 1;
    record.selection = [];
    record.expandedHandles.clear();
    if (record.treeView) record.treeView._setSelection([]);
}

function allocateTreeHandle(record, element, treeItem, depth) {
    const objectElement = element !== null &&
        (typeof element === 'object' || typeof element === 'function');
    const existingHandle = objectElement
        ? record.objectHandles.get(element)
        : record.primitiveHandles.get(element);
    const existing = existingHandle && record.handles.get(existingHandle);
    if (existing && existing.version === record.viewVersion) {
        existing.treeItem = treeItem;
        existing.depth = depth;
        return existingHandle;
    }
    if (record.handles.size >= kMaximumTreeHandles) {
        throw treeError('TREE_HANDLE_LIMIT', 'tree provider handle limit reached');
    }
    const handle = [record.generation, record.epoch, record.viewVersion,
        record.nextHandle++].join(':');
    record.handles.set(handle, {
        element,
        treeItem,
        depth,
        version: record.viewVersion,
    });
    if (objectElement) record.objectHandles.set(element, handle);
    else record.primitiveHandles.set(element, handle);
    return handle;
}

function normalizeTreeCommand(raw) {
    if (!raw) return null;
    const normalizeId = value => {
        const id = String(value || '');
        if (!id || Buffer.byteLength(id, 'utf8') > 256 ||
            /[\u0000-\u001f\u007f]/.test(id)) {
            throw treeError('TREE_COMMAND_INVALID',
                'tree command id is invalid');
        }
        return id;
    };
    if (typeof raw === 'string') {
        const command = normalizeId(raw);
        return { command, title: command, arguments: [] };
    }
    if (typeof raw !== 'object' || !raw.command) return null;
    const command = normalizeId(raw.command);
    return {
        command,
        title: treeText(raw.title) || command,
        tooltip: treeText(raw.tooltip),
        arguments: [],
    };
}

async function serializeTreeItem(record, element, depth, budget) {
    if (!record.active) {
        throw treeError('TREE_PROVIDER_STALE',
            'tree provider generation is stale');
    }
    let raw = await invokeTreeCallback(
        record.provider.getTreeItem.bind(record.provider), [element],
        'TREE_GET_ITEM_FAILED', record.activation);
    if (!record.active) {
        throw treeError('TREE_PROVIDER_STALE',
            'tree provider generation is stale');
    }
    if (!raw || typeof raw !== 'object') {
        throw treeError('TREE_ITEM_INVALID',
            'TreeDataProvider.getTreeItem returned no TreeItem');
    }
    if (typeof record.provider.resolveTreeItem === 'function') {
        const resolved = await invokeTreeCallback(
            record.provider.resolveTreeItem.bind(record.provider),
            [raw, element, neverCancellationToken],
            'TREE_RESOLVE_ITEM_FAILED', record.activation);
        if (!record.active) {
            throw treeError('TREE_PROVIDER_STALE',
                'tree provider generation is stale');
        }
        if (resolved && typeof resolved === 'object') raw = resolved;
    }
    let label = treeText(raw.label);
    if (!label && (typeof element === 'string' || typeof element === 'number')) {
        label = String(element);
    }
    const description = raw.description === true
        ? '' : treeText(raw.description);
    const tooltip = treeText(raw.tooltip);
    addTreeLabelBytes(budget, label);
    addTreeLabelBytes(budget, description);
    addTreeLabelBytes(budget, tooltip);
    const collapsibleState = Number.isInteger(raw.collapsibleState)
        ? Math.max(0, Math.min(2, raw.collapsibleState)) : 0;
    const command = normalizeTreeCommand(raw.command);
    if (command) {
        const registration = state.commandRegistrations.get(command.command);
        const available = Boolean(registration && registration.active &&
            registration.extensionId === record.extensionId &&
            registration.generation === record.generation);
        command.runtimeAvailable = available;
        command.enabled = available;
        command.disabled = !available;
        if (!available) {
            command.disabledReason =
                'Extension command handler is unavailable.';
        }
        addTreeLabelBytes(budget, command.title);
        addTreeLabelBytes(budget, command.tooltip);
    }
    const checkbox = treeCheckbox(raw.checkboxState !== undefined
        ? raw.checkboxState : raw.checkbox);
    if (checkbox) addTreeLabelBytes(budget, checkbox.tooltip);
    let accessibility = null;
    if (raw.accessibilityInformation &&
        typeof raw.accessibilityInformation === 'object') {
        accessibility = {
            label: treeText(raw.accessibilityInformation.label),
            role: treeText(raw.accessibilityInformation.role),
        };
        addTreeLabelBytes(budget, accessibility.label);
        addTreeLabelBytes(budget, accessibility.role);
    }
    const contextValue = treeText(raw.contextValue);
    const resourceUri = treeUri(raw.resourceUri);
    addTreeLabelBytes(budget, contextValue);
    addTreeLabelBytes(budget, resourceUri);
    const handle = allocateTreeHandle(record, element, raw, depth);
    const item = {
        handle,
        label,
        description,
        tooltip,
        contextValue,
        resourceUri,
        collapsibleState,
        children: [],
        childrenLoaded: false,
        lazyChildren: collapsibleState > 0,
        actions: [],
    };
    const iconPath = treeIcon(raw.iconPath);
    if (iconPath) item.iconPath = iconPath;
    if (checkbox) item.checkbox = checkbox;
    if (command) {
        item.command = command;
        item.actions.push(Object.assign({ inline: false }, command));
    }
    if (accessibility) item.accessibilityInformation = accessibility;
    return item;
}

async function serializeTreeChildren(record, parentEntry) {
    const depth = parentEntry ? parentEntry.depth + 1 : 0;
    if (depth >= kMaximumTreeDepth) {
        throw treeError('TREE_DEPTH_EXCEEDED',
            'tree depth exceeds the limit of 64');
    }
    const raw = await invokeTreeCallback(
        record.provider.getChildren.bind(record.provider),
        [parentEntry ? parentEntry.element : undefined],
        'TREE_GET_CHILDREN_FAILED', record.activation);
    if (!record.active) {
        throw treeError('TREE_PROVIDER_STALE',
            'tree provider generation is stale');
    }
    let children;
    if (raw === undefined || raw === null) {
        children = [];
    } else if (Array.isArray(raw)) {
        if (raw.length > kMaximumTreeChildren) {
            throw treeError('TREE_CHILD_LIMIT',
                'tree provider returned more than 500 children');
        }
        children = raw.slice();
    } else if (typeof raw[Symbol.iterator] === 'function') {
        children = [];
        for (const child of raw) {
            if (children.length >= kMaximumTreeChildren) {
                throw treeError('TREE_CHILD_LIMIT',
                    'tree provider returned more than 500 children');
            }
            children.push(child);
        }
    } else {
        throw treeError('TREE_CHILDREN_INVALID',
            'TreeDataProvider.getChildren returned a non-iterable value');
    }
    const budget = { bytes: 0 };
    const items = [];
    const responseHandles = new Set();
    for (const child of children) {
        const item = await serializeTreeItem(record, child, depth, budget);
        if (responseHandles.has(item.handle)) {
            throw treeError('TREE_DUPLICATE_HANDLE',
                'tree provider returned duplicate child elements');
        }
        responseHandles.add(item.handle);
        items.push(item);
    }
    if (items.some(item => {
        const entry = record.handles.get(item.handle);
        return !entry || entry.version !== record.viewVersion;
    })) {
        throw treeError('TREE_STALE_VERSION',
            'tree provider changed during item serialization');
    }
    if (Buffer.byteLength(JSON.stringify(items), 'utf8') >
        kMaximumTreePayloadBytes) {
        throw treeError('TREE_PAYLOAD_BUDGET_EXCEEDED',
            'tree response exceeds the 512 KiB payload limit');
    }
    return items;
}

function requestedTreeVersion(params) {
    const value = params && params.viewVersion;
    if (value === undefined || value === null || value === '') return 0;
    const number = Number(value);
    return Number.isSafeInteger(number) && number >= 0 ? number : -1;
}

function validateTreeRequest(record, params, requireHandle) {
    const version = requestedTreeVersion(params);
    if (version < 0) {
        return { error: treeFailure(record, 'TREE_VERSION_INVALID',
            'tree view version is invalid') };
    }
    if (version !== 0 && version !== record.viewVersion) {
        return { error: treeFailure(record, 'TREE_STALE_VERSION',
            'tree view version is stale') };
    }
    const handle = String(params && params.handle || '');
    if (!handle) {
        if (requireHandle) {
            return { error: treeFailure(record, 'TREE_HANDLE_REQUIRED',
                'tree item handle is required') };
        }
        return { entry: null };
    }
    const entry = record.handles.get(handle);
    if (!entry || entry.version !== record.viewVersion) {
        return { error: treeFailure(record, 'TREE_STALE_HANDLE',
            'tree item handle is stale') };
    }
    return { entry };
}

function enqueueTreeOperation(viewId, params, operation) {
    const key = String(viewId || '');
    const record = state.treeDataProviders.get(key);
    if (!record || !record.active) {
        const tombstone = state.treeProviderTombstones.get(key);
        return Promise.resolve(treeResponse(null, {
            ok: false,
            available: false,
            applied: false,
            items: [],
            errorCode: tombstone ? 'TREE_PROVIDER_STALE'
                : 'TREE_PROVIDER_UNAVAILABLE',
            error: tombstone ? 'tree provider generation is stale'
                : 'TreeDataProvider is not registered',
            viewVersion: tombstone ? tombstone.viewVersion
                : Math.max(0, requestedTreeVersion(params)),
            viewId: key,
        }));
    }
    const run = record.queue.then(async () => {
        if (!record.active || state.treeDataProviders.get(key) !== record) {
            return treeFailure(record, 'TREE_PROVIDER_STALE',
                'tree provider generation is stale', false);
        }
        if (record.callbackQuarantined) {
            return treeFailure(record, 'TREE_CALLBACK_QUARANTINED',
                'tree provider has an unresolved callback', false);
        }
        try {
            let timer = null;
            try {
                const result = await Promise.race([
                    Promise.resolve().then(() => operation(record)),
                    new Promise((_, reject) => {
                        timer = setTimeout(() => reject(treeError(
                            'TREE_CALLBACK_TIMEOUT',
                            'tree provider operation timed out')),
                            kTreeCallbackTimeoutMs);
                    }),
                ]);
                return result;
            } finally {
                if (timer !== null) clearTimeout(timer);
            }
        } catch (error) {
            const internal = Boolean(error && error[treeErrorMarker]);
            if (internal && error.treeCode === 'TREE_CALLBACK_TIMEOUT') {
                record.callbackQuarantined = true;
            }
            return treeFailure(record,
                internal && error.treeCode || 'TREE_CALLBACK_FAILED',
                error && error.message || String(error),
                record.callbackQuarantined ? false : undefined);
        }
    });
    record.queue = run.then(() => undefined, () => undefined);
    return run;
}

function registerTreeDataProvider(viewId, provider) {
    const key = String(viewId || '');
    if (!key || key.includes('\0') ||
        Buffer.byteLength(key, 'utf8') > kMaximumTreeViewIdBytes ||
        !provider || typeof provider.getChildren !== 'function' ||
        typeof provider.getTreeItem !== 'function') {
        throw new Error('registerTreeDataProvider requires viewId, getChildren, and getTreeItem');
    }
    const scope = requireActivationScope('registerTreeDataProvider');
    const existing = state.treeDataProviders.get(key);
    if (existing && existing.active) {
        throw new Error(`TreeDataProvider already registered: ${key}`);
    }
    if (!existing && state.treeDataProviders.size >= kMaximumTreeProviders) {
        throw new Error('TreeDataProvider registration limit reached');
    }
    if (!state.treeProviderEpochs.has(key) &&
        state.treeProviderEpochs.size >= kMaximumTreeProviderIdentities) {
        throw new Error('TreeDataProvider identity limit reached');
    }
    const epoch = advanceTreeFence(
        state.treeProviderEpochs.get(key) || 0, 'tree provider epoch');
    state.treeProviderEpochs.set(key, epoch);
    const tombstone = state.treeProviderTombstones.get(key);
    const viewVersion = advanceTreeFence(
        tombstone && tombstone.viewVersion || 0, 'tree view version');
    const record = {
        viewId: key,
        provider,
        extensionId: scope.extensionId,
        generation: scope.generation,
        activation: scope,
        epoch,
        viewVersion,
        nextHandle: 1,
        handles: new Map(),
        objectHandles: new WeakMap(),
        primitiveHandles: new Map(),
        queue: Promise.resolve(),
        active: true,
        changeDisposable: null,
        visible: false,
        selection: [],
        expandedHandles: new Set(),
        callbackQuarantined: false,
        treeView: null,
        dragAndDropController: null,
        title: '',
        description: '',
        message: '',
        badge: null,
    };
    state.treeDataProviders.set(key, record);
    if (typeof provider.onDidChangeTreeData === 'function') {
        try {
            record.changeDisposable = provider.onDidChangeTreeData(() => {
                if (!record.active || state.treeDataProviders.get(key) !== record) return;
                try {
                    record.viewVersion = advanceTreeFence(
                        record.viewVersion, 'tree view version');
                } catch (error) {
                    record.callbackQuarantined = true;
                    resetTreeHandles(record);
                    notifyTreeMetadataChanged(record);
                    return;
                }
                resetTreeHandles(record);
                callHost('sao.host.log', {
                    kind: 'extension_tree_changed',
                    viewId: record.viewId,
                    extensionId: record.extensionId,
                    generation: record.generation,
                    viewVersion: record.viewVersion,
                }).catch(error => {
                    process_.stderr.write(
                        `[shim] tree change notification failed: ${error.message}\n`);
                });
            });
        } catch (error) {
            state.treeDataProviders.delete(key);
            throw error;
        }
    }
    let registration;
    registration = disposable(() => {
        if (!record.active) return registration[disposalBarrier];
        record.active = false;
        try {
            record.viewVersion = advanceTreeFence(
                record.viewVersion, 'tree view version');
        } catch (_) {
            record.callbackQuarantined = true;
        }
        resetTreeHandles(record);
        const changeDisposable = record.changeDisposable;
        record.changeDisposable = null;
        const changeDisposal = changeDisposable &&
            typeof changeDisposable.dispose === 'function'
            ? settleDisposable(changeDisposable) : Promise.resolve([]);
        if (state.treeDataProviders.get(key) === record) {
            state.treeDataProviders.delete(key);
        }
        state.treeProviderTombstones.set(key, {
            extensionId: record.extensionId,
            generation: record.generation,
            viewVersion: record.viewVersion,
        });
        callHost('sao.host.log', {
            kind: 'extension_tree_changed',
            viewId: record.viewId,
            extensionId: record.extensionId,
            generation: record.generation,
            viewVersion: record.viewVersion,
            active: false,
        }).catch(error => {
            process_.stderr.write(
                `[shim] tree disposal notification failed: ${error.message}\n`);
        });
        const barrier = Promise.all([
            Promise.resolve(record.queue),
            changeDisposal,
        ]).then(([, disposalErrors]) => {
            if (disposalErrors.length) {
                throw new Error(disposalErrors.join('; '));
            }
        });
        registration[disposalBarrier] = barrier;
        return barrier;
    });
    notifyTreeMetadataChanged(record);
    return registration;
}

function createTreeView(viewId, options) {
    const key = String(viewId || '');
    const settings = options && typeof options === 'object' ? options : {};
    if (!key || !settings.treeDataProvider) {
        throw new Error('createTreeView requires viewId and treeDataProvider');
    }
    const providerRegistration = registerTreeDataProvider(
        key, settings.treeDataProvider);
    const record = state.treeDataProviders.get(key);
    const expandEmitter = new EventEmitter();
    const collapseEmitter = new EventEmitter();
    const selectionEmitter = new EventEmitter();
    const visibilityEmitter = new EventEmitter();
    const checkboxEmitter = new EventEmitter();
    let selection = [];
    let disposed = false;
    const view = {
        id: key,
        onDidExpandElement: expandEmitter.event,
        onDidCollapseElement: collapseEmitter.event,
        onDidChangeSelection: selectionEmitter.event,
        onDidChangeVisibility: visibilityEmitter.event,
        onDidChangeCheckboxState: checkboxEmitter.event,
        reveal(_element, _revealOptions) {
            return Promise.reject(treeError('TREE_REVEAL_UNAVAILABLE',
                'tree reveal has no frontend acknowledgement route'));
        },
        dispose() {
            if (disposed) return view[disposalBarrier];
            disposed = true;
            if (state.treeViews.get(key) === view) state.treeViews.delete(key);
            const wasVisible = Boolean(record && record.visible);
            if (record && record.treeView === view) {
                record.visible = false;
                record.treeView = null;
                record.dragAndDropController = null;
            }
            const visibilityResult = wasVisible && visibilityEmitter.hasListeners
                ? invokeTreeCallback(visibilityEmitter.fireAsync.bind(
                    visibilityEmitter), [{ visible: false }],
                    'TREE_VISIBILITY_EVENT_FAILED', record.activation)
                : Promise.resolve();
            let providerResult;
            try { providerResult = providerRegistration.dispose(); }
            catch (error) { providerResult = Promise.reject(error); }
            const barrier = Promise.allSettled([
                Promise.resolve(visibilityResult),
                Promise.resolve(providerResult),
            ]).then(results => {
                const failed = results.find(result => result.status === 'rejected');
                if (failed) throw failed.reason;
            }).finally(() => {
                expandEmitter.dispose();
                collapseEmitter.dispose();
                selectionEmitter.dispose();
                visibilityEmitter.dispose();
                checkboxEmitter.dispose();
            });
            view[disposalBarrier] = barrier;
            return barrier;
        },
        _expandEmitter: expandEmitter,
        _collapseEmitter: collapseEmitter,
        _selectionEmitter: selectionEmitter,
        _visibilityEmitter: visibilityEmitter,
        _checkboxEmitter: checkboxEmitter,
        _setSelection(value) {
            selection = Array.isArray(value) ? value.slice() : [];
            if (record) record.selection = selection.slice();
        },
    };
    Object.defineProperties(view, {
        visible: { enumerable: true, get: () => Boolean(record && record.visible) },
        selection: { enumerable: true, get: () => selection.slice() },
        title: {
            enumerable: true,
            get: () => record && record.title || '',
            set: value => {
                if (record && setTreeMetadataText(
                        record, 'title', value, 'tree title')) {
                    notifyTreeMetadataChanged(record);
                }
            },
        },
        description: {
            enumerable: true,
            get: () => record && record.description || '',
            set: value => {
                if (record && setTreeMetadataText(
                        record, 'description', value, 'tree description')) {
                    notifyTreeMetadataChanged(record);
                }
            },
        },
        message: {
            enumerable: true,
            get: () => record && record.message || '',
            set: value => {
                if (record && setTreeMetadataText(
                        record, 'message', value, 'tree message')) {
                    notifyTreeMetadataChanged(record);
                }
            },
        },
        badge: {
            enumerable: true,
            get: () => record && record.badge || undefined,
            set: value => {
                if (!record) return;
                const previous = record.badge;
                if (value === null || value === undefined) {
                    record.badge = null;
                    if (previous !== null) notifyTreeMetadataChanged(record);
                    return;
                }
                const number = Number(value && value.value);
                const badgeValue = Math.trunc(number);
                if (!Number.isSafeInteger(badgeValue) || badgeValue < 0) {
                    record.badge = null;
                    if (previous !== null) notifyTreeMetadataChanged(record);
                    return;
                }
                setTreeMetadataText(record, 'badge', value.tooltip,
                    'tree badge tooltip');
                record.badge = {
                    value: badgeValue,
                    tooltip: record.badge,
                };
                if (!previous || previous.value !== record.badge.value ||
                    previous.tooltip !== record.badge.tooltip) {
                    notifyTreeMetadataChanged(record);
                }
            },
        },
    });
    if (record) {
        record.treeView = view;
        record.dragAndDropController = settings.dragAndDropController || null;
        record.visible = false;
    }
    state.treeViews.set(key, view);
    return trackActivationDisposable(view);
}

async function treeChildrenRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const validation = validateTreeRequest(record, params, false);
        if (validation.error) return validation.error;
        const items = await serializeTreeChildren(record, validation.entry);
        return treeResponse(record, {
            ok: true, available: true, applied: true, items,
        });
    });
}

async function treeExpandRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const validation = validateTreeRequest(record, params, true);
        if (validation.error) return validation.error;
        const expanded = Boolean(params && params.expanded);
        const callback = expanded ? record.provider.handleExpand
            : record.provider.handleCollapse;
        if (typeof callback === 'function') {
            await invokeTreeCallback(callback.bind(record.provider),
                [validation.entry.element], 'TREE_EXPAND_CALLBACK_FAILED',
                record.activation);
        }
        const emitter = record.treeView && (expanded
            ? record.treeView._expandEmitter : record.treeView._collapseEmitter);
        if (emitter && emitter.hasListeners) {
            await invokeTreeCallback(emitter.fireAsync.bind(emitter),
                [{ element: validation.entry.element }],
                'TREE_EXPAND_EVENT_FAILED', record.activation);
        }
        const items = expanded
            ? await serializeTreeChildren(record, validation.entry) : [];
        if (expanded) record.expandedHandles.add(String(params.handle));
        else record.expandedHandles.delete(String(params.handle));
        return treeResponse(record, {
            ok: true, available: true, applied: true, items,
        });
    });
}

async function treeCheckboxRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const validation = validateTreeRequest(record, params, true);
        if (validation.error) return validation.error;
        const callback = record.provider.handleCheckboxChange ||
            record.provider.setCheckboxState;
        const emitter = record.treeView && record.treeView._checkboxEmitter;
        const checked = Boolean(params && params.checked);
        if (typeof callback !== 'function' &&
            !(emitter && emitter.hasListeners)) {
            return treeFailure(record, 'TREE_CHECKBOX_UNAVAILABLE',
                'tree checkbox has no acknowledgement callback');
        }
        if (typeof callback === 'function') {
            await invokeTreeCallback(callback.bind(record.provider),
                [validation.entry.element, checked],
                'TREE_CHECKBOX_CALLBACK_FAILED', record.activation);
        }
        if (emitter && emitter.hasListeners) {
            await invokeTreeCallback(emitter.fireAsync.bind(emitter),
                [{ items: [[validation.entry.element, checked ? 1 : 0]] }],
                'TREE_CHECKBOX_EVENT_FAILED', record.activation);
        }
        return treeResponse(record, {
            ok: true, available: true, applied: true, items: [],
        });
    });
}

async function treeSelectRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const validation = validateTreeRequest(record, params, true);
        if (validation.error) return validation.error;
        const callback = record.provider.handleSelection || record.provider.select;
        const emitter = record.treeView && record.treeView._selectionEmitter;
        const previousSelection = record.selection.slice();
        try {
            record.selection = [validation.entry.element];
            if (record.treeView) {
                record.treeView._setSelection([validation.entry.element]);
            }
            if (typeof callback === 'function') {
                await invokeTreeCallback(callback.bind(record.provider),
                    [validation.entry.element], 'TREE_SELECTION_CALLBACK_FAILED',
                    record.activation);
            }
            if (emitter && emitter.hasListeners) {
                await invokeTreeCallback(emitter.fireAsync.bind(emitter),
                    [{ selection: [validation.entry.element] }],
                    'TREE_SELECTION_EVENT_FAILED', record.activation);
            }
        } catch (error) {
            record.selection = previousSelection.slice();
            if (record.treeView) record.treeView._setSelection(previousSelection);
            throw error;
        }
        return treeResponse(record, {
            ok: true, available: true, applied: true, items: [],
        });
    });
}

async function treeDropRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const version = requestedTreeVersion(params);
        if (version < 0 || (version !== 0 && version !== record.viewVersion)) {
            return treeFailure(record, 'TREE_STALE_VERSION',
                'tree view version is stale');
        }
        const handles = Array.isArray(params && params.sourceHandles)
            ? params.sourceHandles : [];
        if (!handles.length || handles.length > kMaximumTreeChildren) {
            return treeFailure(record, 'TREE_DROP_INVALID',
                'tree drop source handles are invalid');
        }
        const sources = [];
        const seenHandles = new Set();
        for (const rawHandle of handles) {
            const handle = String(rawHandle || '');
            if (!handle || seenHandles.has(handle)) {
                return treeFailure(record, 'TREE_DROP_INVALID',
                    'tree drop source handles are duplicated');
            }
            seenHandles.add(handle);
            const entry = record.handles.get(handle);
            if (!entry || entry.version !== record.viewVersion) {
                return treeFailure(record, 'TREE_STALE_HANDLE',
                    'tree drop source handle is stale');
            }
            sources.push(entry.element);
        }
        const targetHandle = String(params && params.targetHandle || '');
        const target = targetHandle ? record.handles.get(targetHandle) : null;
        if (targetHandle && (!target || target.version !== record.viewVersion)) {
            return treeFailure(record, 'TREE_STALE_HANDLE',
                'tree drop target handle is stale');
        }
        const controller = record.dragAndDropController ||
            record.provider.dragAndDropController || record.provider;
        if (!controller || typeof controller.handleDrop !== 'function') {
            return treeFailure(record, 'TREE_DROP_UNAVAILABLE',
                'tree provider has no drop callback', false);
        }
        const transfer = new vscodeModule.DataTransfer();
        const inputData = params && params.data &&
            typeof params.data === 'object' && !Array.isArray(params.data)
            ? params.data : {};
        for (const [mime, value] of Object.entries(inputData).slice(0, 64)) {
            if (Buffer.byteLength(mime, 'utf8') > 256) continue;
            transfer.set(mime, new vscodeModule.DataTransferItem(value));
        }
        if (typeof controller.handleDrag === 'function') {
            await invokeTreeCallback(controller.handleDrag.bind(controller),
                [sources, transfer, neverCancellationToken],
                'TREE_DRAG_CALLBACK_FAILED', record.activation);
        }
        await invokeTreeCallback(controller.handleDrop.bind(controller),
            [target ? target.element : undefined, transfer,
             neverCancellationToken],
            'TREE_DROP_CALLBACK_FAILED', record.activation);
        return treeResponse(record, {
            ok: true, available: true, applied: true, items: [],
        });
    });
}

function treeMenuMatchesView(item, viewId, contextValue) {
    const when = String(item && item.when || '');
    if (!when) return true;
    const hasView = when.includes('view ==') || when.includes('view ===') ||
        when.includes('view =~');
    const hasContainer = when.includes('viewContainer ==') ||
        when.includes('viewContainer ===') || when.includes('viewContainer =~');
    if ((hasView || hasContainer) && !when.includes(viewId)) return false;
    if (contextValue && when.includes('viewItem') &&
        !when.includes(contextValue)) return false;
    return true;
}

function treeActionDeclared(record, entry, command) {
    const itemCommand = normalizeTreeCommand(entry.treeItem && entry.treeItem.command);
    if (itemCommand && itemCommand.command === command) return true;
    const extension = state.extensions.get(record.extensionId);
    const menus = extension && extension.manifest && extension.manifest.contributes &&
        extension.manifest.contributes.menus;
    const rows = menus && Array.isArray(menus['view/item/context'])
        ? menus['view/item/context'] : [];
    const contextValue = treeText(entry.treeItem && entry.treeItem.contextValue);
    return rows.some(item => item && String(item.command || '') === command &&
        treeMenuMatchesView(item, record.viewId, contextValue));
}

async function treeActionRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const validation = validateTreeRequest(record, params, true);
        if (validation.error) return validation.error;
        const command = String(params && params.command || '');
        const args = Array.isArray(params && params.arguments)
            ? params.arguments : [];
        const registration = state.commandRegistrations.get(command);
        if (!command || args.length > 64 || !treeActionDeclared(
                record, validation.entry, command) || !registration ||
            !registration.active || registration.extensionId !== record.extensionId ||
            registration.generation !== record.generation) {
            return treeFailure(record, 'TREE_ACTION_UNAVAILABLE',
                'tree action callback is unavailable', false);
        }
        const selectionEmitter = record.treeView &&
            record.treeView._selectionEmitter;
        const previousSelection = record.selection.slice();
        try {
            record.selection = [validation.entry.element];
            if (record.treeView) {
                record.treeView._setSelection([validation.entry.element]);
            }
            if (selectionEmitter && selectionEmitter.hasListeners) {
                await invokeTreeCallback(selectionEmitter.fireAsync.bind(selectionEmitter),
                    [{ selection: [validation.entry.element] }],
                    'TREE_SELECTION_EVENT_FAILED', record.activation);
            }
            const rawCommand = validation.entry.treeItem &&
                validation.entry.treeItem.command;
            const callbackArgs = rawCommand &&
                String(rawCommand.command || '') === command &&
                Array.isArray(rawCommand.arguments)
                ? rawCommand.arguments
                : [validation.entry.element, ...args];
            await invokeTreeCallback(registration.handler, callbackArgs,
                'TREE_ACTION_CALLBACK_FAILED', registration.activation);
        } catch (error) {
            record.selection = previousSelection.slice();
            if (record.treeView) record.treeView._setSelection(previousSelection);
            throw error;
        }
        return treeResponse(record, {
            ok: true, available: true, applied: true, items: [],
        });
    });
}

async function treeVisibilityRequest(params) {
    const viewId = String(params && params.viewId || '');
    return enqueueTreeOperation(viewId, params, async (record) => {
        const version = requestedTreeVersion(params);
        if (version < 0 || (version !== 0 && version !== record.viewVersion)) {
            return treeFailure(record, 'TREE_STALE_VERSION',
                'tree view version is stale');
        }
        const callback = record.provider.handleVisibilityChange;
        const emitter = record.treeView && record.treeView._visibilityEmitter;
        const visible = Boolean(params && params.visible);
        const previousVisible = record.visible;
        record.visible = visible;
        try {
            if (typeof callback === 'function') {
                await invokeTreeCallback(callback.bind(record.provider),
                    [visible], 'TREE_VISIBILITY_CALLBACK_FAILED',
                    record.activation);
            }
            if (emitter && emitter.hasListeners) {
                await invokeTreeCallback(emitter.fireAsync.bind(emitter),
                    [{ visible }], 'TREE_VISIBILITY_EVENT_FAILED',
                    record.activation);
            }
        } catch (error) {
            record.visible = previousVisible;
            throw error;
        }
        return treeResponse(record, {
            ok: true, available: true, applied: true, items: [],
        });
    });
}

function treeLabelBytes(items) {
    let bytes = 0;
    for (const item of items || []) {
        for (const value of [item && item.label, item && item.description,
            item && item.tooltip, item && item.contextValue,
            item && item.resourceUri, item && item.command && item.command.command,
            item && item.command && item.command.title,
            item && item.command && item.command.tooltip,
            item && item.checkbox && item.checkbox.tooltip,
            item && item.accessibilityInformation &&
                item.accessibilityInformation.label,
            item && item.accessibilityInformation &&
                item.accessibilityInformation.role]) {
            bytes += Buffer.byteLength(String(value || ''), 'utf8');
        }
        for (const action of item && Array.isArray(item.actions)
            ? item.actions : []) {
            for (const value of [action && action.command,
                action && action.submenu, action && action.group,
                action && action.title, action && action.tooltip,
                action && action.disabledReason]) {
                bytes += Buffer.byteLength(String(value || ''), 'utf8');
            }
        }
        bytes += treeLabelBytes(item && item.children);
    }
    return bytes;
}

function treeInventoryLabelBytes(row) {
    let bytes = treeLabelBytes(row && row.items);
    for (const value of [row && row.title, row && row.description,
        row && row.message, row && row.badge && row.badge.tooltip]) {
        bytes += Buffer.byteLength(String(value || ''), 'utf8');
    }
    return bytes;
}

function treeInventoryFailure(record, errorCode, error) {
    return treeResponse(null, {
        ok: false,
        available: Boolean(record && record.active),
        applied: false,
        items: [],
        errorCode,
        error,
        viewVersion: record ? record.viewVersion : 0,
        viewId: record ? record.viewId : '',
        extensionId: record ? record.extensionId : '',
        generation: record ? record.generation : 0,
    });
}

async function treeInventoryRequest() {
    const records = Array.from(state.treeDataProviders.values())
        .filter(record => record.active)
        .sort((left, right) => left.viewId.localeCompare(right.viewId))
        .slice(0, 256);
    if (!records.length) {
        return treeResponse(null, {
            ok: true,
            available: false,
            applied: false,
            items: [],
            errorCode: 'TREE_PROVIDER_UNAVAILABLE',
            error: 'No TreeDataProvider is registered',
            viewVersion: 0,
        });
    }
    const rows = await Promise.all(records.map(record =>
        enqueueTreeOperation(record.viewId, { viewVersion: record.viewVersion },
            async current => {
                const items = await serializeTreeChildren(current, null);
                return treeResponse(current, {
                    ok: true, available: true, applied: true, items,
                });
            })));
    let aggregateBytes = 0;
    let aggregatePayloadBytes = 0;
    for (let index = 0; index < rows.length; ++index) {
        const bytes = treeInventoryLabelBytes(rows[index]);
        const payloadBytes = Buffer.byteLength(
            JSON.stringify(rows[index]), 'utf8');
        if (aggregateBytes + bytes > kMaximumTreeLabelBytes ||
            aggregatePayloadBytes + payloadBytes > kMaximumTreePayloadBytes) {
            const labelExceeded = aggregateBytes + bytes >
                kMaximumTreeLabelBytes;
            rows[index] = treeInventoryFailure(records[index],
                labelExceeded ? 'TREE_LABEL_BUDGET_EXCEEDED'
                    : 'TREE_PAYLOAD_BUDGET_EXCEEDED',
                labelExceeded
                    ? 'tree inventory labels exceed the 64 KiB aggregate limit'
                    : 'tree inventory exceeds the 512 KiB payload limit');
            aggregatePayloadBytes += Buffer.byteLength(
                JSON.stringify(rows[index]), 'utf8');
        } else {
            aggregateBytes += bytes;
            aggregatePayloadBytes += payloadBytes;
        }
    }
    const inventoryRowsBudget = kMaximumTreePayloadBytes - 1024;
    let inventoryRowsBytes = Buffer.byteLength(JSON.stringify(rows), 'utf8');
    for (let index = rows.length - 1;
         inventoryRowsBytes > inventoryRowsBudget && index >= 0; --index) {
        if (rows[index].ok !== true) continue;
        rows[index] = treeInventoryFailure(records[index],
            'TREE_PAYLOAD_BUDGET_EXCEEDED',
            'tree inventory exceeds the 512 KiB payload limit');
        inventoryRowsBytes = Buffer.byteLength(JSON.stringify(rows), 'utf8');
    }
    if (inventoryRowsBytes > inventoryRowsBudget) {
        return treeResponse(null, {
            ok: false,
            available: false,
            applied: false,
            items: [],
            errorCode: 'TREE_PAYLOAD_BUDGET_EXCEEDED',
            error: 'tree inventory exceeds the 512 KiB payload limit',
            viewVersion: 0,
        });
    }
    const applied = rows.length > 0 && rows.every(row => row.applied === true);
    const ok = rows.every(row => row.ok === true);
    return treeResponse(null, {
        ok,
        available: true,
        applied,
        items: rows,
        errorCode: ok ? '' : 'TREE_INVENTORY_PARTIAL',
        error: ok ? '' : 'One or more tree providers failed to enumerate',
        viewVersion: Math.max(...rows.map(row => Number(row.viewVersion) || 0)),
    });
}

const window = {
    activeTextEditor: undefined,
    visibleTextEditors: [],
    async showInformationMessage(message, ...actions) {
        await callHost('vscode.window.showInformationMessage', { message, actions });
        return undefined;
    },
    async showWarningMessage(message, ...actions) {
        await callHost('vscode.window.showWarningMessage', { message, actions });
        return undefined;
    },
    async showErrorMessage(message, ...actions) {
        await callHost('vscode.window.showErrorMessage', { message, actions });
        return undefined;
    },
    async showQuickPick(items, options) {
        await callHost('vscode.window.showQuickPick', { items, options });
        return undefined;
    },
    async showInputBox(options) {
        await callHost('vscode.window.showInputBox', options || {});
        return undefined;
    },
    createOutputChannel(name) {
        let cachedId = null;
        return {
            name,
            async ensureId() {
                if (cachedId) return cachedId;
                const result = await callHost('vscode.window.createOutputChannel', { name });
                cachedId = result.channelId;
                state.outputChannels.set(cachedId, name);
                return cachedId;
            },
            async append(text) {
                const id = await this.ensureId();
                await callHost('vscode.window.appendOutput', { channelId: id, text });
            },
            async appendLine(text) {
                await this.append(String(text) + '\n');
            },
            async clear() { /* not surfaced */ },
            async dispose() { /* not surfaced */ },
            async show() { /* not surfaced */ },
            async hide() { /* not surfaced */ },
        };
    },
    createStatusBarItem(_alignment, _priority) {
        return {
            text: '',
            tooltip: '',
            command: undefined,
            async show() { /* not surfaced */ },
            async hide() { /* not surfaced */ },
            dispose() { /* not surfaced */ },
        };
    },
    onDidChangeActiveTextEditor: new EventEmitter().event,
    onDidChangeVisibleTextEditors: new EventEmitter().event,
    onDidChangeTextEditorSelection: new EventEmitter().event,
    registerTreeDataProvider,
    createTreeView,
    registerWebviewViewProvider(viewId, provider, options) {
        const key = String(viewId || '');
        if (!key || key.includes('\0') ||
            Buffer.byteLength(key, 'utf8') > kMaximumTreeViewIdBytes ||
            !provider || typeof provider.resolveWebviewView !== 'function') {
            throw new Error('registerWebviewViewProvider requires a bounded viewId and provider');
        }
        const scope = requireActivationScope('registerWebviewViewProvider');
        const existing = state.webviewViewProviders.get(key);
        if (existing && existing.active) {
            throw new Error(`WebviewViewProvider already registered: ${key}`);
        }
        if (!existing &&
            state.webviewViewProviders.size >= kMaximumWebviewRegistrations) {
            throw new Error('WebviewViewProvider registration limit reached');
        }
        const optionsSnapshot = boundedJsonSnapshot(
            options || {}, kMaximumWebviewInventoryBytes);
        if (!optionsSnapshot.ok || !optionsSnapshot.value ||
            typeof optionsSnapshot.value !== 'object' ||
            Array.isArray(optionsSnapshot.value)) {
            throw new Error('WebviewViewProvider options exceed the inventory budget');
        }
        const registration = {
            provider,
            options: optionsSnapshot.value,
            active: true,
            extensionId: scope.extensionId,
            generation: scope.generation,
            activation: scope,
        };
        state.webviewViewProviders.set(key, registration);
        return disposable(() => {
            registration.active = false;
            if (state.webviewViewProviders.get(key) !== registration) return;
            state.webviewViewProviders.delete(key);
            state.webviewViews.delete(key);
        });
    },
    createWebviewPanel(viewType, title, showOptions, options) {
        const scope = requireActivationScope('createWebviewPanel');
        if (state.webviewPanels.size >= kMaximumWebviewRegistrations ||
            !Number.isSafeInteger(state.nextPanelId) ||
            state.nextPanelId <= 0 ||
            state.nextPanelId >= Number.MAX_SAFE_INTEGER) {
            throw new Error('WebviewPanel registration limit reached');
        }
        const panelId = 'panel-' + (state.nextPanelId++);
        const viewTypeValue = String(viewType || '');
        const titleValue = String(title || '');
        if (!viewTypeValue || viewTypeValue.includes('\0') ||
            Buffer.byteLength(viewTypeValue, 'utf8') > 256 ||
            titleValue.includes('\0') ||
            Buffer.byteLength(titleValue, 'utf8') > kMaximumTreeLabelBytes) {
            throw new Error('WebviewPanel identity exceeds the registration budget');
        }
        const panel = makeMockPanel(
            panelId, viewTypeValue, titleValue, showOptions, options, scope);
        callHost('vscode.window.createWebviewPanel', {
            panelId,
            viewType: viewTypeValue,
            title: titleValue,
            options: Object.assign({}, panel.webview.options, {
                viewColumn: panel.viewColumn,
            }),
        }).catch((err) => {
            state.webviewPanels.delete(panelId);
            process_.stderr.write(
                `[shim] createWebviewPanel failed: ${err.message}\n`);
        });
        return trackActivationDisposable(panel);
    },
};

function registerExtensionCommand(commandId, handler, editorRequired) {
    const id = String(commandId || '');
    if (!id || id.includes('\0') || Buffer.byteLength(id, 'utf8') > 256 ||
        typeof handler !== 'function') {
        throw new Error('registerCommand requires a bounded command id and handler');
    }
    const scope = requireActivationScope(
        editorRequired ? 'registerTextEditorCommand' : 'registerCommand');
    const existing = state.commandRegistrations.get(id);
    if (existing && existing.active) {
        throw new Error(`command is already registered: ${id}`);
    }
    const registration = {
        handler,
        active: true,
        extensionId: scope.extensionId,
        generation: scope.generation,
        activation: scope,
        editorRequired: editorRequired === true,
    };
    state.commandRegistrations.set(id, registration);
    state.commands.set(id, handler);
    return disposable(() => {
        registration.active = false;
        if (state.commandRegistrations.get(id) === registration) {
            state.commandRegistrations.delete(id);
            state.commands.delete(id);
        }
    });
}

const commands = {
    registerCommand(commandId, handler) {
        return registerExtensionCommand(commandId, handler, false);
    },
    registerTextEditorCommand(commandId, handler) {
        return registerExtensionCommand(commandId, handler, true);
    },
    getCommands(_filterInternal) {
        return Promise.resolve(Array.from(state.commands.keys()));
    },
    async executeCommand(commandId, ...args) {
        // Try local dispatch first, then host bounce.
        const registration = state.commandRegistrations.get(commandId);
        if (registration && registration.active &&
            typeof registration.handler === 'function') {
            try {
                return await Promise.resolve(runWithActivation(
                    registration.activation,
                    () => registration.handler(...args)));
            }
            catch (err) { throw err; }
        }
        return callHost('vscode.commands.executeCommand', {
            command: commandId,
            arguments: args,
        });
    },
};

const languages = {
    async getLanguages() {
        return callHost('vscode.languages.getLanguages', {});
    },
    registerCodeLensProvider() { return disposable(); },
    registerDefinitionProvider() { return disposable(); },
    registerHoverProvider() { return disposable(); },
    registerCompletionItemProvider() { return disposable(); },
    registerDocumentFormattingEditProvider() { return disposable(); },
    setLanguageConfiguration() { return disposable(); },
    createDiagnosticCollection(name) {
        return {
            name,
            set(_uri, _diagnostics) { /* not surfaced */ },
            delete(_uri) { /* not surfaced */ },
            clear() { /* not surfaced */ },
            dispose() { /* not surfaced */ },
        };
    },
};

const env = {
    appName: 'SAO AI Editor',
    appHost: 'sao-native',
    machineId: 'sao-native-machine',
    sessionId: 'sao-native-session',
    language: 'en',
    async openExternal(uri) {
        const target = typeof uri === 'string' ? uri : (uri && uri.toString()) || '';
        await callHost('vscode.env.openExternal', { uri: target });
        return true;
    },
    clipboard: {
        async writeText(_value) { /* not surfaced */ },
        async readText() { return ''; },
    },
};

function guessLanguage(fileName) {
    const ext = path.extname(fileName || '').toLowerCase();
    switch (ext) {
        case '.js': case '.mjs': case '.cjs': return 'javascript';
        case '.ts': return 'typescript';
        case '.jsx': return 'javascriptreact';
        case '.tsx': return 'typescriptreact';
        case '.py': return 'python';
        case '.go': return 'go';
        case '.rs': return 'rust';
        case '.cpp': case '.cc': case '.h': case '.hpp': return 'cpp';
        case '.cs': return 'csharp';
        case '.json': return 'json';
        case '.md': return 'markdown';
        case '.html': return 'html';
        case '.css': return 'css';
        default: return 'plaintext';
    }
}

const vscodeModule = {
    version: '1.90.0',
    Uri,
    EventEmitter,
    TreeItem: class {
        constructor(label, collapsibleState = 0) {
            this.label = label;
            this.collapsibleState = collapsibleState;
        }
    },
    ThemeIcon: class {
        constructor(id, color) {
            this.id = String(id || '');
            this.color = color;
        }
    },
    ThemeColor: class {
        constructor(id) { this.id = String(id || ''); }
    },
    DataTransferItem: class {
        constructor(value) { this.value = value; }
        asString() {
            return Promise.resolve(typeof this.value === 'string'
                ? this.value : String(this.value === undefined ? '' : this.value));
        }
        asFile() { return undefined; }
    },
    DataTransfer: class extends Map {},
    Disposable: class {
        static from(...ds) {
            return disposable(async () => {
                const results = await Promise.allSettled(ds.map(item =>
                    Promise.resolve().then(() => item &&
                        typeof item.dispose === 'function'
                        ? item.dispose() : undefined)));
                const failed = results.find(result => result.status === 'rejected');
                if (failed) throw failed.reason;
            });
        }
        constructor(dispose) {
            this._dispose = dispose;
            trackActivationDisposable(this);
        }
        dispose() {
            const callback = this._dispose;
            this._dispose = undefined;
            return callback ? callback() : undefined;
        }
    },
    workspace,
    window,
    commands,
    languages,
    env,
    ExtensionContext: class { constructor() { this.subscriptions = []; } },
    ExtensionMode: { Development: 1, Production: 2, Test: 3 },
    ConfigurationTarget: { Global: 1, Workspace: 2, WorkspaceFolder: 3 },
    TreeItemCollapsibleState: { None: 0, Collapsed: 1, Expanded: 2 },
    TreeItemCheckboxState: { Unchecked: 0, Checked: 1 },
    ViewColumn: { Active: -1, Beside: -2, One: 1, Two: 2, Three: 3 },
    StatusBarAlignment: { Left: 1, Right: 2 },
};

// Install `require('vscode')` before any extension loads.
const originalResolveFilename = Module._resolveFilename;
Module._resolveFilename = function (request, parent, isMain, options) {
    if (request === 'vscode') { return 'vscode'; }
    return originalResolveFilename.call(this, request, parent, isMain, options);
};
require.cache['vscode'] = { id: 'vscode', filename: 'vscode', loaded: true, exports: vscodeModule };

// ----- host request handlers ---------------------------------------------

state.handlers.set('host.initialize', async (_params) => {
    state.initialized = true;
    return {
        protocolVersion: 'sao-ai-editor/1',
        serverInfo: { name: 'sao-extension-host-shim', version: '1.0' },
        capabilities: {
            extensions: { activate: true, deactivate: true, treeDataProvider: true },
            commands: { execute: true },
        },
    };
});

state.handlers.set('host.activate', async (params) => {
    const extensionId = String(params.extensionId || '');
    const extensionPath = String(params.extensionPath || '');
    const mainRelative = String(params.main || '');
    const mainPath = String(params.mainPath || '');
    const generation = Number(params.generation);
    if (!extensionId || !extensionPath || !mainRelative || !mainPath ||
        !Number.isSafeInteger(generation) || generation <= 0) {
        throw new Error('extensionId, extensionPath, main, mainPath, and generation required');
    }
    if (state.extensions.has(extensionId)) {
        throw new Error(`extension is already active: ${extensionId}`);
    }
    let canonicalRoot;
    let resolved;
    try {
        canonicalRoot = fs.realpathSync(extensionPath);
        resolved = fs.realpathSync(mainPath);
        const rootStat = fs.statSync(canonicalRoot);
        const mainStat = fs.statSync(resolved);
        const relative = path.relative(canonicalRoot, resolved);
        if (!rootStat.isDirectory() || !mainStat.isFile() || !relative ||
            relative === '..' || relative.startsWith('..' + path.sep) ||
            path.isAbsolute(relative)) {
            throw new Error('main is outside extension root');
        }
        if (mainStat.size > kMaximumExtensionMainBytes) {
            throw new Error('main exceeds 1 MiB');
        }
        const requested = path.resolve(canonicalRoot, mainRelative);
        if (fs.realpathSync(requested) !== resolved) {
            throw new Error('main binding changed after registration');
        }
    } catch (error) {
        throw new Error(`extension path validation failed: ${error.message}`);
    }
    const activation = {
        phase: 'active',
        journal: [],
        seen: new WeakSet(),
        extensionId,
        generation,
    };
    const context = {
        subscriptions: makeActivationSubscriptions(activation),
        extensionPath: canonicalRoot,
        extensionUri: Uri.file(canonicalRoot),
        workspaceState: makeMemento(),
        globalState: makeMemento(),
        secrets: {
            async store(_key, _value) { /* not surfaced */ },
            async get(_key) { return undefined; },
            async delete(_key) { /* not surfaced */ },
        },
        environmentVariableCollection: {
            replace() {}, append() {}, prepend() {}, get() {}, forEach() {}, delete() {}, clear() {},
        },
        asAbsolutePath(rel) { return path.join(canonicalRoot, rel); },
        storagePath: canonicalRoot,
        globalStoragePath: canonicalRoot,
        logPath: canonicalRoot,
        extensionMode: vscodeModule.ExtensionMode.Production,
    };
    activation.subscriptions = context.subscriptions;
    let activated;
    try {
        activated = await activationStorage.run(activation, async () => {
            let mod;
            try {
                // Clear cache so hot-reload works.
                delete require.cache[require.resolve(resolved)];
                mod = require(resolved);
                const rebound = fs.realpathSync(mainPath);
                const reboundStat = fs.statSync(rebound);
                if (rebound !== resolved || !reboundStat.isFile() ||
                    reboundStat.size > kMaximumExtensionMainBytes) {
                    throw new Error('main binding changed during module load');
                }
            } catch (err) {
                throw new Error(`activate failed: ${err.stack || err.message}`);
            }
            let activationResult;
            if (mod && typeof mod.activate === 'function') {
                activationResult = await Promise.resolve(mod.activate(context));
            }
            return { mod, activationResult };
        });
    } catch (error) {
        const cleanupErrors = await rollbackActivation(activation);
        if (cleanupErrors.length) {
            throw transportError(
                'extension activation rollback did not complete', -207,
                { cleanupComplete: false,
                  cleanupErrors: cleanupErrors.slice(0, 16) });
        }
        throw error;
    }
    activation.phase = 'committed';
    for (const item of activation.journal) {
        if (!context.subscriptions.includes(item)) {
            Array.prototype.push.call(context.subscriptions, item);
        }
    }
    activation.journal.length = 0;
    state.extensions.set(extensionId, {
        module: activated.mod,
        context,
        activation,
        manifest: params.manifest && typeof params.manifest === 'object'
            ? params.manifest : {},
        generation,
    });
    return {
        activated: true,
        extensionId,
        generation,
        exports: activated.activationResult === undefined
            ? null : activated.activationResult,
        subscriptionCount: context.subscriptions.length,
    };
});

state.handlers.set('host.deactivate', async (params) => {
    const extensionId = String(params.extensionId || '');
    const entry = state.extensions.get(extensionId);
    if (!entry) { return { deactivated: false, reason: 'not-activated' }; }
    const generation = params.generation === undefined
        ? entry.generation : Number(params.generation);
    if (!Number.isSafeInteger(generation) || generation !== entry.generation) {
        throw new Error(`stale extension generation: ${extensionId}`);
    }
    const cleanupErrors = [];
    const cleanupDeadline = Date.now() + kDisposableTimeoutMs;
    if (entry.activation) entry.activation.phase = 'deactivating';
    if (entry.module && typeof entry.module.deactivate === 'function') {
        try {
            await runWithDeadline(() => runWithActivation(entry.activation,
                () => entry.module.deactivate()), cleanupDeadline,
                'extension deactivate callback timed out');
        } catch (error) {
            cleanupErrors.push(reportDisposalFailure(error));
        }
    }
    const disposed = new Set();
    for (const sub of entry.context.subscriptions.slice().reverse()) {
        if (!sub || typeof sub.dispose !== 'function' || disposed.has(sub)) {
            continue;
        }
        disposed.add(sub);
        cleanupErrors.push(...await settleDisposable(sub, cleanupDeadline));
    }
    if (entry.activation) entry.activation.phase = 'deactivated';
    state.extensions.delete(extensionId);
    return {
        deactivated: true,
        extensionId,
        generation,
        cleanupComplete: cleanupErrors.length === 0,
        cleanupErrors: cleanupErrors.slice(0, 16),
    };
});

state.handlers.set('host.shutdown', async () => {
    for (const id of Array.from(state.extensions.keys())) {
        try { await state.handlers.get('host.deactivate')({ extensionId: id }); }
        catch (e) { /* noop */ }
    }
    return { shutdown: true };
});

state.handlers.set('commands.execute', async (params) => {
    const commandId = String(params.command || '');
    const args = Array.isArray(params.arguments) ? params.arguments : [];
    const registration = state.commandRegistrations.get(commandId);
    if (!registration || !registration.active ||
        typeof registration.handler !== 'function') {
        throw new Error(`unknown command: ${commandId}`);
    }
    return await Promise.resolve(runWithActivation(
        registration.activation, () => registration.handler(...args)));
});

state.handlers.set('commands.list', async () => {
    const items = [];
    for (const [command, registration] of state.commandRegistrations) {
        if (!registration || !registration.active) continue;
        items.push({
            command: String(command),
            extensionId: String(registration.extensionId || ''),
            generation: Number(registration.generation) || 0,
            editorRequired: registration.editorRequired === true,
        });
    }
    items.sort((left, right) => left.command.localeCompare(right.command));
    return { ok: true, available: true, applied: false, items };
});

state.handlers.set('tree.inventory', treeInventoryRequest);
state.handlers.set('tree.children', treeChildrenRequest);
state.handlers.set('tree.expand', treeExpandRequest);
state.handlers.set('tree.checkbox', treeCheckboxRequest);
state.handlers.set('tree.select', treeSelectRequest);
state.handlers.set('tree.drop', treeDropRequest);
state.handlers.set('tree.action', treeActionRequest);
state.handlers.set('tree.visibility', treeVisibilityRequest);

state.handlers.set('webview.resolveView', async (params) => {
    // Ask a previously registered WebviewViewProvider to populate its view.
    // Returns whatever html the provider set on webview.html so the host can
    // hand it off to WebView2 (or just log it while the UI wiring is stubbed).
    const viewId = String(params && params.viewId || '');
    const record = state.webviewViewProviders.get(viewId);
    if (!record || record.active === false) {
        throw new Error(`unknown webviewViewId: ${viewId}`);
    }
    let entry = state.webviewViews.get(viewId);
    if (!entry) {
        const owner = {
            viewId,
            extensionId: record.extensionId,
            generation: record.generation,
        };
        const { webview, messageEmitter } = makeMockWebview(owner);
        const disposeEmitter = new EventEmitter();
        const visibilityEmitter = new EventEmitter();
        const view = {
            webview,
            visible: true,
            onDidDispose: disposeEmitter.event,
            onDidChangeVisibility: visibilityEmitter.event,
        };
        entry = {
            view, webview, messageEmitter, disposeEmitter, visibilityEmitter,
            extensionId: record.extensionId,
            generation: record.generation,
            activation: record.activation,
        };
        state.webviewViews.set(viewId, entry);
    }
    const context = (params && params.webviewViewContext) || {};
    const tokenSource = neverCancellationToken;
    await Promise.resolve(runWithActivation(record.activation,
        () => record.provider.resolveWebviewView(entry.view, context, tokenSource)));
    const current = state.webviewViewProviders.get(viewId);
    const extension = state.extensions.get(record.extensionId);
    if (current !== record || !record.active || !extension ||
        extension.generation !== record.generation) {
        throw new Error(`stale webviewView generation: ${viewId}`);
    }
    const optionsSnapshot = boundedJsonSnapshot(
        entry.webview.options || {}, kMaximumWebviewInventoryBytes);
    if (!optionsSnapshot.ok || !optionsSnapshot.value ||
        typeof optionsSnapshot.value !== 'object' ||
        Array.isArray(optionsSnapshot.value)) {
        throw new Error(`webviewView options exceed the inventory budget: ${viewId}`);
    }
    entry.webview.options = optionsSnapshot.value;
    const htmlSnapshot = boundedJsonSnapshot(
        typeof entry.webview.html === 'string' ? entry.webview.html : '',
        kMaximumWebviewMessageBytes);
    const html = htmlSnapshot.ok && typeof htmlSnapshot.value === 'string'
        ? htmlSnapshot.value : '';
    if (!htmlSnapshot.ok ||
        Buffer.byteLength(html, 'utf8') > kMaximumExtensionMainBytes) {
        throw new Error(`webviewView HTML exceeds the inventory budget: ${viewId}`);
    }
    return {
        ok: true,
        viewId,
        extensionId: record.extensionId,
        generation: record.generation,
        html,
        options: optionsSnapshot.value,
    };
});

state.handlers.set('webview.postToView', async (params) => {
    // Deliver a message from the SAO side back into the mock webview so the
    // extension's onDidReceiveMessage listener fires.
    const message = params && params.message;
    const panelId = params && params.panelId;
    const viewId = params && params.viewId;
    const messageSnapshot = boundedJsonSnapshot(message);
    if (!messageSnapshot.ok) {
        return {
            ok: false, available: false, applied: false,
            acknowledged: false,
            errorCode: 'WEBVIEW_MESSAGE_LIMIT_EXCEEDED',
            error: 'WebView message exceeds the structural budget',
        };
    }
    const route = panelId ? { key: 'panelId', id: String(panelId),
        record: state.webviewPanels.get(String(panelId)) }
        : viewId ? { key: 'viewId', id: String(viewId),
            record: state.webviewViews.get(String(viewId)) }
            : null;
    if (route) {
        const rec = route.record;
        if (!rec) { throw new Error(`unknown ${route.key}: ${route.id}`); }
        const extension = state.extensions.get(rec.extensionId);
        const current = route.key === 'panelId'
            ? state.webviewPanels.get(route.id) === rec
            : state.webviewViews.get(route.id) === rec &&
              state.webviewViewProviders.get(route.id) &&
              state.webviewViewProviders.get(route.id).active;
        if (!current || !extension ||
            extension.generation !== rec.generation) {
            return {
                ok: false, available: false, applied: false,
                acknowledged: false, [route.key]: route.id,
                extensionId: rec.extensionId, generation: rec.generation,
                errorCode: 'WEBVIEW_MESSAGE_STALE',
                error: 'WebView message target generation is stale',
            };
        }
        if (!rec.messageEmitter.hasListeners) {
            return {
                ok: false, available: true, applied: false,
                acknowledged: false, [route.key]: route.id,
                extensionId: rec.extensionId, generation: rec.generation,
                errorCode: 'WEBVIEW_MESSAGE_LISTENER_UNAVAILABLE',
                error: 'WebView message target has no listener',
            };
        }
        try {
            await invokeTreeCallback(
                rec.messageEmitter.fireAsync.bind(rec.messageEmitter),
                [messageSnapshot.value], 'WEBVIEW_MESSAGE_CALLBACK_FAILED',
                rec.activation);
        } catch (error) {
            if (error && error.treeCode === 'TREE_CALLBACK_TIMEOUT') {
                throw transportError(
                    'WebView message listener did not settle', -207,
                    { callbackUnresolved: true });
            }
            throw error;
        }
        return {
            ok: true, available: true, applied: true,
            acknowledged: true, [route.key]: route.id,
            extensionId: rec.extensionId, generation: rec.generation,
            errorCode: '', error: '',
        };
    }
    throw new Error('webview.postToView requires panelId or viewId');
});

state.handlers.set('webview.disposePanel', async (params) => {
    const panelId = String(params && params.panelId || '');
    const rec = state.webviewPanels.get(panelId);
    if (!rec) { return { disposed: false, reason: 'unknown-panel' }; }
    rec.panel.dispose();
    return { disposed: true, panelId };
});

state.handlers.set('webview.listProviders', async () => {
    const items = [];
    let inventoryBytes = 2;
    for (const [viewId, registration] of state.webviewViewProviders) {
        if (!registration || registration.active === false) continue;
        const runtime = state.webviewViews.get(viewId);
        const html = runtime && typeof runtime.webview.html === 'string'
            ? runtime.webview.html : '';
        const htmlBytes = Buffer.byteLength(html, 'utf8');
        const optionsSnapshot = boundedJsonSnapshot(
            runtime && runtime.webview.options || {},
            kMaximumWebviewInventoryBytes);
        const optionsValid = optionsSnapshot.ok && optionsSnapshot.value &&
            typeof optionsSnapshot.value === 'object' &&
            !Array.isArray(optionsSnapshot.value);
        let htmlAvailable = htmlBytes > 0 &&
            htmlBytes <= kMaximumExtensionMainBytes && optionsValid;
        let item = {
            viewId: String(viewId),
            extensionId: String(registration.extensionId || ''),
            generation: Number(registration.generation) || 0,
            available: true,
            html: htmlAvailable ? html : '',
            htmlAvailable,
            htmlLength: htmlAvailable ? htmlBytes : 0,
            options: optionsValid ? optionsSnapshot.value : {},
            errorCode: htmlBytes > kMaximumExtensionMainBytes
                ? 'WEBVIEW_HTML_TOO_LARGE'
                : !optionsValid ? 'WEBVIEW_OPTIONS_INVALID' : '',
            error: htmlBytes > kMaximumExtensionMainBytes
                ? 'WebviewView HTML exceeds the 1 MiB inventory limit'
                : !optionsValid
                    ? 'WebviewView options exceed the inventory limit' : '',
        };
        let itemSnapshot = boundedJsonSnapshot(
            item, kMaximumWebviewInventoryBytes);
        if (!itemSnapshot.ok ||
            inventoryBytes + itemSnapshot.bytes + (items.length ? 1 : 0) >
                kMaximumWebviewInventoryBytes) {
            item = Object.assign({}, item, {
                html: '', htmlAvailable: false, htmlLength: 0, options: {},
                errorCode: 'WEBVIEW_INVENTORY_LIMIT_EXCEEDED',
                error: 'WebviewView inventory exceeds the 1 MiB limit',
            });
            itemSnapshot = boundedJsonSnapshot(
                item, kMaximumWebviewInventoryBytes);
        }
        if (!itemSnapshot.ok ||
            inventoryBytes + itemSnapshot.bytes + (items.length ? 1 : 0) >
                kMaximumWebviewInventoryBytes) {
            break;
        }
        inventoryBytes += itemSnapshot.bytes + (items.length ? 1 : 0);
        items.push(itemSnapshot.value);
    }
    items.sort((left, right) => left.viewId.localeCompare(right.viewId));
    return {
        viewIds: Array.from(state.webviewViewProviders.keys()),
        panelIds: Array.from(state.webviewPanels.keys()),
        items,
    };
});

function makeMemento() {
    const map = new Map();
    return {
        keys() { return Array.from(map.keys()); },
        get(key, def) { return map.has(key) ? map.get(key) : def; },
        update(key, value) { if (value === undefined) map.delete(key); else map.set(key, value); return Promise.resolve(); },
    };
}

async function dispatchInbound(message) {
    if (message === null || typeof message !== 'object' ||
        Array.isArray(message)) {
        throw transportError('extension host JSON-RPC message must be an object',
            -207);
    }
    if (message.method !== undefined && message.id !== undefined) {
        const handler = state.handlers.get(message.method);
        const reply = { jsonrpc: '2.0', id: message.id };
        if (!handler) {
            reply.error = { code: -32601, message: `method not found: ${message.method}` };
        } else {
            try {
                const value = await Promise.resolve(handler(message.params || {}));
                reply.result = value === undefined ? null : value;
            } catch (err) {
                const errorData = err && err.data && typeof err.data === 'object'
                    ? Object.assign({}, err.data) : {};
                if (err && Number.isInteger(err.status)) {
                    errorData.status = err.status;
                } else {
                    errorData.status = -5;
                }
                errorData.stack = err && err.stack ? err.stack : null;
                reply.error = {
                    code: Number.isInteger(err && err.code) ? err.code : -32000,
                    message: err && err.message ? err.message : String(err),
                    data: errorData,
                };
            }
        }
        if (!send(reply)) {
            closeTransport('extension host could not send the response');
        }
        return;
    }
    if (message.method !== undefined) {
        const handler = state.handlers.get(message.method);
        if (!handler) {
            process_.stderr.write(`[shim] unhandled notification: ${message.method}\n`);
            return;
        }
        try {
            await Promise.resolve(handler(message.params || {}));
        } catch (err) {
            process_.stderr.write(`[shim] notification ${message.method} failed: ${err.stack || err.message}\n`);
        }
        return;
    }
    if (message.id !== undefined) {
        const pending = state.pending.get(message.id);
        if (pending) {
            state.pending.delete(message.id);
            if (message.error) {
                const data = message.error.data && typeof message.error.data === 'object'
                    ? message.error.data : {};
                const error = transportError(
                    message.error.message || 'call failed',
                    Number.isInteger(data.status) ? data.status : -207,
                    data);
                error.code = message.error.code;
                pending.reject(error);
            }
            else { pending.resolve(message.result); }
        }
    }
}

// ----- Content-Length framed stdin parser --------------------------------

let buffer = Buffer.alloc(0);

stdin.on('data', (chunk) => {
    if (state.transportClosed) return;
    if (buffer.length + chunk.length > kMaximumFrameBytes + 64 * 1024) {
        closeTransport('extension host stdin frame exceeds the maximum size',
            -207);
        return;
    }
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
        const separator = buffer.indexOf('\r\n\r\n');
        if (separator < 0) { return; }
        const header = buffer.slice(0, separator).toString('utf8');
        const match = /Content-Length:\s*(\d+)/i.exec(header);
        if (!match) {
            closeTransport('extension host stdin frame is missing Content-Length',
                -207);
            return;
        }
        const size = parseInt(match[1], 10);
        if (!Number.isSafeInteger(size) || size < 0 ||
            size > kMaximumFrameBytes) {
            closeTransport('extension host stdin frame length is invalid', -207);
            return;
        }
        if (buffer.length - separator - 4 < size) { return; }
        const body = buffer.slice(separator + 4, separator + 4 + size).toString('utf8');
        buffer = buffer.slice(separator + 4 + size);
        try {
            const parsed = JSON.parse(body);
            dispatchInbound(parsed).catch((err) => {
                process_.stderr.write(`[shim] dispatch error: ${err.stack || err.message}\n`);
                closeTransport(err.message || 'extension host dispatch failed',
                    Number.isInteger(err.status) ? err.status : -207,
                    err.data);
            });
        } catch (err) {
            process_.stderr.write(`[shim] parse error: ${err.message}\n`);
            closeTransport('extension host stdin JSON is invalid', -207);
            return;
        }
    }
});

stdin.on('end', () => {
    closeTransport('extension host stdin ended');
    process_.exit(0);
});
stdin.on('close', () => {
    closeTransport('extension host stdin closed');
    process_.exit(0);
});

// Boot: wait for the parent's host.initialize call.
