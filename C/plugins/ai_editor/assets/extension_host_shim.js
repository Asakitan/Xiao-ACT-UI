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
const { pathToFileURL, fileURLToPath } = require('url');
const process_ = require('process');
const { AsyncLocalStorage } = require('async_hooks');
const installExecutionApis = require('./extension_execution_shim.js');

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
    terminals: new Map(),
    terminalAliases: new Map(),
    activeTerminalId: undefined,
    taskProviders: new Map(),
    taskExecutions: new Map(),
    debugProviders: new Map(),
    debugPreparations: new Map(),
    debugSessions: new Map(),
    activeDebugSessionId: undefined,
    breakpoints: new Map(),
    processRoutes: new Map(),
    earlyProcessEvents: new Map(),
    earlyProcessEventBytes: 0,
    nextSurfaceId: 1,
    surfacesInitialized: false,
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
const kMaximumSurfacePayloadBytes = 1024 * 1024;
const kMaximumSurfaceRegistrations = 512;
const kMaximumProcessEventBytes = 1024 * 1024;
const kMaximumEarlyProcessEventBytes = 16 * 1024 * 1024;
const kMaximumPendingHostRequests = 512;
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
    if (state.pending.size >= kMaximumPendingHostRequests) {
        return Promise.reject(transportError(
            `host request limit reached while calling ${method}`, -6));
    }
    if (!Number.isSafeInteger(state.nextId) || state.nextId <= 0 ||
        state.nextId >= Number.MAX_SAFE_INTEGER) {
        return Promise.reject(transportError('host request identifiers are exhausted', -6));
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
    if (scope && scope.hostOwned === true && scope.phase === 'active') {
        return scope;
    }
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
        return Uri.parse(pathToFileURL(path.resolve(String(fsPath))).href);
    },
    parse(value) {
        const raw = String(value || '');
        const parsed = new URL(raw);
        const scheme = parsed.protocol.slice(0, -1);
        const pathOnly = decodeURIComponent(parsed.pathname);
        return {
            scheme,
            authority: parsed.host,
            path: pathOnly,
            fsPath: scheme === 'file' ? fileURLToPath(parsed) : pathOnly,
            query: parsed.search.slice(1),
            fragment: parsed.hash.slice(1),
            toString() { return parsed.href; },
            toJSON() { return parsed.href; },
        };
    },
};

const EventEmitter = (function () {
    return class {
        constructor() { this._listeners = new Map(); }
        get event() {
            return (listener, thisArgs, disposables) => {
                if (typeof listener !== 'function') throw new TypeError('Expected event listener');
                const callback = value => listener.call(thisArgs, value);
                this._listeners.set(callback, activationStorage.getStore());
                const subscription = disposable(() => this._listeners.delete(callback));
                if (disposables) disposables.push(subscription);
                return subscription;
            };
        }
        fire(value) {
            for (const [listener, scope] of Array.from(this._listeners)) {
                try {
                    Promise.resolve(runWithActivation(scope, () => listener(value)))
                        .catch(() => undefined);
                }
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

function activationScopeCurrent(scope) {
    if (scope && scope.hostOwned === true && scope.phase === 'active') {
        return true;
    }
    if (!scope || !scope.extensionId ||
        !Number.isSafeInteger(scope.generation) || scope.generation <= 0 ||
        (scope.phase !== 'active' && scope.phase !== 'committed')) {
        return false;
    }
    if (scope.phase === 'active') return true;
    const extension = state.extensions.get(scope.extensionId);
    return Boolean(extension && extension.generation === scope.generation &&
        extension.activation === scope);
}

function requireCurrentGeneration(scope, operation) {
    if (!activationScopeCurrent(scope)) {
        throw new Error(`${operation || 'extension callback'} belongs to a retired generation`);
    }
}

function allocateSurfaceId(prefix, scope) {
    if (!Number.isSafeInteger(state.nextSurfaceId) ||
        state.nextSurfaceId <= 0 || state.nextSurfaceId >= Number.MAX_SAFE_INTEGER) {
        throw new Error('Extension surface identifiers are exhausted');
    }
    const owner = scope && scope.extensionId
        ? String(scope.extensionId).replace(/[^A-Za-z0-9_.-]/g, '_') : 'host';
    return `${prefix}-${owner}-${scope && scope.generation || 0}-${state.nextSurfaceId++}`;
}

function surfaceOwner(scope, prefix) {
    return {
        extensionId: scope.extensionId,
        generation: scope.generation,
        registrationId: allocateSurfaceId(prefix, scope),
    };
}

function boundedSurfaceSnapshot(value, label, maximumBytes =
    kMaximumSurfacePayloadBytes) {
    const snapshot = boundedJsonSnapshot(value, maximumBytes);
    if (!snapshot.ok) {
        throw new Error(`${label || 'Extension API payload'} exceeds the transport budget`);
    }
    return snapshot.value;
}

function reportAsyncFailure(prefix, error) {
    const message = error && error.message ? error.message : String(error);
    process_.stderr.write(`[shim] ${prefix}: ${message}\n`);
}

function callHostObserved(method, params, prefix) {
    return callHost(method, params).catch(error => {
        reportAsyncFailure(prefix || `${method} failed`, error);
        throw error;
    });
}

function eventDataBuffer(payload, maximumBytes = kMaximumProcessEventBytes) {
    const data = payload && payload.data;
    let buffer;
    if (Buffer.isBuffer(data)) buffer = data;
    else if (data instanceof Uint8Array) buffer = Buffer.from(data);
    else if (Array.isArray(data) && data.every(byte =>
        Number.isInteger(byte) && byte >= 0 && byte <= 255)) {
        buffer = Buffer.from(data);
    } else if (typeof data === 'string') {
        buffer = Buffer.from(data, payload.encoding === 'base64' ? 'base64' : 'utf8');
    } else if (data === undefined || data === null) buffer = Buffer.alloc(0);
    else buffer = Buffer.from(JSON.stringify(data), 'utf8');
    if (buffer.length > maximumBytes) {
        throw new Error('Process event data exceeds the transport budget');
    }
    return buffer;
}

function processEventId(payload) {
    const snapshot = payload && payload.snapshot &&
        typeof payload.snapshot === 'object' ? payload.snapshot : {};
    return String(payload && (payload.id || payload.processId ||
        payload.terminalId || payload.executionId || payload.transportId ||
        payload.sessionId) || snapshot.id || snapshot.processId ||
        snapshot.terminalId || snapshot.executionId || snapshot.transportId ||
        snapshot.sessionId || '');
}

function registerProcessRoute(id, route) {
    const key = String(id || '');
    if (!key || typeof route !== 'function') {
        throw new Error('A process route requires an id and callback');
    }
    state.processRoutes.set(key, route);
    const queued = state.earlyProcessEvents.get(key);
    state.earlyProcessEvents.delete(key);
    if (queued) {
        for (const event of queued) {
            state.earlyProcessEventBytes = Math.max(0,
                state.earlyProcessEventBytes - event.bytes);
            route(event.payload);
        }
    }
    return () => {
        if (state.processRoutes.get(key) === route) state.processRoutes.delete(key);
    };
}

function queueEarlyProcessEvent(payload) {
    const id = processEventId(payload);
    if (!id) return;
    let bytes;
    try { bytes = Buffer.byteLength(JSON.stringify(payload), 'utf8'); }
    catch (_) { return; }
    if (bytes > kMaximumFrameBytes || bytes > kMaximumEarlyProcessEventBytes) return;
    const evictOldest = excludedId => {
        let oldest;
        for (const candidate of state.earlyProcessEvents.keys()) {
            if (candidate !== excludedId) {
                oldest = candidate;
                break;
            }
        }
        if (oldest === undefined) return false;
        const removed = state.earlyProcessEvents.get(oldest) || [];
        state.earlyProcessEvents.delete(oldest);
        for (const event of removed) {
            state.earlyProcessEventBytes = Math.max(0,
                state.earlyProcessEventBytes - event.bytes);
        }
        return true;
    };
    if (!state.earlyProcessEvents.has(id) &&
        state.earlyProcessEvents.size >= 128) {
        evictOldest(id);
    }
    const events = state.earlyProcessEvents.get(id) || [];
    if (events.length >= 32) return;
    while (state.earlyProcessEventBytes + bytes >
           kMaximumEarlyProcessEventBytes) {
        if (!evictOldest(id)) return;
    }
    events.push({ payload, bytes });
    state.earlyProcessEventBytes += bytes;
    state.earlyProcessEvents.set(id, events);
}

class Position {
    constructor(line, character) {
        if (!Number.isInteger(line) || line < 0 || !Number.isInteger(character) || character < 0)
            throw new TypeError('Invalid position');
        this.line = line;
        this.character = character;
        Object.freeze(this);
    }
    compareTo(other) { return this.line - other.line || this.character - other.character; }
    isBefore(other) { return this.compareTo(other) < 0; }
    isBeforeOrEqual(other) { return this.compareTo(other) <= 0; }
    isAfter(other) { return this.compareTo(other) > 0; }
    isAfterOrEqual(other) { return this.compareTo(other) >= 0; }
    isEqual(other) { return this.compareTo(other) === 0; }
    with(line = this.line, character = this.character) {
        if (typeof line === 'object') return new Position(line.line ?? this.line, line.character ?? this.character);
        return new Position(line, character);
    }
    translate(line = 0, character = 0) {
        if (typeof line === 'object') return this.translate(line.lineDelta || 0, line.characterDelta || 0);
        return new Position(this.line + line, this.character + character);
    }
}
const asPosition = value => new Position(value.line, value.character);
class Range {
    constructor(start, end, endLine, endCharacter) {
        if (typeof start === 'number') {
            start = new Position(start, end);
            end = new Position(endLine, endCharacter);
        } else { start = asPosition(start); end = asPosition(end); }
        this.start = start.isBeforeOrEqual(end) ? start : end;
        this.end = start.isBeforeOrEqual(end) ? end : start;
    }
    get isEmpty() { return this.start.isEqual(this.end); }
    get isSingleLine() { return this.start.line === this.end.line; }
    contains(value) {
        return value.start ? this.contains(value.start) && this.contains(value.end)
            : this.start.isBeforeOrEqual(value) && this.end.isAfterOrEqual(value);
    }
    isEqual(value) { return this.start.isEqual(value.start) && this.end.isEqual(value.end); }
    with(start = this.start, end = this.end) {
        if (start.start || start.end) return new Range(start.start || this.start, start.end || this.end);
        return new Range(start, end);
    }
    intersection(other) {
        const start = this.start.isAfter(other.start) ? this.start : other.start;
        const end = this.end.isBefore(other.end) ? this.end : other.end;
        return start.isAfter(end) ? undefined : new Range(start, end);
    }
    union(other) {
        return new Range(this.start.isBefore(other.start) ? this.start : other.start,
            this.end.isAfter(other.end) ? this.end : other.end);
    }
}
class TextEdit {
    constructor(range, newText) { this.range = range; this.newText = newText; }
    static replace(range, text) { return new TextEdit(range, text); }
    static insert(position, text) { return new TextEdit(new Range(position, position), text); }
    static delete(range) { return new TextEdit(range, ''); }
}
class WorkspaceEdit {
    constructor() { this._entries = new Map(); }
    set(uri, edits) { this._entries.set(uri.toString(), [uri, edits.slice()]); }
    get(uri) { return this._entries.get(uri.toString())?.[1].slice() || []; }
    has(uri) { return this._entries.has(uri.toString()); }
    get size() { return this._entries.size; }
    entries() { return Array.from(this._entries.values(), ([uri, edits]) => [uri, edits.slice()]); }
    replace(uri, range, text) { this.set(uri, [...this.get(uri), TextEdit.replace(range, text)]); }
    insert(uri, position, text) { this.replace(uri, new Range(position, position), text); }
    delete(uri, range) { this.replace(uri, range, ''); }
}

const documents = new Map();
const providerDocuments = new Map();
let workspaceInitialized = false;
const documentEvents = Object.fromEntries(['open', 'close', 'change', 'save', 'config', 'folders']
    .map(name => [name, new EventEmitter()]));

function updateDocument(snapshot, tracked = true) {
    const uri = Uri.parse(snapshot.uri);
    const key = uri.toString();
    const store = tracked ? documents : providerDocuments;
    let document = store.get(key);
    const version = tracked ? snapshot.version : (document?.version || 0) +
        (!document || snapshot.content !== document.getText() || snapshot.languageId !== document.languageId ? 1 : 0);
    snapshot = { ...snapshot, dirty: snapshot.dirty ?? snapshot.isDirty ?? false,
        untitled: snapshot.untitled ?? uri.scheme === 'untitled',
        version: Number.isSafeInteger(version) ? version : document?.version || 1 };
    if (!document || document.isClosed) {
        document = {
            uri, _snapshot: snapshot, _lines: [], _offsets: [],
            get fileName() { return this._snapshot.fsPath || this.uri.fsPath; },
            get languageId() { return this._snapshot.languageId || guessLanguage(this.fileName); },
            get version() { return this._snapshot.version; },
            get isDirty() { return this._snapshot.dirty === true; },
            get isUntitled() { return this._snapshot.untitled === true; },
            get isClosed() { return this._snapshot.isClosed === true; },
            get eol() { return this._snapshot.content.includes('\r\n') ? 2 : 1; },
            get lineCount() { return this._lines.length; },
            getText(range) {
                return range ? this._snapshot.content.slice(this.offsetAt(range.start), this.offsetAt(range.end))
                    : this._snapshot.content;
            },
            validatePosition(position) {
                const line = Math.max(0, Math.min(position.line, this.lineCount - 1));
                return new Position(line, position.line >= this.lineCount ? this._lines[line].length
                    : Math.max(0, Math.min(position.character, this._lines[line].length)));
            },
            validateRange(range) { return new Range(this.validatePosition(range.start), this.validatePosition(range.end)); },
            offsetAt(position) {
                const p = this.validatePosition(position);
                return this._offsets[p.line] + p.character;
            },
            positionAt(offset) {
                offset = Math.max(0, Math.min(Math.floor(offset), this._snapshot.content.length));
                let low = 0, high = this._offsets.length;
                while (low + 1 < high) {
                    const mid = (low + high) >>> 1;
                    if (this._offsets[mid] > offset) high = mid; else low = mid;
                }
                return new Position(low, Math.min(offset - this._offsets[low], this._lines[low].length));
            },
            lineAt(value) {
                const line = typeof value === 'number' ? value : this.validatePosition(value).line;
                if (!Number.isInteger(line) || line < 0 || line >= this.lineCount) throw new RangeError('Invalid line');
                const text = this._lines[line];
                const first = text.search(/\S/);
                return Object.freeze({ lineNumber: line, text, range: new Range(line, 0, line, text.length),
                    rangeIncludingLineBreak: line + 1 < this.lineCount ? new Range(line, 0, line + 1, 0)
                        : new Range(line, 0, line, text.length),
                    firstNonWhitespaceCharacterIndex: first < 0 ? text.length : first,
                    isEmptyOrWhitespace: first < 0 });
            },
            getWordRangeAtPosition(position, regex = /[\p{L}\p{N}_]+/u) {
                const p = this.validatePosition(position);
                const matcher = new RegExp(regex.source, regex.flags.replace(/[gy]/g, '') + 'g');
                for (const match of this._lines[p.line].matchAll(matcher)) {
                    if (!match[0].length) return undefined;
                    if (match.index <= p.character && p.character <= match.index + match[0].length)
                        return new Range(p.line, match.index, p.line, match.index + match[0].length);
                }
                return undefined;
            },
            async save() {
                if (this.isClosed || this.isUntitled) return false;
                try {
                    const result = await callHost('vscode.workspace.saveTextDocument',
                        { uri: key, version: this.version, expectedContent: this.getText() });
                    if (result.document) updateDocument(result.document);
                    return result.ok === true;
                } catch (_) { return false; }
            },
        };
        if (!tracked && store.size >= 512) store.delete(store.keys().next().value);
        store.set(key, document);
    }
    if (snapshot.version < document.version) return document;
    document._snapshot = Object.assign({}, snapshot);
    const text = snapshot.content || '';
    document._snapshot.content = text;
    document._lines = text.split(/\r\n|\r|\n/);
    document._offsets = [0];
    for (const match of text.matchAll(/\r\n|\r|\n/g)) document._offsets.push(match.index + match[0].length);
    return document;
}

const workspace = {
    workspaceFolders: [],
    get textDocuments() { return Array.from(documents.values()).filter(doc => !doc.isClosed); },
    async openTextDocument(uri) {
        const params = typeof uri === 'string' ? { path: uri }
            : uri && uri.scheme ? { uri: uri.toString() }
            : { untitled: true, content: uri?.content || '', languageId: uri?.language || 'plaintext' };
        return updateDocument(await callHost('vscode.workspace.openTextDocument', params));
    },
    async applyEdit(edit) {
        if (!(edit instanceof WorkspaceEdit)) throw new TypeError('Expected WorkspaceEdit');
        const documentEdits = [];
        for (const [uri, edits] of edit.entries()) {
            const document = await this.openTextDocument(uri);
            documentEdits.push({ uri: document.uri.toString(), version: document.version, edits });
        }
        try {
            const result = await callHost('vscode.workspace.applyEdit', { documentEdits });
            for (const document of result.documents || []) updateDocument(document);
            return result.applied === true;
        } catch (_) { return false; }
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
    onDidChangeConfiguration: documentEvents.config.event,
    onDidChangeWorkspaceFolders: documentEvents.folders.event,
    onDidOpenTextDocument: documentEvents.open.event,
    onDidCloseTextDocument: documentEvents.close.event,
    onDidChangeTextDocument: documentEvents.change.event,
    onDidSaveTextDocument: documentEvents.save.event,
    fs: {
        async readFile(uri) {
            const p = typeof uri === 'string' ? uri : (uri && uri.fsPath) || '';
            const result = await callHost('vscode.workspace.fs.readFile', { path: p });
            if (!Array.isArray(result) || result.length > 1024 * 1024 ||
                result.some(byte => !Number.isInteger(byte) || byte < 0 || byte > 255))
                throw new Error('Invalid file byte response');
            return Uint8Array.from(result);
        },
        async writeFile(uri, content) {
            const p = typeof uri === 'string' ? uri : (uri && uri.fsPath) || '';
            if (!(content instanceof Uint8Array)) throw new TypeError('Expected Uint8Array');
            if (content.byteLength > 1024 * 1024) throw new Error('File exceeds the transport budget');
            await callHost('vscode.workspace.fs.writeFile', { path: p, content: Array.from(content) });
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

// ----- terminals ----------------------------------------------------------

const terminalEvents = {
    open: new EventEmitter(),
    close: new EventEmitter(),
    active: new EventEmitter(),
    data: new EventEmitter(),
    dimensions: new EventEmitter(),
    state: new EventEmitter(),
};

function terminalUri(value) {
    if (!value) return undefined;
    if (typeof value === 'string') return value;
    if (typeof value.toString === 'function') return value.toString();
    return String(value.fsPath || value.path || '');
}

function terminalIcon(value) {
    if (!value) return undefined;
    if (typeof value === 'string') return value;
    if (value.id) {
        return { id: String(value.id), color: value.color && value.color.id
            ? String(value.color.id) : undefined };
    }
    if (value.light || value.dark) {
        return { light: terminalUri(value.light), dark: terminalUri(value.dark) };
    }
    return terminalUri(value);
}

function terminalDimensions(value) {
    if (!value || typeof value !== 'object') return undefined;
    const columns = Number(value.columns);
    const rows = Number(value.rows);
    if (!Number.isSafeInteger(columns) || columns <= 0 || columns > 10000 ||
        !Number.isSafeInteger(rows) || rows <= 0 || rows > 10000) {
        return undefined;
    }
    return Object.freeze({ columns, rows });
}

function normalizeTerminalCreation(first, shellPath, shellArgs) {
    let source;
    if (first && typeof first === 'object' && !Array.isArray(first)) {
        source = first;
    } else {
        source = { name: first, shellPath, shellArgs };
    }
    const pty = source.pty;
    if (pty !== undefined && (!pty || typeof pty.open !== 'function' ||
        typeof pty.close !== 'function')) {
        throw new TypeError('ExtensionTerminalOptions.pty requires open() and close()');
    }
    const name = String(source.name || (pty ? 'Extension Terminal' : 'Terminal'));
    if (!name || name.includes('\0') || Buffer.byteLength(name, 'utf8') > 256) {
        throw new TypeError('Terminal name is invalid');
    }
    const options = { name };
    if (!pty) {
        if (source.shellPath !== undefined) options.shellPath = String(source.shellPath);
        if (source.shellArgs !== undefined) {
            if (typeof source.shellArgs === 'string') options.shellArgs = source.shellArgs;
            else if (Array.isArray(source.shellArgs)) {
                options.shellArgs = source.shellArgs.map(value => String(value));
            } else throw new TypeError('Terminal shellArgs must be a string or array');
        }
        if (source.cwd !== undefined) options.cwd = terminalUri(source.cwd);
        if (source.env !== undefined) options.env = source.env;
        if (source.strictEnv !== undefined) options.strictEnv = source.strictEnv === true;
        if (source.message !== undefined) options.message = String(source.message);
        if (source.hideFromUser !== undefined) options.hideFromUser = source.hideFromUser === true;
    }
    if (source.iconPath !== undefined) options.iconPath = terminalIcon(source.iconPath);
    if (source.color !== undefined) {
        options.color = source.color && source.color.id
            ? { id: String(source.color.id) } : String(source.color);
    }
    if (source.location !== undefined) options.location = source.location;
    if (source.isTransient !== undefined) options.isTransient = source.isTransient === true;
    return {
        pty,
        options: boundedSurfaceSnapshot(options, 'Terminal options'),
        creationOptions: source,
    };
}

function terminalRecordById(value) {
    const id = String(value || '');
    return state.terminals.get(id) ||
        state.terminals.get(state.terminalAliases.get(id));
}

function setActiveTerminal(record) {
    const previous = terminalRecordById(state.activeTerminalId);
    const nextId = record && !record.closed ? record.id : undefined;
    if (state.activeTerminalId === nextId) return;
    state.activeTerminalId = nextId;
    if (previous && previous !== record) previous.active = false;
    if (record) record.active = true;
    terminalEvents.active.fire(record ? record.terminal : undefined);
}

function callPseudoterminal(record, method, ...args) {
    if (!record.pty || typeof record.pty[method] !== 'function') return;
    try {
        return runWithActivation(record.scope,
            () => record.pty[method](...args));
    } catch (error) {
        reportAsyncFailure(`pseudoterminal ${method} failed`, error);
        return undefined;
    }
}

function bindPseudoterminal(record) {
    const pty = record.pty;
    if (!pty) return;
    const subscribe = (event, listener) => {
        if (typeof event !== 'function') return;
        try {
            const value = event(listener);
            if (value && typeof value.dispose === 'function') {
                record.ptySubscriptions.push(value);
            }
        } catch (error) {
            reportAsyncFailure('pseudoterminal event subscription failed', error);
        }
    };
    subscribe(pty.onDidWrite, data => {
        if (record.closed) return;
        const text = String(data === undefined ? '' : data);
        if (Buffer.byteLength(text, 'utf8') > kMaximumProcessEventBytes) {
            reportAsyncFailure('pseudoterminal output rejected',
                new Error('Pseudoterminal output exceeds the transport budget'));
            return;
        }
        callHostObserved('vscode.window.sendTerminalText', {
            ...record.owner,
            terminalId: record.hostId,
            id: record.hostId,
            text,
            addNewLine: false,
            stream: 'stdout',
            direction: 'output',
            fromPty: true,
        }, 'pseudoterminal output delivery failed').catch(() => undefined);
    });
    subscribe(pty.onDidClose, exitCode => {
        if (record.closed || record.disposing) return;
        record.requestedExitCode = Number.isInteger(exitCode) ? exitCode : undefined;
        record.terminal.dispose();
    });
    subscribe(pty.onDidChangeName, name => {
        const text = String(name || '');
        if (!text || Buffer.byteLength(text, 'utf8') > 256) return;
        record.name = text;
        callHostObserved('vscode.window.sendTerminalText', {
            ...record.owner, terminalId: record.hostId, id: record.hostId,
            op: 'name', name: text, fromPty: true,
        }, 'pseudoterminal name update failed').catch(() => undefined);
    });
    subscribe(pty.onDidOverrideDimensions, dimensions => {
        const value = terminalDimensions(dimensions);
        if (!value || record.closed) return;
        record.dimensions = value;
        terminalEvents.dimensions.fire({ terminal: record.terminal, dimensions: value });
    });
}

function markTerminalOpen(record, snapshot = {}) {
    if (record.closed) return;
    if (snapshot.name) record.name = String(snapshot.name);
    const dimensions = terminalDimensions(snapshot.dimensions || snapshot);
    if (dimensions) record.dimensions = dimensions;
    if (Number.isSafeInteger(snapshot.processId)) {
        record.processIdValue = snapshot.processId;
        record.resolveProcessId(snapshot.processId);
        record.processIdSettled = true;
    }
    if (!record.opened) {
        record.opened = true;
        if (!record.processIdSettled && record.pty) {
            record.resolveProcessId(undefined);
            record.processIdSettled = true;
        }
        callPseudoterminal(record, 'open', record.dimensions);
        terminalEvents.open.fire(record.terminal);
    }
    if (snapshot.active === true) setActiveTerminal(record);
    if (snapshot.state && typeof snapshot.state === 'object') {
        record.state = Object.assign({}, record.state, snapshot.state);
        terminalEvents.state.fire({ terminal: record.terminal, state: record.state });
    }
}

function finalizeTerminal(record, exitStatus) {
    if (!record || record.closed) return;
    record.closed = true;
    record.active = false;
    if (!record.processIdSettled) {
        record.resolveProcessId(undefined);
        record.processIdSettled = true;
    }
    if (exitStatus) record.exitStatus = exitStatus;
    for (const subscription of record.ptySubscriptions.splice(0)) {
        try { subscription.dispose(); } catch (_) { /* best effort */ }
    }
    callPseudoterminal(record, 'close');
    state.terminals.delete(record.id);
    for (const [alias, id] of Array.from(state.terminalAliases)) {
        if (id === record.id) state.terminalAliases.delete(alias);
    }
    if (state.activeTerminalId === record.id ||
        state.activeTerminalId === record.hostId) setActiveTerminal(undefined);
    terminalEvents.close.fire(record.terminal);
}

class Terminal {
    constructor(record) {
        this._record = record;
        this.processId = record.processId;
        this.creationOptions = record.creationOptions;
    }
    get id() { return this._record.id; }
    get name() { return this._record.name; }
    get exitStatus() { return this._record.exitStatus; }
    get state() { return Object.freeze(Object.assign({}, this._record.state)); }
    sendText(text, shouldExecute = true) {
        if (this._record.closed) return;
        const value = String(text === undefined ? '' : text);
        if (Buffer.byteLength(value, 'utf8') > kMaximumProcessEventBytes) {
            throw new Error('Terminal input exceeds the transport budget');
        }
        callHostObserved('vscode.window.sendTerminalText', {
            ...this._record.owner,
            terminalId: this._record.hostId,
            id: this._record.hostId,
            text: value,
            addNewLine: shouldExecute !== false,
            direction: 'input',
        }, 'terminal input failed').catch(() => undefined);
    }
    show(preserveFocus = false) {
        if (this._record.closed) return;
        callHostObserved('vscode.window.showTerminal', {
            ...this._record.owner,
            terminalId: this._record.hostId,
            id: this._record.hostId,
            preserveFocus: preserveFocus === true,
        }, 'terminal show failed').then(result => {
            if (!this._record.closed && result && result.active !== false) {
                setActiveTerminal(this._record);
            }
        }).catch(() => undefined);
    }
    hide() {
        if (this._record.closed) return;
        callHostObserved('vscode.window.hideTerminal', {
            ...this._record.owner,
            terminalId: this._record.hostId,
            id: this._record.hostId,
        }, 'terminal hide failed').then(() => {
            if (state.activeTerminalId === this._record.id) {
                setActiveTerminal(undefined);
            }
        }).catch(() => undefined);
    }
    dispose() {
        const record = this._record;
        if (record.closed || record.disposing) return this[disposalBarrier];
        record.disposing = true;
        const barrier = callHost('vscode.window.disposeTerminal', {
            ...record.owner,
            terminalId: record.hostId,
            id: record.hostId,
            exitCode: record.requestedExitCode,
        }).catch(error => {
            reportAsyncFailure('terminal disposal failed', error);
        }).finally(() => {
            finalizeTerminal(record, record.exitStatus || {
                code: record.requestedExitCode,
                reason: 4,
            });
        });
        this[disposalBarrier] = barrier;
        return barrier;
    }
}

function newTerminalRecord(id, name, creationOptions, pty, scope, owner) {
    let resolveProcessId;
    const processId = new Promise(resolve => { resolveProcessId = resolve; });
    const record = {
        id,
        hostId: id,
        name,
        creationOptions,
        pty,
        scope,
        owner: owner || {},
        processId,
        resolveProcessId,
        processIdSettled: false,
        processIdValue: undefined,
        opened: false,
        closed: false,
        disposing: false,
        active: false,
        state: { isInteractedWith: false },
        dimensions: undefined,
        exitStatus: undefined,
        lastSequence: -1,
        ptySubscriptions: [],
    };
    record.terminal = new Terminal(record);
    state.terminals.set(id, record);
    bindPseudoterminal(record);
    return record;
}

function createTerminalApi(first, shellPath, shellArgs) {
    const scope = requireActivationScope('createTerminal');
    if (state.terminals.size >= kMaximumSurfaceRegistrations) {
        throw new Error('Terminal limit reached');
    }
    const normalized = normalizeTerminalCreation(first, shellPath, shellArgs);
    const owner = surfaceOwner(scope, 'terminal');
    const id = owner.registrationId;
    const record = newTerminalRecord(id, normalized.options.name,
        normalized.creationOptions, normalized.pty, scope, owner);
    trackActivationDisposable(record.terminal);
    const ready = observeRegistration(scope,
        callHost('vscode.window.createTerminal', {
            ...owner,
            terminalId: id,
            id,
            kind: normalized.pty ? 'extension' : 'shell',
            options: normalized.options,
        }).then(result => {
            requireCurrentGeneration(scope, 'Terminal creation');
            if (record.closed) return record.terminal;
            const snapshot = result && (result.terminal || result.snapshot || result);
            const hostId = String(snapshot && (snapshot.terminalId || snapshot.id) || id);
            record.hostId = hostId;
            state.terminalAliases.set(hostId, id);
            markTerminalOpen(record, snapshot || {});
            return record.terminal;
        }).catch(error => {
            finalizeTerminal(record, { code: undefined, reason: 4 });
            throw error;
        }));
    record.ready = ready;
    record.terminal.ready = ready;
    return record.terminal;
}

function reviveTerminal(snapshot) {
    if (!snapshot || typeof snapshot !== 'object') return undefined;
    const id = String(snapshot.terminalId || snapshot.id || '');
    if (!id) return undefined;
    let record = terminalRecordById(id);
    if (!record) {
        const options = boundedSurfaceSnapshot(snapshot.creationOptions || {
            name: snapshot.name || 'Terminal',
        }, 'Terminal snapshot');
        const owner = snapshot.extensionId && Number.isSafeInteger(snapshot.generation)
            ? { extensionId: String(snapshot.extensionId),
                generation: snapshot.generation }
            : {};
        record = newTerminalRecord(id, String(snapshot.name || 'Terminal'),
            options, undefined, undefined, owner);
        record.hostId = id;
        state.terminalAliases.set(id, id);
    }
    markTerminalOpen(record, snapshot);
    return record.terminal;
}

function handleTerminalProcessEvent(payload) {
    const snapshot = payload.snapshot && typeof payload.snapshot === 'object'
        ? payload.snapshot : {};
    const id = processEventId(payload);
    const op = String(payload.op || '');
    let record = terminalRecordById(id);
    if (!record && op !== 'exit' && op !== 'close' && Object.keys(snapshot).length) {
        reviveTerminal(Object.assign({}, snapshot, { terminalId: id }));
        record = terminalRecordById(id);
    }
    if (!record) return op === 'exit' || op === 'close';
    if (Number.isSafeInteger(payload.sequence)) {
        if (payload.sequence <= record.lastSequence) return true;
        record.lastSequence = payload.sequence;
    }
    if (op === 'open') {
        markTerminalOpen(record, snapshot);
    } else if (op === 'data') {
        const data = eventDataBuffer(payload).toString('utf8');
        if (record.pty && (payload.stream === 'stdin' ||
            payload.direction === 'input')) {
            callPseudoterminal(record, 'handleInput', data);
        } else {
            terminalEvents.data.fire({ terminal: record.terminal, data });
        }
        if (!record.state.isInteractedWith) {
            record.state = Object.assign({}, record.state, { isInteractedWith: true });
            terminalEvents.state.fire({ terminal: record.terminal, state: record.state });
        }
    } else if (op === 'dimensions') {
        const dimensions = terminalDimensions(payload.dimensions || snapshot.dimensions || snapshot);
        if (dimensions) {
            record.dimensions = dimensions;
            callPseudoterminal(record, 'setDimensions', dimensions);
            terminalEvents.dimensions.fire({ terminal: record.terminal, dimensions });
        }
    } else if (op === 'exit' || op === 'close') {
        const code = Number.isInteger(payload.code) ? payload.code
            : Number.isInteger(payload.exitCode) ? payload.exitCode
                : Number.isInteger(snapshot.exitCode) ? snapshot.exitCode : undefined;
        const reason = Number.isInteger(payload.reason) ? payload.reason
            : Number.isInteger(snapshot.reason) ? snapshot.reason : 2;
        const disposing = record.disposing;
        finalizeTerminal(record, { code, reason });
        if (!disposing && !snapshot.executionId) {
            callHostObserved('vscode.window.disposeTerminal', {
                ...record.owner, terminalId: record.hostId, id: record.hostId,
            }, 'terminal exit cleanup failed').catch(() => undefined);
        }
    }
    return true;
}

function hydrateTerminals(result) {
    const rows = Array.isArray(result) ? result
        : result && Array.isArray(result.terminals) ? result.terminals : [];
    for (const row of rows.slice(0, kMaximumSurfaceRegistrations)) reviveTerminal(row);
    const activeId = result && (result.activeTerminalId || result.activeId);
    if (activeId) setActiveTerminal(terminalRecordById(activeId));
}

const executionApis = installExecutionApis({
    state, EventEmitter, disposable, disposalBarrier, requireActivationScope,
    activationScopeCurrent, runWithActivation, runWithDeadline, surfaceOwner,
    observeRegistration,
    boundedSurfaceSnapshot, callHost, callHostObserved, reportAsyncFailure,
    neverCancellationToken, registerProcessRoute, queueEarlyProcessEvent,
    processEventId, terminalRecordById, newTerminalRecord, reviveTerminal,
    finalizeTerminal, markTerminalOpen, terminalEvents, Uri, createTerminalApi,
    hydrateTerminals, handleTerminalProcessEvent,
});

const window = {
    activeTextEditor: undefined,
    visibleTextEditors: [],
    get terminals() {
        return Array.from(state.terminals.values())
            .filter(record => !record.closed)
            .map(record => record.terminal);
    },
    get activeTerminal() {
        return terminalRecordById(state.activeTerminalId)?.terminal;
    },
    createTerminal: createTerminalApi,
    onDidOpenTerminal: terminalEvents.open.event,
    onDidCloseTerminal: terminalEvents.close.event,
    onDidChangeActiveTerminal: terminalEvents.active.event,
    onDidWriteTerminalData: terminalEvents.data.event,
    onDidChangeTerminalDimensions: terminalEvents.dimensions.event,
    onDidChangeTerminalState: terminalEvents.state.event,
    registerTerminalProfileProvider: executionApis.registerTerminalProfileProvider,
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
        if (languageCommandKinds[commandId]) return executeLanguageCommand(commandId, args);
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

const languageProviders = new Map();
const languageCommandKinds = {
    'vscode.executeCodeLensProvider': 'codelens',
    'vscode.executeDefinitionProvider': 'definition',
    'vscode.executeHoverProvider': 'hover',
    'vscode.executeCompletionItemProvider': 'completion',
    'vscode.executeFormatDocumentProvider': 'documentFormatting',
};
const diagnosticCollections = new Map();
const diagnosticsChanged = new EventEmitter();
let nextLanguageRegistration = 1;

function languageOwner(scope) {
    if (nextLanguageRegistration >= Number.MAX_SAFE_INTEGER) throw new Error('Registration ids exhausted');
    return { extensionId: scope.extensionId, generation: scope.generation,
        registrationId: `lang-${scope.generation}-${nextLanguageRegistration++}` };
}

function observeRegistration(scope, promise) {
    if (scope.phase === 'active') scope.pendingRegistrations.push(promise);
    promise.catch(reportDisposalFailure);
    return promise;
}

function wireSnapshot(value) {
    const json = JSON.stringify(value, (_key, item) => item instanceof RegExp
        ? { pattern: item.source, flags: item.flags } : item);
    const snapshot = boundedJsonSnapshot(json === undefined ? null : JSON.parse(json), 1024 * 1024);
    if (!snapshot.ok) throw new Error('Language payload exceeds the transport budget');
    return snapshot.value;
}

function registerLanguageProvider(kind, method, selector, provider, metadata = {}) {
    const scope = requireActivationScope(method);
    if (!provider || typeof provider[method] !== 'function') throw new TypeError(`Missing ${method}`);
    if (languageProviders.size >= 512) throw new Error('Language registration limit reached');
    const owner = languageOwner(scope);
    owner.providerId = owner.registrationId;
    const record = { owner, scope, kind, method, provider, selector: wireSnapshot(selector), active: true,
        cancellations: new Set(), subscription: undefined };
    languageProviders.set(owner.providerId, record);
    const ready = observeRegistration(scope, callHost('vscode.languages.registerProvider',
        { ...owner, kind, selector: record.selector, metadata }).catch(error => {
        record.active = false;
        languageProviders.delete(owner.providerId);
        throw error;
    }));
    const value = disposable(() => {
        record.active = false;
        for (const cancel of record.cancellations) cancel();
        record.subscription?.dispose();
        languageProviders.delete(owner.providerId);
        return value[disposalBarrier] = ready.then(() => callHost('vscode.languages.unregisterProvider', owner), () => undefined);
    });
    value.ready = ready;
    record.ready = ready;
    if (kind === 'codelens' && typeof provider.onDidChangeCodeLenses === 'function') {
        record.subscription = provider.onDidChangeCodeLenses(() => {
            if (record.active) observeRegistration(scope, ready.then(() => record.active
                ? callHost('vscode.languages.providerChanged', owner) : undefined));
        });
        trackActivationDisposable(record.subscription);
    }
    return value;
}

function selectorMatches(selector, document) {
    if (Array.isArray(selector)) return selector.some(item => selectorMatches(item, document));
    if (typeof selector === 'string') return selector === '*' || selector === document.languageId;
    if (!selector || typeof selector !== 'object') return false;
    if (selector.language && selector.language !== '*' && selector.language !== document.languageId) return false;
    if (selector.scheme && selector.scheme !== '*' && selector.scheme !== document.uri.scheme) return false;
    if (selector.notebookType) return false;
    if (selector.pattern) {
        const pattern = typeof selector.pattern === 'string' ? selector.pattern : selector.pattern.pattern;
        if (typeof pattern !== 'string' || pattern.length > 4096) return false;
        let file = document.uri.path;
        const base = selector.pattern.baseUri || selector.pattern.base;
        if (base) {
            const basePath = typeof base === 'string' && base.startsWith('file:') ? Uri.parse(base).fsPath
                : typeof base === 'string' ? base : base.fsPath;
            if (!basePath) return false;
            file = path.relative(basePath, document.uri.fsPath).replace(/\\/g, '/');
            if (file === '..' || file.startsWith('../') || path.isAbsolute(file)) return false;
        }
        let expression = '';
        let braces = 0;
        for (let i = 0; i < pattern.length; ++i) {
            const c = pattern[i];
            if (c === '*' && pattern[i + 1] === '*') {
                ++i;
                if (pattern[i + 1] === '/') { ++i; expression += '(?:.*/)?'; }
                else expression += '.*';
            } else if (c === '*') expression += '[^/]*';
            else if (c === '?') expression += '[^/]';
            else if (c === '{') { if (++braces > 8) return false; expression += '(?:'; }
            else if (c === '}') { if (--braces < 0) return false; expression += ')'; }
            else if (c === ',' && braces > 0) expression += '|';
            else if (c === '[') {
                const end = pattern.indexOf(']', i + 1);
                if (end < 0) return false;
                const chars = pattern.slice(i + 1, end);
                if (!chars || chars.includes('/') || chars.includes('\\')) return false;
                expression += '[' + (chars[0] === '!' ? '^' + chars.slice(1) : chars) + ']';
                i = end;
            }
            else expression += c.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        }
        if (braces !== 0) return false;
        try { return new RegExp(`^${expression}$`).test(file); } catch (_) { return false; }
    }
    return true;
}

async function invokeLanguageProvider(request) {
    const record = languageProviders.get(request.providerId);
    const current = () => record && record.active &&
        record.owner.extensionId === request.extensionId && record.owner.generation === request.generation &&
        ['active', 'committed'].includes(record.scope.phase);
    if (!current()) throw new Error('Stale language provider generation');
    const supplied = request.document;
    const document = supplied && typeof supplied.content === 'string'
        ? updateDocument(supplied, Number.isSafeInteger(supplied.version)) : await workspace.openTextDocument(Uri.parse(
            typeof request.uri === 'string' ? request.uri : request.uri?.uri || request.uri?.toString()));
    if (!current()) throw new Error('Stale language provider generation');
    if (!selectorMatches(record.selector, document)) return { result: null };
    const cancellation = new EventEmitter();
    let cancelled = false;
    const cancel = () => { if (!cancelled) { cancelled = true; cancellation.fire(undefined); } };
    const token = { get isCancellationRequested() { return cancelled; }, onCancellationRequested: cancellation.event };
    record.cancellations.add(cancel);
    let args;
    switch (record.kind) {
        case 'codelens': args = [document, token]; break;
        case 'documentFormatting': args = [document, request.options || request.context || {}, token]; break;
        case 'completion': args = [document, asPosition(request.position), token,
            { triggerKind: 0, ...(request.context || {}) }]; break;
        default: args = [document, asPosition(request.position), token];
    }
    try {
        const result = await runWithDeadline(() => runWithActivation(record.scope,
            () => record.provider[record.method](...args)), Date.now() + 10000, 'Language provider timed out');
        if (!current()) throw new Error('Language provider disposed during callback');
        return { result: wireSnapshot(result) };
    } catch (error) {
        cancel();
        throw error;
    } finally {
        cancellation.dispose();
        record.cancellations.delete(cancel);
    }
}

async function executeLanguageCommand(command, args) {
    const kind = languageCommandKinds[command];
    const document = await workspace.openTextDocument(args[0]);
    const items = [];
    let incomplete = false;
    for (const record of Array.from(languageProviders.values())) {
        if (!record.active || record.kind !== kind || !selectorMatches(record.selector, document)) continue;
        await record.ready;
        const response = await invokeLanguageProvider({ ...record.owner, document: document._snapshot,
            uri: document.uri.toString(), position: args[1], options: args[1],
            context: { triggerKind: args[2] === undefined ? 0 : 1, triggerCharacter: args[2] } });
        const result = reviveLanguageValue(response.result);
        if (result === null || result === undefined) continue;
        if (kind === 'documentFormatting') return result;
        if (kind === 'completion' && !Array.isArray(result)) {
            incomplete ||= result.isIncomplete === true;
            items.push(...(result.items || []));
        } else items.push(...(Array.isArray(result) ? result : [result]));
    }
    return kind === 'completion' ? new vscodeModule.CompletionList(items, incomplete) : items;
}

function createDiagnosticCollection(name) {
    const scope = requireActivationScope('createDiagnosticCollection');
    const owner = languageOwner(scope);
    name = name === undefined ? owner.registrationId : String(name);
    if (!name || Buffer.byteLength(name, 'utf8') > 256) throw new TypeError('Invalid diagnostic collection name');
    if (diagnosticCollections.size >= 512) throw new Error('Diagnostic collection limit reached');
    const entries = new Map();
    let disposed = false;
    let tail = Promise.resolve();
    const enqueue = (method, params) => {
        tail = observeRegistration(scope, tail.catch(() => undefined).then(() => callHost(method,
            { ...owner, collection: name, ...params })));
        return tail;
    };
    const check = () => { if (disposed) throw new Error('Diagnostic collection is disposed'); };
    const collection = {
        name,
        set(uri, diagnostics) {
            check();
            if (uri === undefined) { this.clear(); return; }
            const merged = new Map();
            for (const [target, values] of Array.isArray(uri) ? uri : [[uri, diagnostics]]) {
                const key = target.toString();
                if (values === undefined) merged.set(key, undefined);
                else {
                    if (!Array.isArray(values)) throw new TypeError('Expected diagnostics array');
                    merged.set(key, [...(merged.get(key) || []), ...values]);
                }
            }
            const candidate = new Map(entries);
            const updates = [];
            for (const [key, values] of merged) {
                const copy = values === undefined ? undefined : wireSnapshot(values);
                if (copy === undefined || !copy.length) candidate.delete(key); else candidate.set(key, copy);
                updates.push({ uri: key, diagnostics: copy || [] });
            }
            wireSnapshot(Array.from(candidate));
            const payload = wireSnapshot(updates);
            entries.clear();
            for (const [key, values] of candidate) entries.set(key, values);
            enqueue('vscode.languages.setDiagnostics', { updates: payload });
        },
        delete(uri) { this.set(uri, undefined); },
        clear() { check(); entries.clear(); enqueue('vscode.languages.setDiagnostics', {}); },
        get(uri) { check(); const found = entries.get(uri.toString()); return found && reviveLanguageValue(wireSnapshot(found)); },
        has(uri) { check(); return entries.has(uri.toString()); },
        forEach(callback, thisArg) {
            check();
            for (const [key] of entries) { const uri = Uri.parse(key); callback.call(thisArg, uri, this.get(uri), this); }
        },
        *[Symbol.iterator]() { check(); for (const [key] of entries) { const uri = Uri.parse(key); yield [uri, this.get(uri)]; } },
        dispose() {
            if (disposed) return collection[disposalBarrier];
            disposed = true;
            entries.clear();
            diagnosticCollections.delete(owner.registrationId);
            return collection[disposalBarrier] = enqueue('vscode.languages.disposeDiagnostics', {});
        },
    };
    diagnosticCollections.set(owner.registrationId, collection);
    enqueue('vscode.languages.setDiagnostics', {});
    return trackActivationDisposable(collection);
}

function reviveLanguageValue(value) {
    if (Array.isArray(value)) return value.map(reviveLanguageValue);
    if (!value || typeof value !== 'object') return value;
    if (Number.isInteger(value.line) && Number.isInteger(value.character)) return asPosition(value);
    if (value.start && value.end && Number.isInteger(value.start.line)) return new Range(value.start, value.end);
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key,
        ['uri', 'targetUri'].includes(key) && typeof item === 'string' ? Uri.parse(item) : reviveLanguageValue(item)]));
}

const languages = {
    async getLanguages() {
        return callHost('vscode.languages.getLanguages', {});
    },
    registerCodeLensProvider(selector, provider) { return registerLanguageProvider('codelens', 'provideCodeLenses', selector, provider); },
    registerDefinitionProvider(selector, provider) { return registerLanguageProvider('definition', 'provideDefinition', selector, provider); },
    registerHoverProvider(selector, provider) { return registerLanguageProvider('hover', 'provideHover', selector, provider); },
    registerCompletionItemProvider(selector, provider, ...triggerCharacters) {
        return registerLanguageProvider('completion', 'provideCompletionItems', selector, provider, { triggerCharacters });
    },
    registerDocumentFormattingEditProvider(selector, provider) {
        return registerLanguageProvider('documentFormatting', 'provideDocumentFormattingEdits', selector, provider);
    },
    setLanguageConfiguration(language, configuration) {
        const scope = requireActivationScope('setLanguageConfiguration');
        const owner = { ...languageOwner(scope), language };
        const ready = observeRegistration(scope, callHost('vscode.languages.setLanguageConfiguration',
            { ...owner, configuration: wireSnapshot(configuration) }));
        const value = disposable(() => value[disposalBarrier] = ready.then(() =>
            callHost('vscode.languages.disposeLanguageConfiguration', owner), () => undefined));
        value.ready = ready;
        return value;
    },
    createDiagnosticCollection,
    onDidChangeDiagnostics: diagnosticsChanged.event,
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
        case '.c': return 'c';
        case '.lua': return 'lua';
        case '.emma': return 'emma';
        case '.as': return 'angelscript';
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
    Position, Range, TextEdit, WorkspaceEdit,
    EndOfLine: { LF: 1, CRLF: 2 },
    DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
    DiagnosticTag: { Unnecessary: 1, Deprecated: 2 },
    CompletionTriggerKind: { Invoke: 0, TriggerCharacter: 1, TriggerForIncompleteCompletions: 2 },
    CompletionItemKind: Object.fromEntries(['Text', 'Method', 'Function', 'Constructor', 'Field', 'Variable',
        'Class', 'Interface', 'Module', 'Property', 'Unit', 'Value', 'Enum', 'Keyword', 'Snippet', 'Color',
        'File', 'Reference', 'Folder', 'EnumMember', 'Constant', 'Struct', 'Event', 'Operator', 'TypeParameter',
        'User', 'Issue'].map((name, index) => [name, index])),
    Diagnostic: class { constructor(range, message, severity = 0) { Object.assign(this, { range, message, severity }); } },
    DiagnosticRelatedInformation: class { constructor(location, message) { Object.assign(this, { location, message }); } },
    Location: class { constructor(uri, range) { this.uri = uri; this.range = range instanceof Position ? new Range(range, range) : range; } },
    Hover: class { constructor(contents, range) { this.contents = Array.isArray(contents) ? contents : [contents]; this.range = range; } },
    CodeLens: class { constructor(range, command) { Object.assign(this, { range, command }); } get isResolved() { return !!this.command; } },
    CompletionItem: class { constructor(label, kind) { Object.assign(this, { label, kind }); } },
    CompletionList: class { constructor(items = [], isIncomplete = false) { Object.assign(this, { items, isIncomplete }); } },
    MarkdownString: class {
        constructor(value = '', supportThemeIcons = false) { Object.assign(this, { value, supportThemeIcons }); }
        appendText(value) { this.value += String(value).replace(/[\\`*_{}[\]()<>#+.!~-]/g, '\\$&'); return this; }
        appendMarkdown(value) { this.value += value; return this; }
        appendCodeblock(value, language = '') { this.value += `\n\n\`\`\`${language}\n${value}\n\`\`\`\n\n`; return this; }
    },
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
    tasks: executionApis.tasks,
    debug: executionApis.debug,
    env,
    Task: executionApis.Task,
    TaskExecution: executionApis.TaskExecution,
    ProcessExecution: executionApis.ProcessExecution,
    ShellExecution: executionApis.ShellExecution,
    CustomExecution: executionApis.CustomExecution,
    TaskScope: executionApis.TaskScope,
    TaskGroup: executionApis.TaskGroup,
    TaskRevealKind: executionApis.TaskRevealKind,
    TaskPanelKind: executionApis.TaskPanelKind,
    DebugSession: executionApis.DebugSession,
    DebugAdapterExecutable: executionApis.DebugAdapterExecutable,
    DebugAdapterServer: executionApis.DebugAdapterServer,
    DebugAdapterNamedPipeServer: executionApis.DebugAdapterNamedPipeServer,
    DebugAdapterInlineImplementation: executionApis.DebugAdapterInlineImplementation,
    SourceBreakpoint: executionApis.SourceBreakpoint,
    FunctionBreakpoint: executionApis.FunctionBreakpoint,
    DataBreakpoint: executionApis.DataBreakpoint,
    DebugConfigurationProviderTriggerKind:
        executionApis.DebugConfigurationProviderTriggerKind,
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
    await executionApis.initialize();
    return {
        protocolVersion: 'sao-ai-editor/1',
        serverInfo: { name: 'sao-extension-host-shim', version: '1.0' },
        capabilities: {
            extensions: { activate: true, deactivate: true, treeDataProvider: true },
            commands: { execute: true },
            terminals: { shell: true, pseudoterminal: true, conpty: true },
            tasks: { providers: true, execution: true, lifecycle: true },
            debug: { providers: true, dap: true, lifecycle: true },
        },
    };
});
state.handlers.set('host.initialized', () => null);

state.handlers.set('sao.extapi.event', event => {
    const payload = event.payload || {};
    if (executionApis.handleEvent(event.kind, payload)) {
        return { handled: true };
    }
    if (event.kind === 'document') {
        const key = Uri.parse(payload.uri).toString();
        const previous = documents.get(key);
        const previousVersion = previous?.version;
        const previousDirty = previous?.isDirty;
        if (payload.document && payload.document.version < previousVersion) return;
        const document = payload.document ? updateDocument(payload.document) : previous;
        if (!document) return;
        if (payload.op === 'close') {
            document._snapshot.isClosed = true;
            documents.delete(key);
            providerDocuments.delete(key);
            documentEvents.close.fire(document);
        } else if (payload.op === 'open' && !previous) documentEvents.open.fire(document);
        else if (payload.op === 'change' && (previousVersion === undefined || document.version > previousVersion || previousDirty !== document.isDirty))
            documentEvents.change.fire({ document, contentChanges: reviveLanguageValue(payload.changes || []) });
        else if (payload.op === 'save') documentEvents.save.fire(document);
    } else if (event.kind === 'configChanged') {
        documentEvents.config.fire({ affectsConfiguration(section) {
            return !payload.section || section === payload.section || section.startsWith(payload.section + '.') || payload.section.startsWith(section + '.');
        } });
    } else if (event.kind === 'diagnostics') {
        diagnosticsChanged.fire({ uris: (payload.uris || []).map(uri => Uri.parse(uri)) });
    } else if (event.kind === 'workspaceFolders' && Array.isArray(payload.folders)) {
        workspace.workspaceFolders = payload.folders.map(folder => ({ ...folder, uri: Uri.parse(folder.uri) }));
        documentEvents.folders.fire({ added: (payload.added || []).map(folder => ({ ...folder, uri: Uri.parse(folder.uri) })),
            removed: (payload.removed || []).map(folder => ({ ...folder, uri: Uri.parse(folder.uri) })) });
    }
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
        pendingRegistrations: [],
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
            if (!workspaceInitialized) {
                const snapshot = await callHost('vscode.workspace.documents', { includeContent: true });
                for (const document of snapshot.documents || []) updateDocument(document);
                const folders = await callHost('vscode.workspace.workspaceFolders', {});
                workspace.workspaceFolders = folders.map(folder => ({ ...folder, uri: Uri.parse(folder.uri) }));
                workspaceInitialized = true;
            }
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
            while (activation.pendingRegistrations.length)
                await Promise.all(activation.pendingRegistrations.splice(0));
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
    if (commandId === 'sao.internal.extapi.invoke') {
        if (args[0]?.invoke === 'languages') return invokeLanguageProvider(args[0]);
        return executionApis.invoke(args[0]);
    }
    if (commandId === 'sao.internal.extapi.event') return state.handlers.get('sao.extapi.event')(args[0] || {});
    if (commandId === 'sao.internal.tasks.fetch') {
        const items = await executionApis.tasks.fetchTasks(args[0] || undefined);
        return items.map(task => ({ ...task, execution: task.execution,
            _providerKey: task._providerKey }));
    }
    if (commandId === 'sao.internal.tasks.execute') {
        const execution = await executionApis.tasks.executeTask(args[0]);
        return { executionId: execution.id };
    }
    if (commandId === 'sao.internal.tasks.terminate') {
        const execution = state.taskExecutions.get(String(args[0] || ''))?.execution;
        if (!execution) return false;
        await execution.terminate();
        return true;
    }
    if (commandId === 'sao.internal.debug.start') {
        const started = await executionApis.debug.startDebugging(
            args[0], args[1] || {}, undefined, args[2] || {});
        const session = executionApis.debug.activeDebugSession;
        return { started, sessionId: session?.id, name: session?.name,
            type: session?.type, running: started === true,
            configuration: session?.configuration };
    }
    if (commandId === 'sao.internal.debug.configurations') {
        return { configurations: await executionApis.provideDebugConfigurations(
            args[0] ? String(args[0]) : '', args[1] || null) };
    }
    if (commandId === 'sao.internal.debug.stop') {
        const session = args[0]
            ? state.debugSessions.get(String(args[0]))?.session : undefined;
        return executionApis.debug.stopDebugging(session);
    }
    if (commandId === 'sao.internal.debug.sessions') {
        return Array.from(state.debugSessions.values()).map(record => ({
            sessionId: record.id, name: record.session.name,
            type: record.session.type, running: record.active,
            configuration: record.configuration,
        }));
    }
    if (commandId === 'sao.internal.debug.evaluate') {
        const record = args[1]
            ? state.debugSessions.get(String(args[1]))
            : state.debugSessions.get(state.activeDebugSessionId);
        if (!record) throw new Error('No active debug session');
        return record.session.customRequest('evaluate', {
            expression: String(args[0] || ''), context: 'repl',
            frameId: Number.isInteger(args[2]) ? args[2] : undefined,
        });
    }
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
