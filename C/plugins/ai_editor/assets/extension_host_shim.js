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
const Module = require('module');
const process_ = require('process');

const stdin = process_.stdin;
const stdout = process_.stdout;

const state = {
    nextId: 1,
    pending: new Map(),
    handlers: new Map(),
    commands: new Map(),
    outputChannels: new Map(),
    extensions: new Map(),
    disposables: [],
    initialized: false,
};

function encodeFrame(payload) {
    const body = Buffer.from(JSON.stringify(payload), 'utf8');
    const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'utf8');
    return Buffer.concat([header, body]);
}

function send(payload) {
    try {
        stdout.write(encodeFrame(payload));
    } catch (err) {
        process_.stderr.write(`[shim] send failed: ${err.message}\n`);
    }
}

function callHost(method, params) {
    const id = state.nextId++;
    return new Promise((resolve, reject) => {
        state.pending.set(id, { resolve, reject });
        send({ jsonrpc: '2.0', id, method, params: params || {} });
    });
}

function notifyHost(method, params) {
    send({ jsonrpc: '2.0', method, params: params || {} });
}

// ----- vscode module polyfill --------------------------------------------

const disposable = (dispose) => ({ dispose: () => { try { dispose && dispose(); } catch (e) { /* noop */ } } });

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
        constructor() { this._listeners = new Set(); }
        get event() {
            return (listener) => {
                this._listeners.add(listener);
                return disposable(() => this._listeners.delete(listener));
            };
        }
        fire(value) {
            for (const listener of Array.from(this._listeners)) {
                try { listener(value); } catch (e) { /* noop */ }
            }
        }
        dispose() { this._listeners.clear(); }
    };
})();

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
};

const commands = {
    registerCommand(commandId, handler) {
        state.commands.set(commandId, handler);
        return disposable(() => state.commands.delete(commandId));
    },
    registerTextEditorCommand(commandId, handler) {
        state.commands.set(commandId, handler);
        return disposable(() => state.commands.delete(commandId));
    },
    getCommands(_filterInternal) {
        return Promise.resolve(Array.from(state.commands.keys()));
    },
    async executeCommand(commandId, ...args) {
        // Try local dispatch first, then host bounce.
        const local = state.commands.get(commandId);
        if (typeof local === 'function') {
            try { return await Promise.resolve(local(...args)); }
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
    Disposable: class {
        static from(...ds) { return disposable(() => ds.forEach((d) => d && d.dispose && d.dispose())); }
        constructor(dispose) { this._dispose = dispose; }
        dispose() { try { this._dispose && this._dispose(); } catch (e) { /* noop */ } }
    },
    workspace,
    window,
    commands,
    languages,
    env,
    ExtensionContext: class { constructor() { this.subscriptions = []; } },
    ExtensionMode: { Development: 1, Production: 2, Test: 3 },
    ConfigurationTarget: { Global: 1, Workspace: 2, WorkspaceFolder: 3 },
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
            extensions: { activate: true, deactivate: true },
            commands: { execute: true },
        },
    };
});

state.handlers.set('host.activate', async (params) => {
    const extensionId = String(params.extensionId || '');
    const extensionPath = String(params.extensionPath || '');
    const mainRelative = String(params.main || '');
    if (!extensionId || !extensionPath) {
        throw new Error('extensionId + extensionPath required');
    }
    const resolved = path.isAbsolute(mainRelative)
        ? mainRelative
        : path.join(extensionPath, mainRelative || 'extension.js');
    let mod;
    try {
        // Clear cache so hot-reload works.
        delete require.cache[require.resolve(resolved)];
        mod = require(resolved);
    } catch (err) {
        throw new Error(`activate failed: ${err.stack || err.message}`);
    }
    const context = {
        subscriptions: [],
        extensionPath,
        extensionUri: Uri.file(extensionPath),
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
        asAbsolutePath(rel) { return path.join(extensionPath, rel); },
        storagePath: extensionPath,
        globalStoragePath: extensionPath,
        logPath: extensionPath,
        extensionMode: vscodeModule.ExtensionMode.Production,
    };
    let activationResult;
    if (mod && typeof mod.activate === 'function') {
        activationResult = await Promise.resolve(mod.activate(context));
    }
    state.extensions.set(extensionId, { module: mod, context });
    return {
        activated: true,
        extensionId,
        exports: activationResult === undefined ? null : activationResult,
        subscriptionCount: context.subscriptions.length,
    };
});

state.handlers.set('host.deactivate', async (params) => {
    const extensionId = String(params.extensionId || '');
    const entry = state.extensions.get(extensionId);
    if (!entry) { return { deactivated: false, reason: 'not-activated' }; }
    try {
        if (entry.module && typeof entry.module.deactivate === 'function') {
            await Promise.resolve(entry.module.deactivate());
        }
        for (const sub of entry.context.subscriptions.slice().reverse()) {
            try { sub && sub.dispose && sub.dispose(); } catch (e) { /* noop */ }
        }
    } finally {
        state.extensions.delete(extensionId);
    }
    return { deactivated: true, extensionId };
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
    const handler = state.commands.get(commandId);
    if (typeof handler !== 'function') {
        throw new Error(`unknown command: ${commandId}`);
    }
    return await Promise.resolve(handler(...args));
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
    if (message.method !== undefined && message.id !== undefined) {
        const handler = state.handlers.get(message.method);
        const reply = { jsonrpc: '2.0', id: message.id };
        if (!handler) {
            reply.error = { code: -32601, message: `method not found: ${message.method}` };
        } else {
            try {
                reply.result = await Promise.resolve(handler(message.params || {}));
            } catch (err) {
                reply.error = { code: -32000, message: err.message || String(err), data: { stack: err.stack || null } };
            }
        }
        send(reply);
        return;
    }
    if (message.method !== undefined) {
        const handler = state.handlers.get(message.method);
        if (handler) { try { await Promise.resolve(handler(message.params || {})); } catch (e) { /* noop */ } }
        return;
    }
    if (message.id !== undefined) {
        const pending = state.pending.get(message.id);
        if (pending) {
            state.pending.delete(message.id);
            if (message.error) { pending.reject(new Error(message.error.message || 'call failed')); }
            else { pending.resolve(message.result); }
        }
    }
}

// ----- Content-Length framed stdin parser --------------------------------

let buffer = Buffer.alloc(0);

stdin.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
        const separator = buffer.indexOf('\r\n\r\n');
        if (separator < 0) { return; }
        const header = buffer.slice(0, separator).toString('utf8');
        const match = /Content-Length:\s*(\d+)/i.exec(header);
        if (!match) {
            buffer = buffer.slice(separator + 4);
            continue;
        }
        const size = parseInt(match[1], 10);
        if (buffer.length - separator - 4 < size) { return; }
        const body = buffer.slice(separator + 4, separator + 4 + size).toString('utf8');
        buffer = buffer.slice(separator + 4 + size);
        try {
            const parsed = JSON.parse(body);
            dispatchInbound(parsed).catch((err) => {
                process_.stderr.write(`[shim] dispatch error: ${err.stack || err.message}\n`);
            });
        } catch (err) {
            process_.stderr.write(`[shim] parse error: ${err.message}\n`);
        }
    }
});

stdin.on('end', () => { process_.exit(0); });
stdin.on('close', () => { process_.exit(0); });

// Boot: wait for the parent's host.initialize call.
