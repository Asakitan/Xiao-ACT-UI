/**
 * Node.js Extension Host — spawned by our Python backend.
 *
 * Loads and activates VSCode extensions that have Node.js entry points.
 * Communicates with Python over stdin/stdout JSON lines (one JSON object
 * per line). stderr is used for debug logging only.
 *
 * Inbound (from Python):
 *   activate, deactivate, webview_message, command, shutdown
 *
 * Outbound (to Python):
 *   activated, webview_html, webview_post_message, command_registered,
 *   output, error, show_message
 */
'use strict';

const path = require('node:path');
const Module = require('node:module');
const readline = require('node:readline');

// -------------------------------------------------------------------------
// Logging — stderr only
// -------------------------------------------------------------------------
const log = (...args) => process.stderr.write(`[ext-host] ${args.join(' ')}\n`);

// -------------------------------------------------------------------------
// Send message to Python parent
// -------------------------------------------------------------------------
function send(msg) {
    process.stdout.write(JSON.stringify(msg) + '\n');
}

// -------------------------------------------------------------------------
// EventEmitter (lightweight, mirrors vscode.EventEmitter)
// -------------------------------------------------------------------------
class EventEmitter {
    constructor() { this._listeners = []; }
    get event() { return (fn) => this._subscribe(fn); }
    _subscribe(fn) {
        this._listeners.push(fn);
        return new Disposable(() => {
            const i = this._listeners.indexOf(fn);
            if (i >= 0) this._listeners.splice(i, 1);
        });
    }
    fire(data) {
        for (const fn of [...this._listeners]) {
            try { fn(data); } catch (e) { log('EventEmitter listener error:', e.message); }
        }
    }
    dispose() { this._listeners.length = 0; }
}

// -------------------------------------------------------------------------
// Disposable
// -------------------------------------------------------------------------
class Disposable {
    constructor(fn) { this._fn = fn; }
    dispose() { if (this._fn) { this._fn(); this._fn = null; } }
    static from(...disposables) {
        return new Disposable(() => disposables.forEach(d => d?.dispose?.()));
    }
}

// -------------------------------------------------------------------------
// Uri
// -------------------------------------------------------------------------
class Uri {
    constructor(scheme, authority, path_, query, fragment) {
        this.scheme = scheme || 'file';
        this.authority = authority || '';
        this.path = path_ || '';
        this.query = query || '';
        this.fragment = fragment || '';
    }
    get fsPath() { return this.path.replace(/\//g, path.sep); }
    toString() {
        if (this.scheme === 'file') {
            const p = this.path.startsWith('/') ? this.path : '/' + this.path;
            return `file://${this.authority}${p}`;
        }
        return `${this.scheme}://${this.authority}${this.path}`;
    }
    with(change) {
        return new Uri(
            change.scheme ?? this.scheme,
            change.authority ?? this.authority,
            change.path ?? this.path,
            change.query ?? this.query,
            change.fragment ?? this.fragment,
        );
    }
    static file(fsPath) { return new Uri('file', '', fsPath.replace(/\\/g, '/')); }
    static parse(value) {
        try {
            const u = new URL(value);
            return new Uri(u.protocol.replace(/:$/, ''), u.host, decodeURIComponent(u.pathname), u.search.replace(/^\?/, ''), u.hash.replace(/^#/, ''));
        } catch { return Uri.file(value); }
    }
    static joinPath(base, ...segments) {
        return Uri.file(path.join(base.fsPath, ...segments));
    }
}

// -------------------------------------------------------------------------
// Position / Range / Selection / Location
// -------------------------------------------------------------------------
class Position {
    constructor(line = 0, character = 0) { this.line = line; this.character = character; }
    isEqual(other) { return this.line === other.line && this.character === other.character; }
    isBefore(other) { return this.line < other.line || (this.line === other.line && this.character < other.character); }
    isAfter(other) { return !this.isEqual(other) && !this.isBefore(other); }
    translate(lineDelta = 0, charDelta = 0) { return new Position(this.line + lineDelta, this.character + charDelta); }
}

class Range {
    constructor(startOrLine, startCharOrEnd, endLine, endChar) {
        if (typeof startOrLine === 'number') {
            this.start = new Position(startOrLine, startCharOrEnd ?? 0);
            this.end = new Position(endLine ?? startOrLine, endChar ?? 0);
        } else {
            this.start = startOrLine || new Position();
            this.end = startCharOrEnd || new Position();
        }
    }
    get isEmpty() { return this.start.isEqual(this.end); }
    contains(posOrRange) { return true; /* stub */ }
}

class Selection extends Range {
    constructor(anchor, active) {
        super(anchor, active);
        this.anchor = anchor;
        this.active = active;
    }
    get isReversed() { return this.anchor.isAfter(this.active); }
}

class Location {
    constructor(uri, rangeOrPosition) {
        this.uri = uri;
        this.range = rangeOrPosition instanceof Range ? rangeOrPosition : new Range(rangeOrPosition, rangeOrPosition);
    }
}

// -------------------------------------------------------------------------
// CancellationToken
// -------------------------------------------------------------------------
class CancellationTokenSource {
    constructor() {
        this._emitter = new EventEmitter();
        this.token = { isCancellationRequested: false, onCancellationRequested: this._emitter.event };
    }
    cancel() {
        this.token.isCancellationRequested = true;
        this._emitter.fire();
    }
    dispose() { this._emitter.dispose(); }
}

// -------------------------------------------------------------------------
// Memento (globalState / workspaceState)
// -------------------------------------------------------------------------
class Memento {
    constructor() { this._data = {}; }
    get(key, defaultValue) { return key in this._data ? this._data[key] : defaultValue; }
    update(key, value) { this._data[key] = value; return Promise.resolve(); }
    keys() { return Object.keys(this._data); }
    setKeysForSync() {}
}

// -------------------------------------------------------------------------
// Webview + WebviewView
// -------------------------------------------------------------------------
let _nextViewHandle = 1;

class Webview {
    constructor(viewId) {
        this._viewId = viewId;
        this._html = '';
        this._options = {};
        this._onDidReceiveMessage = new EventEmitter();
        this.onDidReceiveMessage = this._onDidReceiveMessage.event;
        this.cspSource = 'https://cdn.example.com';
    }
    get html() { return this._html; }
    set html(value) {
        this._html = value;
        send({ type: 'webview_html', viewId: this._viewId, html: value });
    }
    get options() { return this._options; }
    set options(value) { this._options = value; }
    postMessage(message) {
        send({ type: 'webview_post_message', viewId: this._viewId, message });
        return Promise.resolve(true);
    }
    asWebviewUri(localUri) {
        return Uri.parse(`https://webview.local/${localUri.path}`);
    }
}

class WebviewView {
    constructor(viewId, viewType) {
        this.viewType = viewType;
        this.webview = new Webview(viewId);
        this._title = '';
        this._description = '';
        this._badge = undefined;
        this.visible = true;
        this._onDidDispose = new EventEmitter();
        this.onDidDispose = this._onDidDispose.event;
        this._onDidChangeVisibility = new EventEmitter();
        this.onDidChangeVisibility = this._onDidChangeVisibility.event;
    }
    get title() { return this._title; }
    set title(v) { this._title = v; }
    get description() { return this._description; }
    set description(v) { this._description = v; }
    get badge() { return this._badge; }
    set badge(v) { this._badge = v; }
    show(preserveFocus) { this.visible = true; }
    dispose() { this._onDidDispose.fire(); }
}

// -------------------------------------------------------------------------
// OutputChannel
// -------------------------------------------------------------------------
class OutputChannel {
    constructor(name) {
        this.name = name;
        this._lines = [];
    }
    append(text) {
        this._lines.push(text);
        send({ type: 'output', channelName: this.name, text });
    }
    appendLine(text) { this.append(text + '\n'); }
    clear() { this._lines.length = 0; }
    show() {}
    hide() {}
    replace(value) { this.clear(); this.append(value); }
    dispose() {}
}

// -------------------------------------------------------------------------
// Global registries
// -------------------------------------------------------------------------
const _extensions = new Map();           // extensionId -> { desc, module, context, deactivate }
const _commands = new Map();             // commandId -> handler
const _webviewViewProviders = new Map(); // viewType -> { provider, options }
const _webviewViews = new Map();         // viewId -> WebviewView
const _outputChannels = new Map();       // name -> OutputChannel

// -------------------------------------------------------------------------
// Build the mock vscode module for a given extension
// -------------------------------------------------------------------------
function buildVscodeModule(extDesc, extensionPath) {
    const subscriptions = [];
    const globalState = new Memento();
    const workspaceState = new Memento();
    const secretStorage = {
        get: (key) => Promise.resolve(undefined),
        store: (key, value) => Promise.resolve(),
        delete: (key) => Promise.resolve(),
        onDidChange: new EventEmitter().event,
    };
    const extensionUri = Uri.file(extensionPath);

    const context = {
        subscriptions,
        extensionPath,
        extensionUri,
        globalState,
        workspaceState,
        secrets: secretStorage,
        storagePath: path.join(extensionPath, '.storage'),
        storageUri: Uri.file(path.join(extensionPath, '.storage')),
        globalStoragePath: path.join(extensionPath, '.global-storage'),
        globalStorageUri: Uri.file(path.join(extensionPath, '.global-storage')),
        logPath: path.join(extensionPath, '.log'),
        logUri: Uri.file(path.join(extensionPath, '.log')),
        extensionMode: 3, // Production
        extension: {
            id: extDesc.extensionId || '',
            extensionUri,
            extensionPath,
            isActive: true,
            packageJSON: extDesc.manifest || {},
            extensionKind: 2, // Workspace
            exports: undefined,
        },
        environmentVariableCollection: {
            persistent: false,
            description: '',
            replace: () => {},
            append: () => {},
            prepend: () => {},
            get: () => undefined,
            forEach: () => {},
            delete: () => {},
            clear: () => {},
            [Symbol.iterator]: function* () {},
        },
        languageModelAccessInformation: {
            onDidChange: new EventEmitter().event,
            canSendRequest: () => true,
        },
    };

    // Build the vscode namespace
    const vscode = {
        // --- Types ---
        Uri,
        Position,
        Range,
        Selection,
        Location,
        Disposable,
        EventEmitter,
        CancellationTokenSource,

        // --- Enums ---
        ViewColumn: { One: 1, Two: 2, Three: 3, Active: -1, Beside: -2 },
        StatusBarAlignment: { Left: 1, Right: 2 },
        TreeItemCollapsibleState: { None: 0, Collapsed: 1, Expanded: 2 },
        ExtensionKind: { UI: 1, Workspace: 2 },
        ExtensionMode: { Production: 1, Development: 2, Test: 3 },
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
        CompletionItemKind: Object.fromEntries([
            'Text', 'Method', 'Function', 'Constructor', 'Field', 'Variable',
            'Class', 'Interface', 'Module', 'Property', 'Unit', 'Value',
            'Enum', 'Keyword', 'Snippet', 'Color', 'File', 'Reference',
            'Folder', 'EnumMember', 'Constant', 'Struct', 'Event', 'Operator',
            'TypeParameter',
        ].map((n, i) => [n, i])),
        SymbolKind: Object.fromEntries([
            'File', 'Module', 'Namespace', 'Package', 'Class', 'Method',
            'Property', 'Field', 'Constructor', 'Enum', 'Interface',
            'Function', 'Variable', 'Constant', 'String', 'Number',
            'Boolean', 'Array', 'Object', 'Key', 'Null', 'EnumMember',
            'Struct', 'Event', 'Operator', 'TypeParameter',
        ].map((n, i) => [n, i])),
        ColorThemeKind: { Light: 1, Dark: 2, HighContrast: 3, HighContrastLight: 4 },
        UIKind: { Desktop: 1, Web: 2 },
        LogLevel: { Off: 0, Trace: 1, Debug: 2, Info: 3, Warning: 4, Error: 5 },
        LanguageModelChatMessageRole: { User: 1, Assistant: 2 },
        ChatResultFeedbackKind: { Unhelpful: 0, Helpful: 1 },

        // --- Namespace: commands ---
        commands: {
            registerCommand(id, handler, thisArg) {
                const wrapped = thisArg ? handler.bind(thisArg) : handler;
                _commands.set(id, wrapped);
                send({ type: 'command_registered', commandId: id });
                const d = new Disposable(() => _commands.delete(id));
                subscriptions.push(d);
                return d;
            },
            executeCommand(id, ...args) {
                const handler = _commands.get(id);
                if (handler) return Promise.resolve(handler(...args));
                // Forward to Python for commands we don't own
                return new Promise((resolve) => {
                    // Fire-and-forget for now; full RPC tracking can be added later
                    send({ type: 'execute_command', commandId: id, args });
                    resolve(undefined);
                });
            },
            registerTextEditorCommand(id, handler) {
                return vscode.commands.registerCommand(id, handler);
            },
            getCommands(filterInternal) {
                return Promise.resolve([..._commands.keys()]);
            },
        },

        // --- Namespace: window ---
        window: {
            registerWebviewViewProvider(viewType, provider, options) {
                _webviewViewProviders.set(viewType, { provider, options: options || {} });
                log(`registered WebviewViewProvider: ${viewType}`);
                return new Disposable(() => _webviewViewProviders.delete(viewType));
            },
            createWebviewPanel(viewType, title, showOptions, options) {
                const viewId = `panel-${_nextViewHandle++}`;
                const view = new WebviewView(viewId, viewType);
                view.title = title;
                _webviewViews.set(viewId, view);
                log(`stub: createWebviewPanel ${viewType} -> ${viewId}`);
                return {
                    viewType,
                    title,
                    webview: view.webview,
                    visible: true,
                    active: true,
                    viewColumn: 1,
                    onDidDispose: view.onDidDispose,
                    onDidChangeViewState: new EventEmitter().event,
                    reveal() {},
                    dispose() { view.dispose(); },
                };
            },
            createOutputChannel(name, options) {
                const ch = new OutputChannel(typeof options === 'string' ? `${name} (${options})` : name);
                _outputChannels.set(ch.name, ch);
                return ch;
            },
            showInformationMessage(message, ...items) {
                send({ type: 'show_message', level: 'info', message: String(message) });
                return Promise.resolve(items[0]);
            },
            showWarningMessage(message, ...items) {
                send({ type: 'show_message', level: 'warn', message: String(message) });
                return Promise.resolve(items[0]);
            },
            showErrorMessage(message, ...items) {
                send({ type: 'show_message', level: 'error', message: String(message) });
                return Promise.resolve(items[0]);
            },
            showQuickPick(items, options) {
                log('stub: showQuickPick');
                return Promise.resolve(Array.isArray(items) ? items[0] : undefined);
            },
            showInputBox(options) {
                log('stub: showInputBox');
                return Promise.resolve(options?.value || '');
            },
            withProgress(options, task) {
                const progress = { report: () => {} };
                const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
                return task(progress, token);
            },
            createStatusBarItem(alignmentOrId, priorityOrAlignment, priority) {
                return {
                    alignment: 1, priority: 0, text: '', tooltip: '', color: '',
                    backgroundColor: undefined, command: undefined, name: '',
                    show() {}, hide() {}, dispose() {},
                };
            },
            registerTreeDataProvider(viewId, treeDataProvider) {
                log(`stub: registerTreeDataProvider ${viewId}`);
                return new Disposable(() => {});
            },
            createTreeView(viewId, options) {
                log(`stub: createTreeView ${viewId}`);
                return {
                    onDidExpandElement: new EventEmitter().event,
                    onDidCollapseElement: new EventEmitter().event,
                    onDidChangeSelection: new EventEmitter().event,
                    onDidChangeVisibility: new EventEmitter().event,
                    visible: true,
                    selection: [],
                    reveal() { return Promise.resolve(); },
                    dispose() {},
                };
            },
            get activeTextEditor() { return undefined; },
            get visibleTextEditors() { return []; },
            get activeColorTheme() { return { kind: 2 }; }, // Dark
            onDidChangeActiveTextEditor: new EventEmitter().event,
            onDidChangeVisibleTextEditors: new EventEmitter().event,
            onDidChangeActiveColorTheme: new EventEmitter().event,
            get tabGroups() {
                return { all: [], activeTabGroup: { tabs: [], isActive: true, viewColumn: 1 }, onDidChangeTabGroups: new EventEmitter().event, onDidChangeTabs: new EventEmitter().event, close: () => Promise.resolve() };
            },
        },

        // --- Namespace: workspace ---
        workspace: {
            getConfiguration(section) {
                return _createConfigProxy(section);
            },
            workspaceFolders: [],
            name: undefined,
            rootPath: undefined,
            fs: {
                readFile: (uri) => require('node:fs/promises').readFile(uri.fsPath),
                writeFile: (uri, content) => require('node:fs/promises').writeFile(uri.fsPath, content),
                stat: (uri) => require('node:fs/promises').stat(uri.fsPath).then(s => ({ type: s.isDirectory() ? 2 : 1, size: s.size, ctime: s.ctimeMs, mtime: s.mtimeMs })),
                readDirectory: (uri) => require('node:fs/promises').readdir(uri.fsPath, { withFileTypes: true }).then(ents => ents.map(e => [e.name, e.isDirectory() ? 2 : 1])),
                createDirectory: (uri) => require('node:fs/promises').mkdir(uri.fsPath, { recursive: true }),
                delete: (uri) => require('node:fs/promises').rm(uri.fsPath, { force: true }),
                rename: (src, dst) => require('node:fs/promises').rename(src.fsPath, dst.fsPath),
                copy: (src, dst) => require('node:fs/promises').copyFile(src.fsPath, dst.fsPath),
            },
            onDidChangeConfiguration: new EventEmitter().event,
            onDidChangeWorkspaceFolders: new EventEmitter().event,
            findFiles(include, exclude, maxResults) {
                log('stub: findFiles');
                return Promise.resolve([]);
            },
            applyEdit(edit) {
                log('stub: applyEdit');
                return Promise.resolve(true);
            },
            openTextDocument(uriOrPath) {
                log('stub: openTextDocument');
                return Promise.resolve({ uri: typeof uriOrPath === 'string' ? Uri.file(uriOrPath) : uriOrPath, getText: () => '', lineCount: 0 });
            },
            registerTextDocumentContentProvider(scheme, provider) {
                log(`stub: registerTextDocumentContentProvider ${scheme}`);
                return new Disposable(() => {});
            },
        },

        // --- Namespace: env ---
        env: {
            appName: 'SAO AI Editor',
            appRoot: process.cwd(),
            language: 'en',
            machineId: 'node-ext-host',
            sessionId: `session-${Date.now()}`,
            uriScheme: 'vscode',
            clipboard: {
                readText: () => Promise.resolve(''),
                writeText: () => Promise.resolve(),
            },
            openExternal(uri) {
                log(`stub: openExternal ${uri}`);
                return Promise.resolve(true);
            },
            get shell() { return process.platform === 'win32' ? 'powershell.exe' : '/bin/bash'; },
            get uiKind() { return 1; }, // Desktop
            createTelemetryLogger() {
                return { logUsage() {}, logError() {}, dispose() {} };
            },
        },

        // --- Namespace: extensions ---
        extensions: {
            getExtension(id) {
                const ext = _extensions.get(id);
                if (!ext) return undefined;
                return { id, extensionUri: Uri.file(ext.context.extensionPath), extensionPath: ext.context.extensionPath, isActive: true, packageJSON: ext.desc.manifest || {}, exports: ext.module?.exports, extensionKind: 2, activate: () => Promise.resolve(ext.module?.exports) };
            },
            get all() {
                return [..._extensions.entries()].map(([id, e]) => ({ id, extensionPath: e.context.extensionPath, isActive: true, packageJSON: e.desc.manifest || {}, exports: e.module?.exports }));
            },
            onDidChange: new EventEmitter().event,
        },

        // --- Namespace: languages (stubs) ---
        languages: {
            registerHoverProvider() { log('stub: registerHoverProvider'); return new Disposable(() => {}); },
            registerCompletionItemProvider() { log('stub: registerCompletionItemProvider'); return new Disposable(() => {}); },
            registerDefinitionProvider() { log('stub: registerDefinitionProvider'); return new Disposable(() => {}); },
            registerCodeActionsProvider() { log('stub: registerCodeActionsProvider'); return new Disposable(() => {}); },
            registerCodeLensProvider() { log('stub: registerCodeLensProvider'); return new Disposable(() => {}); },
            createDiagnosticCollection(name) { return { name, set() {}, delete() {}, clear() {}, forEach() {}, get() {}, has() { return false; }, dispose() {} }; },
            registerDocumentFormattingEditProvider() { return new Disposable(() => {}); },
            registerDocumentLinkProvider() { return new Disposable(() => {}); },
            registerInlineCompletionItemProvider() { return new Disposable(() => {}); },
            getLanguages() { return Promise.resolve([]); },
            match(selector, doc) { return 0; },
            onDidChangeDiagnostics: new EventEmitter().event,
            getDiagnostics() { return []; },
        },

        // --- Namespace: lm (Language Models) ---
        lm: {
            selectChatModels(selector) {
                log('stub: lm.selectChatModels');
                return Promise.resolve([]);
            },
            registerTool(name, tool) {
                log(`stub: lm.registerTool ${name}`);
                return new Disposable(() => {});
            },
            onDidChangeChatModels: new EventEmitter().event,
        },

        // --- Namespace: chat ---
        chat: {
            createChatParticipant(id, handler) {
                log(`stub: chat.createChatParticipant ${id}`);
                return {
                    id,
                    iconPath: undefined,
                    onDidReceiveFeedback: new EventEmitter().event,
                    requestHandler: handler,
                    dispose() {},
                };
            },
        },

        // --- Namespace: authentication ---
        authentication: {
            getSession(providerId, scopes, options) {
                log(`stub: authentication.getSession ${providerId}`);
                return Promise.resolve(undefined);
            },
            registerAuthenticationProvider(id, label, provider) {
                log(`stub: registerAuthenticationProvider ${id}`);
                return new Disposable(() => {});
            },
            onDidChangeSessions: new EventEmitter().event,
        },

        // --- Namespace: debug (stubs) ---
        debug: {
            registerDebugConfigurationProvider() { return new Disposable(() => {}); },
            registerDebugAdapterDescriptorFactory() { return new Disposable(() => {}); },
            startDebugging() { log('stub: debug.startDebugging'); return Promise.resolve(false); },
            get activeDebugSession() { return undefined; },
            get breakpoints() { return []; },
            onDidChangeActiveDebugSession: new EventEmitter().event,
            onDidStartDebugSession: new EventEmitter().event,
            onDidTerminateDebugSession: new EventEmitter().event,
            onDidChangeBreakpoints: new EventEmitter().event,
        },

        // --- Namespace: tasks (stubs) ---
        tasks: {
            registerTaskProvider() { log('stub: tasks.registerTaskProvider'); return new Disposable(() => {}); },
            fetchTasks() { return Promise.resolve([]); },
            executeTask() { log('stub: executeTask'); return Promise.resolve({ terminate() {} }); },
            onDidStartTask: new EventEmitter().event,
            onDidEndTask: new EventEmitter().event,
            taskExecutions: [],
        },

        // --- Types used by some extensions ---
        ThemeIcon: class { constructor(id, color) { this.id = id; this.color = color; } },
        ThemeColor: class { constructor(id) { this.id = id; } },
        MarkdownString: class {
            constructor(value = '', supportThemeIcons = false) {
                this.value = value; this.isTrusted = false; this.supportThemeIcons = supportThemeIcons; this.supportHtml = false;
            }
            appendText(v) { this.value += v; return this; }
            appendMarkdown(v) { this.value += v; return this; }
            appendCodeblock(code, lang) { this.value += `\n\`\`\`${lang || ''}\n${code}\n\`\`\`\n`; return this; }
        },
        TreeItem: class {
            constructor(labelOrUri, collapsibleState) {
                if (typeof labelOrUri === 'string') { this.label = labelOrUri; } else { this.resourceUri = labelOrUri; }
                this.collapsibleState = collapsibleState ?? 0;
            }
        },
        CompletionItem: class { constructor(label, kind) { this.label = label; this.kind = kind; } },
        CompletionList: class { constructor(items, isIncomplete) { this.items = items || []; this.isIncomplete = !!isIncomplete; } },
        CodeAction: class { constructor(title, kind) { this.title = title; this.kind = kind; } },
        CodeActionKind: { QuickFix: 'quickfix', Refactor: 'refactor', Source: 'source', Empty: '' },
        Hover: class { constructor(contents, range) { this.contents = Array.isArray(contents) ? contents : [contents]; this.range = range; } },
        DocumentLink: class { constructor(range, target) { this.range = range; this.target = target; } },
        TextEdit: class { static replace(range, text) { return { range, newText: text }; }; static insert(pos, text) { return { range: new Range(pos, pos), newText: text }; }; static delete(range) { return { range, newText: '' }; } },
        WorkspaceEdit: class { constructor() { this._edits = []; } replace(uri, range, text) { this._edits.push({ uri, range, text }); } insert(uri, pos, text) { this._edits.push({ uri, range: new Range(pos, pos), text }); } delete(uri, range) { this._edits.push({ uri, range, text: '' }); } set(uri, edits) { for (const e of edits) this._edits.push({ uri, ...e }); } },
        RelativePattern: class { constructor(base, pattern) { this.base = base; this.pattern = pattern; } },
        FileType: { Unknown: 0, File: 1, Directory: 2, SymbolicLink: 64 },
        EndOfLine: { LF: 1, CRLF: 2 },
        TextEditorRevealType: { Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3 },

        // Placeholder for LanguageModelChatMessage
        LanguageModelChatMessage: class {
            constructor(role, content, name) { this.role = role; this.content = content; this.name = name; }
            static User(content, name) { return new vscode.LanguageModelChatMessage(1, content, name); }
            static Assistant(content, name) { return new vscode.LanguageModelChatMessage(2, content, name); }
        },
    };

    return { vscode, context };
}

// -------------------------------------------------------------------------
// Configuration proxy (returns defaults / empty values)
// -------------------------------------------------------------------------
function _createConfigProxy(section) {
    const _data = {};
    return {
        get(key, defaultValue) { return key in _data ? _data[key] : defaultValue; },
        has(key) { return key in _data; },
        inspect(key) { return { key: `${section}.${key}` }; },
        update(key, value, configTarget, overrideInLanguage) {
            _data[key] = value;
            return Promise.resolve();
        },
    };
}

// -------------------------------------------------------------------------
// Extension loader
// -------------------------------------------------------------------------
async function activateExtension(msg) {
    const { extensionPath, extensionId, manifest } = msg;
    if (_extensions.has(extensionId)) {
        send({ type: 'activated', extensionId, ok: true, already: true });
        return;
    }

    const pkgPath = manifest?.main
        ? path.resolve(extensionPath, manifest.main)
        : path.join(extensionPath, 'extension.js');

    log(`activating ${extensionId} from ${pkgPath}`);

    const { vscode, context } = buildVscodeModule(
        { extensionId, manifest },
        extensionPath,
    );

    // Intercept require('vscode')
    const origResolve = Module._resolveFilename;
    Module._resolveFilename = function (request, parent, isMain, options) {
        if (request === 'vscode') return 'vscode';
        return origResolve.call(this, request, parent, isMain, options);
    };
    const origLoad = Module._cache['vscode'];
    const fakeModule = new Module('vscode');
    fakeModule.exports = vscode;
    fakeModule.loaded = true;
    Module._cache['vscode'] = fakeModule;

    try {
        // Clear any previous cache of the extension module itself
        const resolved = require.resolve(pkgPath);
        delete require.cache[resolved];

        const extModule = require(pkgPath);
        const deactivateFn = extModule.deactivate || null;

        if (typeof extModule.activate === 'function') {
            const result = extModule.activate(context);
            if (result && typeof result.then === 'function') {
                await result;
            }
        }

        _extensions.set(extensionId, {
            desc: { extensionId, manifest },
            module: extModule,
            context,
            deactivate: deactivateFn,
            vscode,
        });

        // Resolve any pending webview view providers that were registered
        // during activate(). For each, resolve immediately so Python knows.
        for (const [viewType] of _webviewViewProviders) {
            resolveWebviewView(viewType);
        }

        send({ type: 'activated', extensionId, ok: true });
    } catch (err) {
        log(`activation failed for ${extensionId}: ${err.stack || err.message}`);
        send({ type: 'error', extensionId, error: err.message || String(err) });
        send({ type: 'activated', extensionId, ok: false, error: err.message });
    } finally {
        // Restore module resolution
        Module._resolveFilename = origResolve;
        if (origLoad) Module._cache['vscode'] = origLoad;
        else delete Module._cache['vscode'];
    }
}

// -------------------------------------------------------------------------
// Resolve a webview view (call provider.resolveWebviewView)
// -------------------------------------------------------------------------
function resolveWebviewView(viewType, state) {
    const reg = _webviewViewProviders.get(viewType);
    if (!reg) {
        log(`no provider for viewType=${viewType}`);
        return;
    }
    const viewId = `view-${_nextViewHandle++}`;
    const view = new WebviewView(viewId, viewType);
    _webviewViews.set(viewId, view);

    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const result = reg.provider.resolveWebviewView(view, { state: state ?? null }, token);
        if (result && typeof result.then === 'function') {
            result.catch(err => {
                log(`resolveWebviewView error for ${viewType}: ${err.message}`);
                send({ type: 'error', extensionId: viewType, error: err.message });
            });
        }
    } catch (err) {
        log(`resolveWebviewView error for ${viewType}: ${err.message}`);
        send({ type: 'error', extensionId: viewType, error: err.message });
    }
}

// -------------------------------------------------------------------------
// Deactivate an extension
// -------------------------------------------------------------------------
async function deactivateExtension(extensionId) {
    const ext = _extensions.get(extensionId);
    if (!ext) return;

    try {
        if (typeof ext.deactivate === 'function') {
            const result = ext.deactivate();
            if (result && typeof result.then === 'function') await result;
        }
        // Dispose subscriptions
        if (ext.context?.subscriptions) {
            for (const sub of ext.context.subscriptions) {
                try { sub.dispose?.(); } catch {}
            }
        }
    } catch (err) {
        log(`deactivate error for ${extensionId}: ${err.message}`);
    }
    _extensions.delete(extensionId);
    log(`deactivated ${extensionId}`);
}

// -------------------------------------------------------------------------
// Handle webview message from Python -> extension
// -------------------------------------------------------------------------
function handleWebviewMessage(viewId, message) {
    const view = _webviewViews.get(viewId);
    if (!view) {
        // Try matching by viewType
        for (const [id, v] of _webviewViews) {
            if (v.viewType === viewId) {
                v.webview._onDidReceiveMessage.fire(message);
                return;
            }
        }
        log(`webview_message: unknown viewId=${viewId}`);
        return;
    }
    view.webview._onDidReceiveMessage.fire(message);
}

// -------------------------------------------------------------------------
// Execute a registered command
// -------------------------------------------------------------------------
async function executeCommand(commandId, args) {
    const handler = _commands.get(commandId);
    if (!handler) {
        log(`command not found: ${commandId}`);
        send({ type: 'error', extensionId: '', error: `command not found: ${commandId}` });
        return;
    }
    try {
        const result = handler(...(args || []));
        if (result && typeof result.then === 'function') await result;
    } catch (err) {
        log(`command error ${commandId}: ${err.message}`);
        send({ type: 'error', extensionId: '', error: `command ${commandId}: ${err.message}` });
    }
}

// -------------------------------------------------------------------------
// Shutdown — deactivate all and exit
// -------------------------------------------------------------------------
async function shutdown() {
    log('shutting down...');
    for (const [id] of _extensions) {
        await deactivateExtension(id);
    }
    log('shutdown complete');
    process.exit(0);
}

// -------------------------------------------------------------------------
// Message dispatcher
// -------------------------------------------------------------------------
async function handleMessage(msg) {
    switch (msg.type) {
        case 'activate':
            await activateExtension(msg);
            break;
        case 'deactivate':
            await deactivateExtension(msg.extensionId);
            break;
        case 'webview_message':
            handleWebviewMessage(msg.viewId, msg.message);
            break;
        case 'resolve_webview_view':
            resolveWebviewView(msg.viewType, msg.state);
            break;
        case 'command':
            await executeCommand(msg.commandId, msg.args);
            break;
        case 'shutdown':
            await shutdown();
            break;
        default:
            log(`unknown message type: ${msg.type}`);
    }
}

// -------------------------------------------------------------------------
// stdin reader — one JSON line per message
// -------------------------------------------------------------------------
const rl = readline.createInterface({ input: process.stdin, terminal: false });

rl.on('line', (line) => {
    const trimmed = line.trim();
    if (!trimmed) return;
    let msg;
    try {
        msg = JSON.parse(trimmed);
    } catch (err) {
        log(`invalid JSON from parent: ${err.message}`);
        return;
    }
    handleMessage(msg).catch(err => {
        log(`unhandled error in handleMessage: ${err.stack || err.message}`);
        send({ type: 'error', extensionId: msg.extensionId || '', error: err.message });
    });
});

rl.on('close', () => {
    log('stdin closed, exiting');
    process.exit(0);
});

process.on('uncaughtException', (err) => {
    log(`uncaughtException: ${err.stack || err.message}`);
    send({ type: 'error', extensionId: '', error: `uncaught: ${err.message}` });
});

process.on('unhandledRejection', (reason) => {
    const msg = reason instanceof Error ? reason.message : String(reason);
    log(`unhandledRejection: ${msg}`);
    send({ type: 'error', extensionId: '', error: `unhandledRejection: ${msg}` });
});

log('node extension host started, waiting for messages...');
