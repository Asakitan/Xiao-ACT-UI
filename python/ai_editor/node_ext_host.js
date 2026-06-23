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
const fsp = require('node:fs/promises');

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

class DocumentHighlight {
    constructor(range, kind) {
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        this.kind = kind === undefined || kind === null ? 0 : Number(kind);
    }
}

class DataTransferFile {
    constructor(name, uri, data) {
        this.name = name === undefined || name === null ? '' : String(name);
        this.uri = uri ? _uriFromPayload(uri) : undefined;
        this._data = data;
    }
    data() {
        if (this._data instanceof Uint8Array) return Promise.resolve(this._data);
        if (Array.isArray(this._data)) return Promise.resolve(Uint8Array.from(this._data));
        if (Buffer.isBuffer(this._data)) return Promise.resolve(new Uint8Array(this._data));
        return Promise.resolve(new TextEncoder().encode(String(this._data ?? '')));
    }
}

class DataTransferItem {
    constructor(value) {
        this.value = value;
    }
    asString() {
        if (this.value === undefined || this.value === null) return Promise.resolve('');
        if (typeof this.value === 'string') return Promise.resolve(this.value);
        if (Buffer.isBuffer(this.value)) return Promise.resolve(this.value.toString('utf8'));
        if (this.value instanceof Uint8Array) return Promise.resolve(new TextDecoder().decode(this.value));
        if (typeof this.value === 'object') {
            try { return Promise.resolve(JSON.stringify(_serializeLanguageValue(this.value))); }
            catch { return Promise.resolve(String(this.value)); }
        }
        return Promise.resolve(String(this.value));
    }
    asFile() {
        if (this.value instanceof DataTransferFile) return this.value;
        if (this.value && typeof this.value === 'object'
            && (this.value.name !== undefined || this.value.uri !== undefined || this.value.data !== undefined)) {
            return new DataTransferFile(
                this.value.name || this.value.fileName || '',
                this.value.uri || this.value.path,
                this.value.data || this.value.contents || []);
        }
        return undefined;
    }
}

class DataTransfer {
    constructor(entries) {
        this._items = new Map();
        if (entries instanceof DataTransfer) {
            for (const [mime, item] of entries) this.set(mime, item);
        } else if (entries && typeof entries === 'object' && !Array.isArray(entries)) {
            for (const [mime, item] of Object.entries(entries)) this.set(mime, item);
        } else if (Array.isArray(entries)) {
            for (const [mime, item] of entries) this.set(mime, item);
        }
    }
    _key(mimeType) { return String(mimeType || '').toLowerCase(); }
    get(mimeType) { return this._items.get(this._key(mimeType)); }
    set(mimeType, value) {
        const key = this._key(mimeType);
        if (!key) return;
        this._items.set(key, value instanceof DataTransferItem ? value : new DataTransferItem(value));
    }
    delete(mimeType) { this._items.delete(this._key(mimeType)); }
    has(mimeType) { return this._items.has(this._key(mimeType)); }
    forEach(callbackfn, thisArg) {
        for (const [mime, item] of this._items) {
            callbackfn.call(thisArg, item, mime, this);
        }
    }
    [Symbol.iterator]() { return this._items[Symbol.iterator](); }
    toJSON() {
        const result = {};
        for (const [mime, item] of this._items) {
            result[mime] = _serializeLanguageValue(item.value);
        }
        return result;
    }
}

class DocumentDropOrPasteEditKind {
    constructor(value) {
        this.value = value === undefined || value === null ? '' : String(value);
    }
    append(...parts) {
        const suffix = parts.filter(part => part !== undefined && part !== null && String(part))
            .map(part => String(part).replace(/^\.+|\.+$/g, ''))
            .join('.');
        return new DocumentDropOrPasteEditKind([this.value, suffix].filter(Boolean).join('.'));
    }
    intersects(other) {
        const otherKind = _dropOrPasteKindFromPayload(other);
        return this.contains(otherKind) || otherKind.contains(this);
    }
    contains(other) {
        const otherValue = _dropOrPasteKindFromPayload(other).value;
        if (!this.value) return !otherValue;
        return otherValue === this.value || otherValue.startsWith(`${this.value}.`);
    }
    toString() { return this.value; }
}
DocumentDropOrPasteEditKind.Empty = new DocumentDropOrPasteEditKind('');
DocumentDropOrPasteEditKind.Text = new DocumentDropOrPasteEditKind('text');
DocumentDropOrPasteEditKind.TextUpdateImports = new DocumentDropOrPasteEditKind('text.updateImports');

class DocumentDropEdit {
    constructor(insertText, title, kind) {
        this.insertText = insertText;
        this.title = title === undefined || title === null ? undefined : String(title);
        this.kind = kind === undefined || kind === null ? undefined : _dropOrPasteKindFromPayload(kind);
        this.yieldTo = undefined;
        this.additionalEdit = undefined;
    }
}

class DocumentPasteEdit {
    constructor(insertText, title, kind) {
        this.insertText = insertText;
        this.title = title === undefined || title === null ? '' : String(title);
        this.kind = _dropOrPasteKindFromPayload(kind);
        this.additionalEdit = undefined;
        this.yieldTo = undefined;
    }
}

class SymbolInformation {
    constructor(name, kind, containerOrRange, locationOrUri, containerName) {
        this.name = name === undefined || name === null ? '' : String(name);
        this.kind = Number(kind || 0);
        this.tags = undefined;
        if (locationOrUri instanceof Location) {
            this.containerName = containerOrRange === undefined || containerOrRange === null
                ? ''
                : String(containerOrRange);
            this.location = locationOrUri;
        } else if (containerOrRange instanceof Range) {
            this.containerName = containerName === undefined || containerName === null
                ? ''
                : String(containerName);
            this.location = new Location(locationOrUri || Uri.file(''), containerOrRange);
        } else {
            this.containerName = containerOrRange === undefined || containerOrRange === null
                ? ''
                : String(containerOrRange);
            this.location = locationOrUri instanceof Location
                ? locationOrUri
                : new Location(Uri.file(''), new Range(0, 0, 0, 0));
        }
    }
}

class Color {
    constructor(red, green, blue, alpha) {
        this.red = _clampColorComponent(red, 0);
        this.green = _clampColorComponent(green, 0);
        this.blue = _clampColorComponent(blue, 0);
        this.alpha = _clampColorComponent(alpha, 1);
    }
}

class ColorInformation {
    constructor(range, color) {
        this.range = range;
        this.color = color instanceof Color ? color : _colorFromPayload(color);
    }
}

class ColorPresentation {
    constructor(label) {
        this.label = label === undefined || label === null ? '' : String(label);
        this.textEdit = undefined;
        this.additionalTextEdits = undefined;
    }
}

class FoldingRange {
    constructor(start, end, kind) {
        this.start = Math.max(0, Number(start || 0));
        this.end = Math.max(0, Number(end || 0));
        this.kind = kind;
    }
}

class SelectionRange {
    constructor(range, parent) {
        this.range = range;
        this.parent = parent;
    }
}

class CallHierarchyItem {
    constructor(kind, name, detail, uri, range, selectionRange) {
        this.kind = Number(kind || 0);
        this.name = name === undefined || name === null ? '' : String(name);
        this.detail = detail === undefined || detail === null ? '' : String(detail);
        this.uri = uri instanceof Uri ? uri : _uriFromPayload(uri || '');
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        this.selectionRange = selectionRange instanceof Range ? selectionRange : _rangeFromPayload(selectionRange);
        this.tags = undefined;
    }
}

class CallHierarchyIncomingCall {
    constructor(item, fromRanges) {
        this.from = item;
        this.fromRanges = Array.isArray(fromRanges) ? fromRanges : [];
    }
}

class CallHierarchyOutgoingCall {
    constructor(item, fromRanges) {
        this.to = item;
        this.fromRanges = Array.isArray(fromRanges) ? fromRanges : [];
    }
}

class TypeHierarchyItem {
    constructor(kind, name, detail, uri, range, selectionRange) {
        this.kind = Number(kind || 0);
        this.name = name === undefined || name === null ? '' : String(name);
        this.detail = detail === undefined || detail === null ? '' : String(detail);
        this.uri = uri instanceof Uri ? uri : _uriFromPayload(uri || '');
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        this.selectionRange = selectionRange instanceof Range ? selectionRange : _rangeFromPayload(selectionRange);
        this.tags = undefined;
    }
}

// -------------------------------------------------------------------------
// Semantic tokens
// -------------------------------------------------------------------------
class SemanticTokensLegend {
    constructor(tokenTypes, tokenModifiers) {
        this.tokenTypes = Array.isArray(tokenTypes) ? tokenTypes.map(String) : [];
        this.tokenModifiers = Array.isArray(tokenModifiers) ? tokenModifiers.map(String) : [];
    }
}

class SemanticTokens {
    constructor(data, resultId) {
        this.data = data instanceof Uint32Array ? data : Uint32Array.from(Array.isArray(data) ? data : []);
        this.resultId = resultId;
    }
}

class SemanticTokensEdit {
    constructor(start, deleteCount, data) {
        this.start = Math.max(0, Number(start || 0));
        this.deleteCount = Math.max(0, Number(deleteCount || 0));
        this.data = data === undefined || data === null
            ? undefined
            : (data instanceof Uint32Array ? data : Uint32Array.from(Array.isArray(data) ? data : []));
    }
}

class SemanticTokensEdits {
    constructor(edits, resultId) {
        this.edits = Array.isArray(edits) ? edits : [];
        this.resultId = resultId;
    }
}

class SemanticTokensBuilder {
    constructor(legend) {
        this._legend = legend instanceof SemanticTokensLegend
            ? legend
            : new SemanticTokensLegend(legend?.tokenTypes || [], legend?.tokenModifiers || []);
        this._tokens = [];
    }
    push(...args) {
        let line, char, length, tokenType, tokenModifiers;
        if (args[0] instanceof Range || (args[0] && typeof args[0] === 'object' && args[0].start)) {
            const range = args[0] instanceof Range ? args[0] : _rangeFromPayload(args[0]);
            if (Number(range.start.line || 0) !== Number(range.end.line || 0)) return;
            line = Number(range.start.line || 0);
            char = Number(range.start.character || 0);
            length = Math.max(0, Number(range.end.character || 0) - char);
            tokenType = args[1];
            tokenModifiers = args[2];
        } else {
            line = Number(args[0] || 0);
            char = Number(args[1] || 0);
            length = Number(args[2] || 0);
            tokenType = args[3];
            tokenModifiers = args[4];
        }
        if (!Number.isFinite(length) || length <= 0) return;
        this._tokens.push([
            Math.max(0, line),
            Math.max(0, char),
            Math.max(0, length),
            this._tokenTypeIndex(tokenType),
            this._tokenModifierBits(tokenModifiers),
        ]);
    }
    _tokenTypeIndex(tokenType) {
        if (typeof tokenType === 'string') {
            let index = this._legend.tokenTypes.indexOf(tokenType);
            if (index < 0) {
                this._legend.tokenTypes.push(tokenType);
                index = this._legend.tokenTypes.length - 1;
            }
            return index;
        }
        const n = Number(tokenType || 0);
        return Number.isFinite(n) ? Math.max(0, n) : 0;
    }
    _tokenModifierBits(tokenModifiers) {
        if (tokenModifiers === undefined || tokenModifiers === null) return 0;
        const modifiers = typeof tokenModifiers === 'string' ? [tokenModifiers] : tokenModifiers;
        if (Array.isArray(modifiers)) {
            let bits = 0;
            for (const modifier of modifiers) {
                const name = String(modifier);
                let index = this._legend.tokenModifiers.indexOf(name);
                if (index < 0) {
                    this._legend.tokenModifiers.push(name);
                    index = this._legend.tokenModifiers.length - 1;
                }
                bits |= (1 << index);
            }
            return bits;
        }
        const n = Number(tokenModifiers || 0);
        return Number.isFinite(n) ? Math.max(0, n) : 0;
    }
    build(resultId) {
        const data = [];
        let previousLine = 0;
        let previousChar = 0;
        this._tokens
            .slice()
            .sort((a, b) => a[0] - b[0] || a[1] - b[1])
            .forEach(([line, char, length, tokenType, tokenModifiers]) => {
                const deltaLine = line - previousLine;
                const deltaChar = deltaLine ? char : char - previousChar;
                data.push(
                    Math.max(0, deltaLine),
                    Math.max(0, deltaChar),
                    Math.max(0, length),
                    Math.max(0, tokenType),
                    Math.max(0, tokenModifiers)
                );
                previousLine = line;
                previousChar = char;
            });
        return new SemanticTokens(Uint32Array.from(data), resultId);
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
const _pythonCommandRequests = new Map(); // requestId -> { resolve, reject, timer }
const _webviewViewProviders = new Map(); // viewType -> { provider, options }
const _webviewViews = new Map();         // viewId -> WebviewView
const _treeDataProviders = new Map();    // viewId -> { provider, disposable? }
const _treeViews = new Map();            // viewId -> TreeView-like object
const _treeElementStores = new Map();    // viewId -> element handle store
const _outputChannels = new Map();       // name -> OutputChannel
const _languageProviders = [];           // { kind, selector, provider, triggers?, disposable }
let _nextLanguageProviderHandle = 1;
let _nextPythonCommandRequestHandle = 1;
const _languageDocumentTextCache = new Map(); // uri -> { version, text }
const _workspaceTextDocuments = new Map(); // uri -> TextDocument-like object
const _workspaceRoot = path.resolve(process.cwd());
const _workspaceName = path.basename(_workspaceRoot) || _workspaceRoot;
const _workspaceDefaultSkipDirs = new Set(['.git', 'node_modules', '__pycache__', '.venv', 'venv']);
const _workspaceSymbolCache = new Map(); // handle -> { provider, symbol }
let _nextWorkspaceSymbolHandle = 1;
const _hierarchyItemCache = new Map(); // handle -> { provider, item, kind }
let _nextHierarchyItemHandle = 1;
const _diagnosticCollections = new Map(); // name -> DiagnosticCollection
const _onDidChangeDiagnosticsEmitter = new EventEmitter();
const _debugAdapterFactories = new Map();   // type -> factory
const _debugConfigProviders = new Map();    // type -> provider
const _taskProviders = new Map();           // type -> provider
const _scmProviders = new Map();            // id -> SourceControl
const _onDidChangeConfigurationEmitter = new EventEmitter();

function _treeStore(viewId) {
    const key = String(viewId || '');
    let store = _treeElementStores.get(key);
    if (!store) {
        store = {
            next: 1,
            byHandle: new Map(),
            weak: new WeakMap(),
            primitive: new Map(),
        };
        _treeElementStores.set(key, store);
    }
    return store;
}

function _primitiveTreeKey(element) {
    return `${typeof element}:${String(element)}`;
}

function _treeElementHandle(viewId, element) {
    const store = _treeStore(viewId);
    if (element && (typeof element === 'object' || typeof element === 'function')) {
        const existing = store.weak.get(element);
        if (existing) return existing;
        const handle = `n${store.next++}`;
        store.weak.set(element, handle);
        store.byHandle.set(handle, element);
        return handle;
    }
    const primitiveKey = _primitiveTreeKey(element);
    const existing = store.primitive.get(primitiveKey);
    if (existing) return existing;
    const handle = `n${store.next++}`;
    store.primitive.set(primitiveKey, handle);
    store.byHandle.set(handle, element);
    return handle;
}

function _treeElementForHandle(viewId, handle) {
    if (!handle) return undefined;
    const store = _treeElementStores.get(String(viewId || ''));
    return store ? store.byHandle.get(String(handle)) : undefined;
}

function _treeElementLabel(element) {
    if (element === undefined || element === null) return '';
    if (typeof element === 'string') return element;
    if (typeof element === 'number' || typeof element === 'boolean') return String(element);
    if (typeof element === 'object') {
        return String(element.label || element.name || element.id || element.resourceUri || '');
    }
    return String(element);
}

function _serializeTreeElement(viewId, element) {
    if (element === undefined || element === null) return null;
    return {
        _nodeTreeViewId: String(viewId || ''),
        _nodeTreeHandle: _treeElementHandle(viewId, element),
        label: _treeElementLabel(element),
    };
}

function _serializeTreeUri(value) {
    if (!value) return undefined;
    if (typeof value === 'string') return value;
    if (value instanceof Uri) {
        return {
            scheme: value.scheme,
            authority: value.authority,
            path: value.path,
            query: value.query,
            fragment: value.fragment,
        };
    }
    if (typeof value === 'object' && value.scheme && value.path !== undefined) {
        return {
            scheme: value.scheme,
            authority: value.authority || '',
            path: value.path || '',
            query: value.query || '',
            fragment: value.fragment || '',
        };
    }
    return String(value);
}

function _serializeTreeText(value) {
    if (value === undefined || value === null) return undefined;
    if (typeof value === 'string' || typeof value === 'boolean') return value;
    if (typeof value === 'object' && value.value !== undefined) return String(value.value);
    return String(value);
}

function _serializeThemeColor(color) {
    if (!color) return undefined;
    if (typeof color === 'string') return color;
    if (typeof color === 'object') return color.id || color.value || String(color);
    return String(color);
}

function _serializeTreeIconPath(iconPath) {
    if (!iconPath) return undefined;
    if (typeof iconPath === 'string') return iconPath;
    if (iconPath instanceof Uri) return _serializeTreeUri(iconPath);
    if (typeof iconPath === 'object') {
        if (typeof iconPath.id === 'string') {
            const result = { id: iconPath.id };
            const color = _serializeThemeColor(iconPath.color);
            if (color) result.color = { id: color };
            return result;
        }
        const result = {};
        if (iconPath.light) result.light = _serializeTreeUri(iconPath.light);
        if (iconPath.dark) result.dark = _serializeTreeUri(iconPath.dark);
        if (iconPath.path) result.path = _serializeTreeUri(iconPath.path);
        if (Object.keys(result).length) return result;
    }
    return String(iconPath);
}

function _serializeTreeCommand(command, viewId) {
    if (!command || typeof command !== 'object') return undefined;
    const result = {
        command: String(command.command || command.id || ''),
        title: String(command.title || command.command || command.id || ''),
    };
    if (Array.isArray(command.arguments)) {
        result.arguments = command.arguments.map(arg => _serializeArgForPython(arg, viewId));
    }
    return result.command ? result : undefined;
}

function _serializeAccessibility(value) {
    if (!value || typeof value !== 'object') return undefined;
    const result = {};
    if (value.label) result.label = String(value.label);
    if (value.role) result.role = String(value.role);
    return Object.keys(result).length ? result : undefined;
}

function _serializeCheckboxState(value) {
    if (value === undefined || value === null) return undefined;
    if (typeof value === 'number') return value;
    if (typeof value === 'boolean') return value ? 1 : 0;
    if (typeof value === 'object') {
        const result = {};
        if (value.state !== undefined) result.state = value.state;
        if (value.tooltip !== undefined) result.tooltip = String(value.tooltip);
        const accessibility = _serializeAccessibility(value.accessibilityInformation);
        if (accessibility) result.accessibilityInformation = accessibility;
        return result;
    }
    return value;
}

function _serializeTreeItem(viewId, item, element) {
    if (!item || typeof item !== 'object') {
        return { label: _treeElementLabel(item ?? element), collapsibleState: 0 };
    }
    const result = {};
    if (item.id !== undefined) result.id = String(item.id);
    if (item.label !== undefined) result.label = item.label;
    else result.label = _treeElementLabel(element);
    if (item.description !== undefined) result.description = item.description;
    const tooltip = _serializeTreeText(item.tooltip);
    if (tooltip !== undefined) result.tooltip = tooltip;
    if (item.resourceUri) result.resourceUri = _serializeTreeUri(item.resourceUri);
    const iconPath = _serializeTreeIconPath(item.iconPath);
    if (iconPath !== undefined) result.iconPath = iconPath;
    const command = _serializeTreeCommand(item.command, viewId);
    if (command) result.command = command;
    if (item.contextValue !== undefined) result.contextValue = String(item.contextValue);
    if (item.collapsibleState !== undefined) result.collapsibleState = item.collapsibleState;
    const checkbox = _serializeCheckboxState(item.checkboxState);
    if (checkbox !== undefined) result.checkboxState = checkbox;
    const accessibility = _serializeAccessibility(item.accessibilityInformation);
    if (accessibility) result.accessibilityInformation = accessibility;
    return result;
}

function _serializeArgForPython(value, viewId) {
    if (value === undefined || value === null) return value;
    if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') return value;
    for (const [candidateViewId, store] of _treeElementStores) {
        if (value && (typeof value === 'object' || typeof value === 'function') && store.weak.has(value)) {
            return _serializeTreeElement(candidateViewId, value);
        }
    }
    if (value instanceof Uri) return _serializeTreeUri(value);
    if (Array.isArray(value)) return value.map(item => _serializeArgForPython(item, viewId));
    if (typeof value === 'object') {
        const result = {};
        for (const [key, item] of Object.entries(value)) {
            if (typeof item !== 'function') result[key] = _serializeArgForPython(item, viewId);
        }
        return result;
    }
    return String(value);
}

function _deserializeArgFromPython(value) {
    if (Array.isArray(value)) return value.map(_deserializeArgFromPython);
    if (value && typeof value === 'object') {
        if (value._nodeTreeViewId && value._nodeTreeHandle) {
            const element = _treeElementForHandle(value._nodeTreeViewId, value._nodeTreeHandle);
            if (element !== undefined) return element;
        }
        const result = {};
        for (const [key, item] of Object.entries(value)) result[key] = _deserializeArgFromPython(item);
        return result;
    }
    return value;
}

function _executePythonCommand(commandId, args) {
    const requestId = `pycmd-${_nextPythonCommandRequestHandle++}`;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            _pythonCommandRequests.delete(requestId);
            reject(new Error(`Python command timed out: ${commandId}`));
        }, 3000);
        _pythonCommandRequests.set(requestId, { resolve, reject, timer });
        send({
            type: 'execute_command',
            requestId,
            commandId,
            args: (args || []).map(item => _serializeArgForPython(item)),
        });
    });
}

function handleExecuteCommandResponse(msg) {
    const requestId = String(msg.requestId || '');
    const pending = _pythonCommandRequests.get(requestId);
    if (!pending) return;
    _pythonCommandRequests.delete(requestId);
    clearTimeout(pending.timer);
    if (msg.ok) {
        pending.resolve(_deserializeArgFromPython(msg.value));
    } else {
        pending.reject(new Error(msg.error || 'Python command failed'));
    }
}

function _serializeLanguagePosition(value) {
    return {
        line: Number(value?.line || 0),
        character: Number(value?.character || 0),
    };
}

function _serializeLanguageRange(value) {
    return {
        start: _serializeLanguagePosition(value?.start),
        end: _serializeLanguagePosition(value?.end),
    };
}

function _serializeLanguageUri(value) {
    if (value instanceof Uri) return value.toString();
    if (value && typeof value.toString === 'function' && value.scheme) return value.toString();
    return value === undefined || value === null ? value : String(value);
}

function _serializeLanguageValue(value) {
    if (value === undefined || value === null) return value;
    if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') return value;
    if (value instanceof Uri) return _serializeLanguageUri(value);
    if (value instanceof Position) return _serializeLanguagePosition(value);
    if (value instanceof Range) return _serializeLanguageRange(value);
    if (value instanceof DataTransfer) return value.toJSON();
    if (value instanceof DataTransferItem) return _serializeLanguageValue(value.value);
    if (value instanceof DocumentDropOrPasteEditKind) return { value: value.value };
    if (value instanceof RegExp) {
        return { source: value.source, flags: value.flags, pattern: String(value) };
    }
    if (value instanceof Location) {
        return {
            uri: _serializeLanguageUri(value.uri),
            range: _serializeLanguageRange(value.range),
        };
    }
    if (ArrayBuffer.isView(value)) return Array.from(value);
    if (Array.isArray(value)) return value.map(_serializeLanguageValue);
    if (typeof value === 'object') {
        const result = {};
        for (const [key, item] of Object.entries(value)) {
            if (typeof item !== 'function') result[key] = _serializeLanguageValue(item);
        }
        return result;
    }
    return String(value);
}

function _positionFromPayload(value) {
    if (value instanceof Position) return value;
    return new Position(Number(value?.line || 0), Number(value?.character || 0));
}

function _rangeFromPayload(value) {
    if (value instanceof Range) return value;
    return new Range(_positionFromPayload(value?.start), _positionFromPayload(value?.end));
}

function _dropOrPasteKindFromPayload(value) {
    if (value instanceof DocumentDropOrPasteEditKind) return value;
    if (value && typeof value === 'object' && value.value !== undefined) {
        return new DocumentDropOrPasteEditKind(value.value);
    }
    return new DocumentDropOrPasteEditKind(value === undefined || value === null ? '' : String(value));
}

function _dataTransferFromPayload(value) {
    return value instanceof DataTransfer ? value : new DataTransfer(value || {});
}

function _dataTransferToPayload(value) {
    return _serializeLanguageValue(_dataTransferFromPayload(value).toJSON());
}

function _dataTransferMatchesMime(dataTransfer, mimeType) {
    const requested = String(mimeType || '').toLowerCase();
    if (!requested) return false;
    if (requested === 'files') {
        for (const [, item] of dataTransfer) {
            if (item.asFile()) return true;
        }
        return false;
    }
    if (requested.endsWith('/*')) {
        const prefix = requested.slice(0, -1);
        for (const [mime] of dataTransfer) {
            if (mime.startsWith(prefix)) return true;
        }
        return false;
    }
    return dataTransfer.has(requested);
}

function _dataTransferMatchesMetadata(dataTransfer, metadata, key) {
    if (!metadata || typeof metadata !== 'object') return true;
    const mimeTypes = metadata[key];
    if (!Array.isArray(mimeTypes) || !mimeTypes.length) return true;
    return mimeTypes.some(mime => _dataTransferMatchesMime(dataTransfer, mime));
}

function _callHierarchyItemFromPayload(value) {
    if (value instanceof CallHierarchyItem) return value;
    const item = new CallHierarchyItem(
        value?.kind,
        value?.name,
        value?.detail,
        value?.uri,
        value?.range,
        value?.selectionRange,
    );
    if (value && value.tags !== undefined) item.tags = value.tags;
    return item;
}

function _typeHierarchyItemFromPayload(value) {
    if (value instanceof TypeHierarchyItem) return value;
    const item = new TypeHierarchyItem(
        value?.kind,
        value?.name,
        value?.detail,
        value?.uri,
        value?.range,
        value?.selectionRange,
    );
    if (value && value.tags !== undefined) item.tags = value.tags;
    return item;
}

function _clampColorComponent(value, fallback) {
    const number = Number(value);
    if (!Number.isFinite(number)) return fallback;
    return Math.max(0, Math.min(1, number));
}

function _colorFromPayload(value) {
    if (value instanceof Color) return value;
    return new Color(value?.red, value?.green, value?.blue, value?.alpha);
}

function _uriFromPayload(value) {
    if (value instanceof Uri) return value;
    if (typeof value === 'string' && value) return Uri.parse(value);
    if (value && typeof value === 'object') {
        if (value.scheme) {
            return new Uri(value.scheme, value.authority || '', value.path || '', value.query || '', value.fragment || '');
        }
        if (value.uri || value.path) return _uriFromPayload(value.uri || value.path);
    }
    return Uri.file('');
}

function _languageIdForUri(uri) {
    const ext = path.extname(uri?.path || '').toLowerCase();
    return ({
        '.py': 'python',
        '.js': 'javascript',
        '.jsx': 'javascriptreact',
        '.ts': 'typescript',
        '.tsx': 'typescriptreact',
        '.json': 'json',
        '.md': 'markdown',
        '.html': 'html',
        '.css': 'css',
        '.cs': 'csharp',
        '.xml': 'xml',
        '.yaml': 'yaml',
        '.yml': 'yaml',
    })[ext] || 'plaintext';
}

function _workspaceFolder() {
    return { uri: Uri.file(_workspaceRoot), name: _workspaceName, index: 0 };
}

function _pathFromUriLike(value) {
    if (!value) return '';
    if (value instanceof Uri) return value.fsPath;
    if (typeof value === 'string') return value;
    if (value.uri) return _pathFromUriLike(value.uri);
    if (value.fsPath) return String(value.fsPath);
    if (value.path) return String(value.path);
    return '';
}

function _workspaceUriFromInput(value) {
    if (value instanceof Uri) return value;
    if (typeof value === 'string') {
        if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(value)) return Uri.parse(value);
        return Uri.file(path.isAbsolute(value) ? value : path.join(_workspaceRoot, value));
    }
    if (value && typeof value === 'object') {
        if (value.scheme) return _uriFromPayload(value);
        if (value.uri || value.path || value.fsPath) {
            return _workspaceUriFromInput(value.uri || value.fsPath || value.path);
        }
    }
    return new Uri('untitled', '', '/Untitled-1', '', '');
}

function _workspaceRelativePath(value, includeWorkspaceFolder) {
    const rawPath = _pathFromUriLike(value);
    const absPath = path.resolve(rawPath || _workspaceRoot);
    let rel = path.relative(_workspaceRoot, absPath).replace(/\\/g, '/');
    if (!rel || rel.startsWith('..')) rel = absPath.replace(/\\/g, '/');
    return includeWorkspaceFolder ? `${_workspaceName}/${rel}` : rel;
}

function _workspacePatternText(pattern, fallback = '**/*') {
    if (pattern === undefined || pattern === null) return fallback;
    if (typeof pattern === 'string') return pattern.replace(/\\/g, '/');
    if (pattern && typeof pattern === 'object' && pattern.pattern !== undefined) {
        return String(pattern.pattern || fallback).replace(/\\/g, '/');
    }
    return String(pattern || fallback).replace(/\\/g, '/');
}

function _workspacePatternBase(pattern) {
    if (!pattern || typeof pattern !== 'object') return _workspaceRoot;
    const base = pattern.baseUri || pattern.base || pattern.uri;
    const basePath = _pathFromUriLike(base);
    if (!basePath) return _workspaceRoot;
    const resolved = path.resolve(basePath);
    const rel = path.relative(_workspaceRoot, resolved);
    return rel && (rel.startsWith('..') || path.isAbsolute(rel)) ? _workspaceRoot : resolved;
}

function _escapeRegexText(value) {
    return String(value).replace(/[\\^$+?.()|[\]{}]/g, '\\$&');
}

function _globToRegExp(pattern) {
    let glob = _workspacePatternText(pattern).replace(/^\.\//, '');
    if (!glob) glob = '**/*';
    let source = '^';
    for (let i = 0; i < glob.length; i += 1) {
        const ch = glob[i];
        if (ch === '*') {
            if (glob[i + 1] === '*') {
                i += 1;
                if (glob[i + 1] === '/') {
                    source += '(?:.*/)?';
                    i += 1;
                } else {
                    source += '.*';
                }
            } else {
                source += '[^/]*';
            }
        } else if (ch === '?') {
            source += '[^/]';
        } else if (ch === '{') {
            const end = glob.indexOf('}', i + 1);
            if (end > i) {
                const alternatives = glob.slice(i + 1, end).split(',')
                    .map(part => _escapeRegexText(part.trim()));
                source += '(?:' + alternatives.join('|') + ')';
                i = end;
            } else {
                source += '\\{';
            }
        } else if (ch === '/') {
            source += '/';
        } else {
            source += _escapeRegexText(ch);
        }
    }
    return new RegExp(source + '$');
}

function _workspaceDefaultSkipDir(name) {
    return _workspaceDefaultSkipDirs.has(name);
}

function _workspaceExcluded(rel, excludeRegexes) {
    if (!excludeRegexes.length) return false;
    return excludeRegexes.some(rx => rx.test(rel) || rx.test(rel + '/'));
}

async function _workspaceFindFiles(include, exclude, maxResults) {
    const base = _workspacePatternBase(include);
    const baseRelPrefix = path.relative(_workspaceRoot, base).replace(/\\/g, '/');
    const includeRegex = _globToRegExp(_workspacePatternText(include));
    const excludeRegexes = exclude === undefined || exclude === null
        ? []
        : [_globToRegExp(_workspacePatternText(exclude, ''))];
    const rawMax = Number(maxResults);
    const limit = Number.isFinite(rawMax) && rawMax > 0 ? rawMax : 5000;
    const results = [];
    async function walk(dir) {
        if (results.length >= limit) return;
        let entries = [];
        try { entries = await fsp.readdir(dir, { withFileTypes: true }); }
        catch { return; }
        for (const entry of entries) {
            if (results.length >= limit) break;
            const abs = path.join(dir, entry.name);
            const rel = path.relative(_workspaceRoot, abs).replace(/\\/g, '/');
            const matchRel = baseRelPrefix && !baseRelPrefix.startsWith('..')
                ? path.relative(base, abs).replace(/\\/g, '/')
                : rel;
            if (entry.isDirectory()) {
                if (_workspaceDefaultSkipDir(entry.name) || _workspaceExcluded(rel, excludeRegexes)) continue;
                await walk(abs);
            } else if (entry.isFile()) {
                if (_workspaceExcluded(rel, excludeRegexes)) continue;
                if (includeRegex.test(matchRel) || includeRegex.test(rel)) results.push(Uri.file(abs));
            }
        }
    }
    await walk(base);
    return results;
}

async function _workspaceOpenTextDocument(uriOrPath) {
    if (uriOrPath && typeof uriOrPath === 'object'
        && !(uriOrPath instanceof Uri)
        && (uriOrPath.content !== undefined || uriOrPath.language !== undefined)) {
        const uri = new Uri('untitled', '', '/Untitled-1', '', '');
        const doc = _createLanguageDocument({
            uri,
            text: String(uriOrPath.content || ''),
            languageId: uriOrPath.language || 'plaintext',
            version: Date.now(),
        });
        _workspaceTextDocuments.set(uri.toString(), doc);
        return doc;
    }
    const uri = _workspaceUriFromInput(uriOrPath);
    const text = uri.scheme === 'file' ? await fsp.readFile(uri.fsPath, 'utf8') : '';
    const doc = _createLanguageDocument({
        uri,
        text,
        languageId: _languageIdForUri(uri),
        version: Date.now(),
    });
    _workspaceTextDocuments.set(uri.toString(), doc);
    return doc;
}

function _workspaceEditEntries(edit) {
    if (!edit) return [];
    if (Array.isArray(edit)) return edit;
    if (Array.isArray(edit._edits)) return edit._edits;
    if (Array.isArray(edit.edits)) return edit.edits;
    return [];
}

function _workspaceEditText(entry) {
    if (!entry || typeof entry !== 'object') return '';
    if (entry.newText !== undefined) return String(entry.newText);
    if (entry.text !== undefined) return String(entry.text);
    return '';
}

async function _workspaceApplyEdit(edit) {
    const entries = _workspaceEditEntries(edit);
    if (!entries.length) return true;
    const grouped = new Map();
    for (const entry of entries) {
        const uri = _workspaceUriFromInput(entry && (entry.uri || entry.resource || entry.path));
        if (!uri || uri.scheme !== 'file' || !entry.range) return false;
        const key = uri.toString();
        if (!grouped.has(key)) grouped.set(key, { uri, edits: [] });
        grouped.get(key).edits.push({
            range: _rangeFromPayload(entry.range),
            text: _workspaceEditText(entry),
        });
    }
    try {
        for (const group of grouped.values()) {
            let text = '';
            try { text = await fsp.readFile(group.uri.fsPath, 'utf8'); }
            catch (err) {
                if (!err || err.code !== 'ENOENT') throw err;
            }
            const edits = group.edits.slice().sort((a, b) => {
                const ao = _offsetAt(text, a.range.start);
                const bo = _offsetAt(text, b.range.start);
                if (ao !== bo) return bo - ao;
                return _offsetAt(text, b.range.end) - _offsetAt(text, a.range.end);
            });
            for (const item of edits) {
                const start = _offsetAt(text, item.range.start);
                const end = _offsetAt(text, item.range.end);
                text = text.slice(0, start) + item.text + text.slice(end);
            }
            await fsp.mkdir(path.dirname(group.uri.fsPath), { recursive: true });
            await fsp.writeFile(group.uri.fsPath, text, 'utf8');
            const doc = _createLanguageDocument({
                uri: group.uri,
                text,
                languageId: _languageIdForUri(group.uri),
                version: Date.now(),
            });
            _workspaceTextDocuments.set(group.uri.toString(), doc);
        }
        return true;
    } catch (err) {
        log(`workspace.applyEdit failed: ${err && err.message || err}`);
        return false;
    }
}

function _offsetAt(text, position) {
    const targetLine = Math.max(0, Number(position?.line || 0));
    const targetCharacter = Math.max(0, Number(position?.character || 0));
    let line = 0;
    let character = 0;
    for (let i = 0; i < text.length; i += 1) {
        if (line === targetLine && character >= targetCharacter) return i;
        const ch = text[i];
        if (ch === '\r' || ch === '\n') {
            if (ch === '\r' && text[i + 1] === '\n') i += 1;
            line += 1;
            character = 0;
            if (line > targetLine) return i + 1;
        } else {
            character += 1;
        }
    }
    return text.length;
}

function _positionAt(text, offset) {
    const target = Math.max(0, Math.min(Number(offset || 0), text.length));
    let line = 0;
    let character = 0;
    for (let i = 0; i < target; i += 1) {
        const ch = text[i];
        if (ch === '\r' || ch === '\n') {
            if (ch === '\r' && text[i + 1] === '\n' && i + 1 < target) i += 1;
            line += 1;
            character = 0;
        } else {
            character += 1;
        }
    }
    return new Position(line, character);
}

function _createLanguageDocument(msg) {
    const uri = _uriFromPayload(msg.uri);
    const uriKey = uri.toString();
    const version = Number(msg.version || 1);
    let text = '';
    if (Object.prototype.hasOwnProperty.call(msg, 'text')) {
        text = String(msg.text ?? '');
        _languageDocumentTextCache.set(uriKey, { version, text });
    } else {
        const cached = _languageDocumentTextCache.get(uriKey);
        text = cached && cached.version === version ? cached.text : '';
    }
    const languageId = String(msg.languageId || _languageIdForUri(uri));
    return {
        uri,
        fileName: uri.scheme === 'file' ? uri.fsPath : uri.toString(),
        languageId,
        version,
        isDirty: false,
        isUntitled: uri.scheme === 'untitled',
        get lineCount() { return text.split(/\r\n|\r|\n/).length; },
        getText(range) {
            if (!range) return text;
            const normalized = _rangeFromPayload(range);
            return text.slice(
                _offsetAt(text, normalized.start),
                _offsetAt(text, normalized.end),
            );
        },
        lineAt(lineOrPosition) {
            const line = typeof lineOrPosition === 'number'
                ? lineOrPosition
                : Number(lineOrPosition?.line || 0);
            const lines = text.split(/\r\n|\r|\n/);
            const value = lines[Math.max(0, Math.min(line, lines.length - 1))] || '';
            return {
                lineNumber: line,
                text: value,
                range: new Range(line, 0, line, value.length),
                rangeIncludingLineBreak: new Range(line, 0, line, value.length),
                firstNonWhitespaceCharacterIndex: Math.max(0, value.search(/\S/)),
                isEmptyOrWhitespace: !/\S/.test(value),
            };
        },
        offsetAt(position) { return _offsetAt(text, _positionFromPayload(position)); },
        positionAt(offset) { return _positionAt(text, offset); },
        getWordRangeAtPosition() { return undefined; },
        save() { return Promise.resolve(true); },
    };
}

function _matchDocumentSelector(selector, document) {
    if (!selector || !document) return 0;
    const docLang = document.languageId || '';
    const docScheme = (document.uri && document.uri.scheme) || 'file';
    const selectors = Array.isArray(selector) ? selector : [selector];
    let best = 0;
    for (const sel of selectors) {
        let score = 0;
        if (typeof sel === 'string') {
            score = (sel === docLang) ? 10 : 0;
        } else if (typeof sel === 'object' && sel !== null) {
            const langMatch = !sel.language || sel.language === docLang || sel.language === '*';
            const schemeMatch = !sel.scheme || sel.scheme === docScheme || sel.scheme === '*';
            if (langMatch && schemeMatch) score = 10;
            if (sel.pattern) score = Math.max(score, 5);
        }
        best = Math.max(best, score);
    }
    return best;
}

function _subscribeTreeDataChanges(viewId, provider) {
    const event = provider && provider.onDidChangeTreeData;
    if (typeof event !== 'function') return undefined;
    try {
        return event((element) => {
            send({
                type: 'tree_data_changed',
                viewId,
                element: _serializeTreeElement(viewId, element),
            });
        });
    } catch (err) {
        log(`tree data change subscription failed for ${viewId}: ${err.message}`);
        return undefined;
    }
}

function registerTreeDataProviderInternal(viewId, treeDataProvider) {
    const normalized = String(viewId || '');
    if (!normalized || !treeDataProvider) return new Disposable(() => {});
    const previous = _treeDataProviders.get(normalized);
    try { previous?.disposable?.dispose?.(); } catch {}
    const disposable = _subscribeTreeDataChanges(normalized, treeDataProvider);
    _treeDataProviders.set(normalized, { provider: treeDataProvider, disposable });
    send({ type: 'tree_data_provider_registered', viewId: normalized });
    return new Disposable(() => {
        const current = _treeDataProviders.get(normalized);
        if (current?.provider === treeDataProvider) {
            try { current.disposable?.dispose?.(); } catch {}
            _treeDataProviders.delete(normalized);
            _treeElementStores.delete(normalized);
            send({ type: 'tree_data_provider_disposed', viewId: normalized });
        }
    });
}

function createTreeViewObject(viewId, treeDataProvider) {
    const normalized = String(viewId || '');
    const expandEmitter = new EventEmitter();
    const collapseEmitter = new EventEmitter();
    const selectionEmitter = new EventEmitter();
    const visibilityEmitter = new EventEmitter();
    const providerDisposable = treeDataProvider
        ? registerTreeDataProviderInternal(normalized, treeDataProvider)
        : undefined;
    const view = {
        id: normalized,
        visible: true,
        selection: [],
        onDidExpandElement: expandEmitter.event,
        onDidCollapseElement: collapseEmitter.event,
        onDidChangeSelection: selectionEmitter.event,
        onDidChangeVisibility: visibilityEmitter.event,
        reveal(element, options) {
            send({
                type: 'tree_view_reveal',
                viewId: normalized,
                element: _serializeTreeElement(normalized, element),
                options: options || {},
            });
            return Promise.resolve();
        },
        dispose() {
            try { providerDisposable?.dispose?.(); } catch {}
            _treeViews.delete(normalized);
            visibilityEmitter.fire({ visible: false });
        },
        _onDidExpandElement: expandEmitter,
        _onDidCollapseElement: collapseEmitter,
        _onDidChangeSelection: selectionEmitter,
        _onDidChangeVisibility: visibilityEmitter,
    };
    _treeViews.set(normalized, view);
    return view;
}

// -------------------------------------------------------------------------
// Build the mock vscode module for a given extension
// -------------------------------------------------------------------------
let _sbiCounter = 0;

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
        DocumentHighlight,
        SymbolInformation,
        CallHierarchyItem,
        CallHierarchyIncomingCall,
        CallHierarchyOutgoingCall,
        TypeHierarchyItem,
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
        DocumentHighlightKind: { Text: 0, Read: 1, Write: 2 },
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
                return _executePythonCommand(id, args);
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
                const align = typeof alignmentOrId === 'number' ? alignmentOrId : 2;
                const pri = typeof priorityOrAlignment === 'number' ? priorityOrAlignment :
                            (typeof priority === 'number' ? priority : 0);
                const id = 'sbi-node-' + (++_sbiCounter);
                const item = {
                    alignment: align, priority: pri, text: '', tooltip: '', color: '',
                    backgroundColor: undefined, command: undefined, name: '', _id: id,
                    show() { send({type:'status_bar_show', id, text:item.text, tooltip:item.tooltip,
                              command:item.command||'', alignment:align, priority:pri,
                              color:item.color||'', backgroundColor:item.backgroundColor||''}); },
                    hide() { send({type:'status_bar_hide', id}); },
                    dispose() { send({type:'status_bar_dispose', id}); },
                };
                return item;
            },
            registerTreeDataProvider(viewId, treeDataProvider) {
                log(`registerTreeDataProvider ${viewId}`);
                const d = registerTreeDataProviderInternal(viewId, treeDataProvider);
                subscriptions.push(d);
                return d;
            },
            createTreeView(viewId, options) {
                log(`createTreeView ${viewId}`);
                const view = createTreeViewObject(viewId, options?.treeDataProvider);
                const d = new Disposable(() => view.dispose());
                subscriptions.push(d);
                return view;
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
            get workspaceFolders() { return [_workspaceFolder()]; },
            get name() { return _workspaceName; },
            get rootPath() { return _workspaceRoot; },
            get textDocuments() { return Array.from(_workspaceTextDocuments.values()); },
            fs: {
                readFile: (uri) => fsp.readFile(uri.fsPath),
                writeFile: (uri, content) => fsp.writeFile(uri.fsPath, content),
                stat: (uri) => fsp.stat(uri.fsPath).then(s => ({ type: s.isDirectory() ? 2 : 1, size: s.size, ctime: s.ctimeMs, mtime: s.mtimeMs })),
                readDirectory: (uri) => fsp.readdir(uri.fsPath, { withFileTypes: true }).then(ents => ents.map(e => [e.name, e.isDirectory() ? 2 : 1])),
                createDirectory: (uri) => fsp.mkdir(uri.fsPath, { recursive: true }),
                delete: (uri) => fsp.rm(uri.fsPath, { force: true }),
                rename: (src, dst) => fsp.rename(src.fsPath, dst.fsPath),
                copy: (src, dst) => fsp.copyFile(src.fsPath, dst.fsPath),
            },
            onDidChangeConfiguration: _onDidChangeConfigurationEmitter.event,
            onDidChangeWorkspaceFolders: new EventEmitter().event,
            findFiles(include, exclude, maxResults) {
                return _workspaceFindFiles(include, exclude, maxResults);
            },
            applyEdit(edit) {
                return _workspaceApplyEdit(edit);
            },
            openTextDocument(uriOrPath) {
                return _workspaceOpenTextDocument(uriOrPath);
            },
            asRelativePath(pathOrUri, includeWorkspaceFolder) {
                return _workspaceRelativePath(pathOrUri, includeWorkspaceFolder);
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

        // --- Namespace: languages ---
        languages: (() => {
            const _registerLangProvider = (kind, selector, provider, extra) => {
                const entry = Object.assign({
                    handle: _nextLanguageProviderHandle++,
                    extensionId: extDesc.extensionId || '',
                    kind,
                    selector,
                    provider,
                }, extra || {});
                _languageProviders.push(entry);
                send({
                    type: 'language_provider_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    kind,
                    selector,
                    triggers: entry.triggers || [],
                    metadata: entry.metadata || null,
                });
                log(`register ${kind} provider: ${JSON.stringify(selector)}`);
                const d = new Disposable(() => {
                    const idx = _languageProviders.indexOf(entry);
                    if (idx >= 0) _languageProviders.splice(idx, 1);
                    send({
                        type: 'language_provider_disposed',
                        handle: entry.handle,
                        kind,
                        selector,
                    });
                });
                subscriptions.push(d);
                return d;
            };
            return {
                registerCompletionItemProvider(selector, provider, ...triggers) {
                    return _registerLangProvider('completion', selector, provider, { triggers });
                },
                registerHoverProvider(selector, provider) {
                    return _registerLangProvider('hover', selector, provider);
                },
                registerSignatureHelpProvider(selector, provider, ...metadata) {
                    if (metadata.length === 1 && metadata[0] && typeof metadata[0] === 'object' && !Array.isArray(metadata[0])) {
                        return _registerLangProvider('signatureHelp', selector, provider, {
                            metadata: metadata[0],
                            triggers: metadata[0].triggerCharacters || [],
                        });
                    }
                    return _registerLangProvider('signatureHelp', selector, provider, {
                        metadata: { triggerCharacters: metadata, retriggerCharacters: [] },
                        triggers: metadata,
                    });
                },
                registerDefinitionProvider(selector, provider) {
                    return _registerLangProvider('definition', selector, provider);
                },
                registerTypeDefinitionProvider(selector, provider) {
                    return _registerLangProvider('typeDefinition', selector, provider);
                },
                registerDeclarationProvider(selector, provider) {
                    return _registerLangProvider('declaration', selector, provider);
                },
                registerImplementationProvider(selector, provider) {
                    return _registerLangProvider('implementation', selector, provider);
                },
                registerReferenceProvider(selector, provider) {
                    return _registerLangProvider('references', selector, provider);
                },
                registerDocumentHighlightProvider(selector, provider) {
                    return _registerLangProvider('documentHighlight', selector, provider);
                },
                registerLinkedEditingRangeProvider(selector, provider) {
                    return _registerLangProvider('linkedEditing', selector, provider);
                },
                registerCallHierarchyProvider(selector, provider) {
                    return _registerLangProvider('callHierarchy', selector, provider);
                },
                registerTypeHierarchyProvider(selector, provider) {
                    return _registerLangProvider('typeHierarchy', selector, provider);
                },
                registerDocumentDropEditProvider(selector, provider, metadata) {
                    return _registerLangProvider('documentDrop', selector, provider, {
                        metadata: metadata || null,
                    });
                },
                registerDocumentPasteEditProvider(selector, provider, metadata) {
                    return _registerLangProvider('documentPaste', selector, provider, {
                        metadata: metadata || null,
                    });
                },
                registerRenameProvider(selector, provider) {
                    return _registerLangProvider('rename', selector, provider);
                },
                registerDocumentSymbolProvider(selector, provider) {
                    return _registerLangProvider('documentSymbol', selector, provider);
                },
                registerWorkspaceSymbolProvider(provider) {
                    return _registerLangProvider('workspaceSymbol', null, provider);
                },
                registerCodeActionsProvider(selector, provider, metadata) {
                    return _registerLangProvider('codeActions', selector, provider, { metadata });
                },
                registerCodeLensProvider(selector, provider) {
                    return _registerLangProvider('codeLens', selector, provider);
                },
                createDiagnosticCollection(name) {
                    const collName = name || `diag-${_diagnosticCollections.size}`;
                    const entries = new Map();
                    const collection = {
                        name: collName,
                        set(uri, diagnostics) {
                            if (diagnostics) {
                                entries.set(String(uri), diagnostics);
                            } else if (Array.isArray(uri)) {
                                for (const [u, d] of uri) entries.set(String(u), d);
                            }
                            _onDidChangeDiagnosticsEmitter.fire({ uris: [uri] });
                        },
                        delete(uri) { entries.delete(String(uri)); _onDidChangeDiagnosticsEmitter.fire({ uris: [uri] }); },
                        clear() { entries.clear(); _onDidChangeDiagnosticsEmitter.fire({ uris: [] }); },
                        forEach(cb) { entries.forEach((diags, uri) => cb(uri, diags, collection)); },
                        get(uri) { return entries.get(String(uri)); },
                        has(uri) { return entries.has(String(uri)); },
                        dispose() { entries.clear(); _diagnosticCollections.delete(collName); },
                    };
                    _diagnosticCollections.set(collName, collection);
                    return collection;
                },
                getDiagnostics(uri) {
                    if (uri) {
                        const key = String(uri);
                        for (const col of _diagnosticCollections.values()) {
                            const d = col.get(key);
                            if (d) return d;
                        }
                        return [];
                    }
                    const all = [];
                    for (const col of _diagnosticCollections.values()) {
                        col.forEach((u, diags) => all.push([u, diags]));
                    }
                    return all;
                },
                onDidChangeDiagnostics: _onDidChangeDiagnosticsEmitter.event,
                match(selector, document) {
                    return _matchDocumentSelector(selector, document);
                },
                registerDocumentFormattingEditProvider(selector, provider) {
                    return _registerLangProvider('formatting', selector, provider);
                },
                registerDocumentRangeFormattingEditProvider(selector, provider) {
                    return _registerLangProvider('rangeFormatting', selector, provider);
                },
                registerOnTypeFormattingEditProvider(selector, provider, firstTriggerCharacter, ...moreTriggerCharacters) {
                    const triggers = [firstTriggerCharacter, ...moreTriggerCharacters]
                        .filter(ch => ch !== undefined && ch !== null)
                        .map(ch => String(ch));
                    return _registerLangProvider('onTypeFormatting', selector, provider, {
                        triggers,
                        metadata: { triggerCharacters: triggers },
                    });
                },
                registerDocumentLinkProvider(selector, provider) {
                    return _registerLangProvider('documentLink', selector, provider);
                },
                registerInlayHintsProvider(selector, provider) {
                    return _registerLangProvider('inlayHint', selector, provider);
                },
                registerInlineCompletionItemProvider(selector, provider) {
                    return _registerLangProvider('inlineCompletion', selector, provider);
                },
                registerFoldingRangeProvider(selector, provider) {
                    return _registerLangProvider('foldingRange', selector, provider);
                },
                registerSelectionRangeProvider(selector, provider) {
                    return _registerLangProvider('selectionRange', selector, provider);
                },
                registerColorProvider(selector, provider) {
                    return _registerLangProvider('documentColor', selector, provider);
                },
                registerDocumentSemanticTokensProvider(selector, provider, legend) {
                    return _registerLangProvider('semanticTokens', selector, provider, {
                        metadata: legend || new SemanticTokensLegend([], []),
                    });
                },
                registerDocumentRangeSemanticTokensProvider(selector, provider, legend) {
                    return _registerLangProvider('semanticTokensRange', selector, provider, {
                        metadata: legend || new SemanticTokensLegend([], []),
                    });
                },
                getLanguages() { return Promise.resolve([]); },
            };
        })(),

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

        // --- Namespace: scm ---
        scm: {
            createSourceControl(id, label, rootUri) {
                log(`scm: createSourceControl "${id}" ("${label}")`);
                const groups = new Map();
                const sc = {
                    id,
                    label,
                    rootUri: rootUri || null,
                    inputBox: { value: '', placeholder: '' },
                    count: 0,
                    quickDiffProvider: undefined,
                    statusBarCommands: undefined,
                    createResourceGroup(groupId, groupLabel) {
                        const group = { id: groupId, label: groupLabel, resourceStates: [], hideWhenEmpty: false, dispose() { groups.delete(groupId); } };
                        groups.set(groupId, group);
                        return group;
                    },
                    dispose() {
                        groups.clear();
                        _scmProviders.delete(id);
                        log(`scm: disposed "${id}"`);
                    },
                };
                _scmProviders.set(id, sc);
                return sc;
            },
        },

        // --- Namespace: debug ---
        debug: (() => {
            const _onDidStartDebugSession = new EventEmitter();
            const _onDidTerminateDebugSession = new EventEmitter();
            return {
                registerDebugAdapterDescriptorFactory(type, factory) {
                    _debugAdapterFactories.set(type, factory);
                    log(`debug: registered adapter factory for "${type}"`);
                    return new Disposable(() => _debugAdapterFactories.delete(type));
                },
                registerDebugConfigurationProvider(type, provider) {
                    _debugConfigProviders.set(type, provider);
                    log(`debug: registered config provider for "${type}"`);
                    return new Disposable(() => _debugConfigProviders.delete(type));
                },
                startDebugging(folder, config) {
                    log('debug: startDebugging');
                    send({ type: 'debug_start', config: config || {} });
                    return Promise.resolve(true);
                },
                get activeDebugSession() { return null; },
                get breakpoints() { return []; },
                onDidChangeActiveDebugSession: new EventEmitter().event,
                onDidStartDebugSession: _onDidStartDebugSession.event,
                onDidTerminateDebugSession: _onDidTerminateDebugSession.event,
                onDidChangeBreakpoints: new EventEmitter().event,
                _onDidStartDebugSession,
                _onDidTerminateDebugSession,
            };
        })(),

        // --- Namespace: tasks ---
        tasks: (() => {
            const _onDidStartTask = new EventEmitter();
            const _onDidEndTask = new EventEmitter();
            return {
                registerTaskProvider(type, provider) {
                    _taskProviders.set(type, provider);
                    log(`tasks: registered provider for "${type}"`);
                    return new Disposable(() => _taskProviders.delete(type));
                },
                fetchTasks(filter) {
                    log('tasks: fetchTasks');
                    return Promise.resolve([]);
                },
                executeTask(task) {
                    log('tasks: executeTask');
                    send({ type: 'task_execute', task: { name: task?.name, source: task?.source, definition: task?.definition } });
                    const execution = { task, terminate() {} };
                    return Promise.resolve(execution);
                },
                onDidStartTask: _onDidStartTask.event,
                onDidEndTask: _onDidEndTask.event,
                onDidStartTaskProcess: new EventEmitter().event,
                onDidEndTaskProcess: new EventEmitter().event,
                taskExecutions: [],
                _onDidStartTask,
                _onDidEndTask,
            };
        })(),

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
        ParameterInformation: class { constructor(label, documentation) { this.label = label; this.documentation = documentation; } },
        SignatureInformation: class { constructor(label, documentation) { this.label = label || ''; this.documentation = documentation; this.parameters = []; } },
        SignatureHelp: class { constructor() { this.signatures = []; this.activeSignature = 0; this.activeParameter = 0; } },
        SignatureHelpTriggerKind: { Invoke: 1, TriggerCharacter: 2, ContentChange: 3 },
        CodeAction: class { constructor(title, kind) { this.title = title; this.kind = kind; } },
        CodeActionKind: { QuickFix: 'quickfix', Refactor: 'refactor', Source: 'source', Empty: '' },
        Hover: class { constructor(contents, range) { this.contents = Array.isArray(contents) ? contents : [contents]; this.range = range; } },
        DocumentLink: class { constructor(range, target) { this.range = range; this.target = target; } },
        DocumentDropOrPasteEditKind,
        DocumentDropEdit,
        DocumentPasteEdit,
        DataTransfer,
        DataTransferItem,
        DocumentHighlight,
        SymbolInformation,
        Color,
        ColorInformation,
        ColorPresentation,
        InlayHintLabelPart: class { constructor(value) { this.value = value === undefined || value === null ? '' : String(value); } },
        InlayHint: class { constructor(position, label, kind) { this.position = position; this.label = label; this.kind = kind; } },
        InlayHintKind: { Type: 1, Parameter: 2 },
        InlineCompletionItem: class { constructor(insertText, range, command) { this.insertText = insertText; this.range = range; this.command = command; } },
        InlineCompletionList: class { constructor(items) { this.items = items || []; } },
        InlineCompletionTriggerKind: { Invoke: 0, Automatic: 1 },
        DocumentPasteTriggerKind: { Automatic: 0, PasteAs: 1 },
        CodeLens: class { constructor(range, command) { this.range = range; this.command = command; } get isResolved() { return !!this.command; } },
        FoldingRange,
        FoldingRangeKind: { Comment: 1, Imports: 2, Region: 3 },
        SelectionRange,
        SemanticTokensLegend,
        SemanticTokensBuilder,
        SemanticTokens,
        SemanticTokensEdit,
        SemanticTokensEdits,
        TextEdit: class { static replace(range, text) { return { range, newText: text }; }; static insert(pos, text) { return { range: new Range(pos, pos), newText: text }; }; static delete(range) { return { range, newText: '' }; } },
        WorkspaceEdit: class { constructor() { this._edits = []; } replace(uri, range, text) { this._edits.push({ uri, range, text }); } insert(uri, pos, text) { this._edits.push({ uri, range: new Range(pos, pos), text }); } delete(uri, range) { this._edits.push({ uri, range, text: '' }); } set(uri, edits) { for (const e of edits) this._edits.push({ uri, ...e }); } },
        RelativePattern: class { constructor(base, pattern) { this.base = base; this.pattern = pattern; } },
        FileType: { Unknown: 0, File: 1, Directory: 2, SymbolicLink: 64 },
        EndOfLine: { LF: 1, CRLF: 2 },
        TextEditorRevealType: { Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3 },

        // --- Task types ---
        ShellExecution: class {
            constructor(commandLineOrCommand, argsOrOptions, options) {
                if (Array.isArray(argsOrOptions)) {
                    this.command = commandLineOrCommand;
                    this.args = argsOrOptions;
                    this.options = options;
                } else {
                    this.commandLine = commandLineOrCommand;
                    this.options = argsOrOptions;
                }
            }
        },
        ProcessExecution: class {
            constructor(process_, args, options) {
                this.process = process_;
                this.args = args || [];
                this.options = options;
            }
        },
        Task: class {
            constructor(definition, scopeOrName, nameOrSource, sourceOrExecution, execution) {
                this.definition = definition;
                if (typeof scopeOrName === 'string') {
                    // 4-arg form: (definition, name, source, execution)
                    this.name = scopeOrName;
                    this.source = nameOrSource;
                    this.execution = sourceOrExecution;
                } else {
                    // 5-arg form: (definition, scope, name, source, execution)
                    this.scope = scopeOrName;
                    this.name = nameOrSource;
                    this.source = sourceOrExecution;
                    this.execution = execution;
                }
                this.isBackground = false;
                this.presentationOptions = {};
                this.problemMatchers = [];
                this.group = undefined;
                this.detail = undefined;
            }
        },
        TaskGroup: { Clean: { id: 'clean' }, Build: { id: 'build' }, Rebuild: { id: 'rebuild' }, Test: { id: 'test' } },
        TaskScope: { Global: 1, Workspace: 2 },
        TaskRevealKind: { Always: 1, Silent: 2, Never: 3 },
        TaskPanelKind: { Shared: 1, Dedicated: 2, New: 3 },

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
// Settings cache — populated by Python via settings_sync / settings_changed
// -------------------------------------------------------------------------
let _settings = {};

// -------------------------------------------------------------------------
// Configuration proxy — reads from _settings cache, writes back to Python
// -------------------------------------------------------------------------
function _createConfigProxy(section) {
    const sectionData = () => (section && typeof _settings[section] === 'object' && _settings[section] !== null) ? _settings[section] : {};
    return {
        get(key, defaultValue) {
            if (arguments.length === 0 || key === undefined) {
                return Object.assign({}, sectionData());
            }
            const data = sectionData();
            if (key in data) return data[key];
            // Support dotted keys: "editor.fontSize" -> nested lookup
            const parts = String(key).split('.');
            let current = data;
            for (const part of parts) {
                if (current && typeof current === 'object' && part in current) {
                    current = current[part];
                } else {
                    return defaultValue;
                }
            }
            return current;
        },
        has(key) {
            const data = sectionData();
            if (key in data) return true;
            const parts = String(key).split('.');
            let current = data;
            for (const part of parts) {
                if (current && typeof current === 'object' && part in current) {
                    current = current[part];
                } else {
                    return false;
                }
            }
            return true;
        },
        inspect(key) {
            const data = sectionData();
            const value = key in data ? data[key] : undefined;
            return {
                key: section ? `${section}.${key}` : key,
                defaultValue: undefined,
                globalValue: value,
                workspaceValue: value,
            };
        },
        update(key, value, configTarget, overrideInLanguage) {
            if (!_settings[section] || typeof _settings[section] !== 'object') {
                _settings[section] = {};
            }
            _settings[section][key] = value;
            send({
                type: 'config_set',
                section: section || '',
                key: String(key),
                value: value,
            });
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
async function executeCommand(commandId, args, requestId) {
    const handler = _commands.get(commandId);
    if (!handler) {
        log(`command not found: ${commandId}`);
        send({ type: 'error', extensionId: '', error: `command not found: ${commandId}` });
        if (requestId) {
            send({
                type: 'command_response',
                requestId,
                ok: false,
                error: `command not found: ${commandId}`,
            });
        }
        return;
    }
    try {
        const result = handler(...(args || []).map(_deserializeArgFromPython));
        const value = result && typeof result.then === 'function'
            ? await result
            : result;
        if (requestId) {
            send({
                type: 'command_response',
                requestId,
                ok: true,
                value: value === undefined ? null : _serializeLanguageValue(value),
            });
        }
    } catch (err) {
        const message = err && err.message ? err.message : String(err);
        log(`command error ${commandId}: ${message}`);
        send({ type: 'error', extensionId: '', error: `command ${commandId}: ${message}` });
        if (requestId) {
            send({
                type: 'command_response',
                requestId,
                ok: false,
                error: `command ${commandId}: ${message}`,
            });
        }
    }
}

function _languageProviderMethod(kind) {
    return ({
        completion: 'provideCompletionItems',
        hover: 'provideHover',
        signatureHelp: 'provideSignatureHelp',
        definition: 'provideDefinition',
        typeDefinition: 'provideTypeDefinition',
        declaration: 'provideDeclaration',
        implementation: 'provideImplementation',
        references: 'provideReferences',
        documentHighlight: 'provideDocumentHighlights',
        prepareRename: 'prepareRename',
        rename: 'provideRenameEdits',
        documentLink: 'provideDocumentLinks',
        inlayHint: 'provideInlayHints',
        inlineCompletion: 'provideInlineCompletionItems',
        codeLens: 'provideCodeLenses',
        foldingRange: 'provideFoldingRanges',
        selectionRange: 'provideSelectionRanges',
        linkedEditing: 'provideLinkedEditingRanges',
        prepareCallHierarchy: 'prepareCallHierarchy',
        callHierarchyIncoming: 'provideCallHierarchyIncomingCalls',
        callHierarchyOutgoing: 'provideCallHierarchyOutgoingCalls',
        prepareTypeHierarchy: 'prepareTypeHierarchy',
        typeHierarchySupertypes: 'provideTypeHierarchySupertypes',
        typeHierarchySubtypes: 'provideTypeHierarchySubtypes',
        documentColor: 'provideDocumentColors',
        colorPresentation: 'provideColorPresentations',
        workspaceSymbol: 'provideWorkspaceSymbols',
        workspaceSymbolResolve: 'resolveWorkspaceSymbol',
        semanticTokens: 'provideDocumentSemanticTokens',
        semanticTokensLegend: 'provideDocumentSemanticTokens',
        semanticTokensRange: 'provideDocumentRangeSemanticTokens',
        semanticTokensRangeLegend: 'provideDocumentRangeSemanticTokens',
        documentSymbol: 'provideDocumentSymbols',
        codeActions: 'provideCodeActions',
        formatting: 'provideDocumentFormattingEdits',
        rangeFormatting: 'provideDocumentRangeFormattingEdits',
        onTypeFormatting: 'provideOnTypeFormattingEdits',
        prepareDocumentPaste: 'prepareDocumentPaste',
        documentPaste: 'provideDocumentPasteEdits',
        documentDrop: 'provideDocumentDropEdits',
    })[kind] || '';
}

function _normalizeProviderItems(value) {
    if (value === undefined || value === null) return [];
    if (Array.isArray(value)) return value;
    if (value && typeof value[Symbol.iterator] === 'function' && typeof value !== 'string') {
        try { return Array.from(value); } catch {}
    }
    return [value];
}

function _completionListFromProviderValue(value) {
    if (value === undefined || value === null) {
        return { items: [], isIncomplete: false };
    }
    if (Array.isArray(value)) {
        return { items: value, isIncomplete: false };
    }
    if (value.items !== undefined) {
        return {
            items: _normalizeProviderItems(value.items),
            isIncomplete: !!value.isIncomplete,
        };
    }
    return { items: [value], isIncomplete: false };
}

function _cacheWorkspaceSymbol(provider, symbol) {
    const handle = String(_nextWorkspaceSymbolHandle++);
    _workspaceSymbolCache.set(handle, { provider, symbol });
    while (_workspaceSymbolCache.size > 1000) {
        const first = _workspaceSymbolCache.keys().next().value;
        if (first === undefined) break;
        _workspaceSymbolCache.delete(first);
    }
    const value = _serializeLanguageValue(symbol);
    if (value && typeof value === 'object' && !Array.isArray(value)) {
        value._workspaceSymbolHandle = handle;
    }
    return value;
}

function _hierarchyHandleKey(kind) {
    return kind === 'typeHierarchy' ? '_nodeTypeHierarchyHandle' : '_nodeHierarchyHandle';
}

function _cacheHierarchyItem(provider, item, kind) {
    const handle = String(_nextHierarchyItemHandle++);
    _hierarchyItemCache.set(handle, { provider, item, kind });
    while (_hierarchyItemCache.size > 2000) {
        const first = _hierarchyItemCache.keys().next().value;
        if (first === undefined) break;
        _hierarchyItemCache.delete(first);
    }
    const value = _serializeLanguageValue(item);
    if (value && typeof value === 'object' && !Array.isArray(value)) {
        value[_hierarchyHandleKey(kind)] = handle;
    }
    return value;
}

function _serializeHierarchyItems(provider, value, kind) {
    return _normalizeProviderItems(value).map(item => _cacheHierarchyItem(provider, item, kind));
}

function _serializeCallHierarchyCalls(provider, value, direction) {
    const itemKey = direction === 'incoming' ? 'from' : 'to';
    return _normalizeProviderItems(value).map(call => {
        const serialized = _serializeLanguageValue(call);
        const item = call && call[itemKey];
        if (serialized && serialized[itemKey] && item) {
            serialized[itemKey] = _cacheHierarchyItem(provider, item, 'callHierarchy');
        }
        return serialized;
    });
}

function _cachedHierarchyProvider(value, kind) {
    const handle = String(value?.[_hierarchyHandleKey(kind)] || '');
    if (!handle) return null;
    const cached = _hierarchyItemCache.get(handle);
    return cached && cached.kind === kind ? cached : null;
}

async function handleLanguageProviderRequest(msg) {
    const requestId = String(msg.requestId || '');
    const kind = String(msg.kind || '');
    const methodName = _languageProviderMethod(kind);
    try {
        if (!requestId) throw new Error('Missing language provider requestId');
        if (!methodName) throw new Error(`Unsupported language provider kind: ${kind}`);
        const workspaceSymbolKind = kind === 'workspaceSymbol' || kind === 'workspaceSymbolResolve';
        const document = workspaceSymbolKind ? null : _createLanguageDocument(msg.document || msg);
        const position = _positionFromPayload(msg.position);
        const positions = Array.isArray(msg.positions)
            ? msg.positions.map(_positionFromPayload)
            : [position];
        const range = _rangeFromPayload(msg.range);
        const color = _colorFromPayload(msg.color);
        const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
        const trigger = msg.triggerCharacter === undefined || msg.triggerCharacter === null
            ? ''
            : String(msg.triggerCharacter);
        const context = msg.context && typeof msg.context === 'object'
            ? Object.assign({}, msg.context)
            : {};
        if (trigger) {
            context.triggerKind = 2;
            context.triggerCharacter = trigger;
        } else if (kind === 'completion' || kind === 'signatureHelp') {
            context.triggerKind = context.triggerKind || 1;
        }
        const providerKind = kind === 'semanticTokensLegend'
            ? 'semanticTokens'
            : kind === 'semanticTokensRangeLegend'
                ? 'semanticTokensRange'
                : kind === 'colorPresentation'
                    ? 'documentColor'
                    : kind === 'workspaceSymbolResolve'
                        ? 'workspaceSymbol'
                    : kind === 'prepareCallHierarchy' || kind === 'callHierarchyIncoming' || kind === 'callHierarchyOutgoing'
                    ? 'callHierarchy'
                    : kind === 'prepareTypeHierarchy' || kind === 'typeHierarchySupertypes' || kind === 'typeHierarchySubtypes'
                        ? 'typeHierarchy'
                    : kind === 'prepareDocumentPaste'
                        ? 'documentPaste'
                    : (kind === 'prepareRename' || kind === 'rename') ? 'rename' : kind;
        const providers = workspaceSymbolKind
            ? _languageProviders.filter(entry => entry.kind === 'workspaceSymbol')
            : _languageProviders
                .filter(entry => entry.kind === providerKind)
                .map(entry => ({ entry, score: _matchDocumentSelector(entry.selector, document) }))
                .filter(item => item.score > 0)
                .sort((a, b) => b.score - a.score)
                .map(item => item.entry);

        if (kind === 'workspaceSymbol') {
            const values = [];
            const query = String(msg.query || msg.search || '');
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawSymbols = _normalizeProviderItems(await fn.call(provider, query, token));
                    for (const symbol of rawSymbols) {
                        if (symbol && symbol.name) values.push(_cacheWorkspaceSymbol(provider, symbol));
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: values,
            });
            return;
        }

        if (kind === 'workspaceSymbolResolve') {
            const handle = String(msg.symbol?._workspaceSymbolHandle || '');
            const cached = handle ? _workspaceSymbolCache.get(handle) : null;
            if (cached && typeof cached.provider?.resolveWorkspaceSymbol === 'function') {
                try {
                    const value = await cached.provider.resolveWorkspaceSymbol.call(
                        cached.provider, cached.symbol, token);
                    send({
                        type: 'language_provider_response',
                        requestId,
                        ok: true,
                        kind,
                        value: _serializeLanguageValue(value || cached.symbol),
                    });
                    return;
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: msg.symbol || null,
            });
            return;
        }

        if (kind === 'semanticTokensLegend' || kind === 'semanticTokensRangeLegend') {
            const entry = providers.find(item => item.metadata);
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: entry ? _serializeLanguageValue(entry.metadata) : null,
            });
            return;
        }

        if (kind === 'completion') {
            const items = [];
            let isIncomplete = false;
            for (const entry of providers) {
                const triggers = (entry.triggers || []).map(item => String(item));
                if (trigger && !triggers.includes(trigger)) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token, context);
                    const normalized = _completionListFromProviderValue(value);
                    items.push(...normalized.items);
                    isIncomplete = isIncomplete || normalized.isIncomplete;
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: {
                    items: _serializeLanguageValue(items),
                    isIncomplete,
                },
            });
            return;
        }

        if (kind === 'signatureHelp') {
            for (const entry of providers) {
                const metadata = entry.metadata || {};
                const triggers = [
                    ...(metadata.triggerCharacters || []),
                    ...(metadata.retriggerCharacters || []),
                ].map(item => String(item));
                if (trigger && triggers.length && !triggers.includes(trigger)) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token, context);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: _serializeLanguageValue(value),
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        if (kind === 'linkedEditing') {
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: _serializeLanguageValue(value),
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        if (kind === 'prepareCallHierarchy' || kind === 'prepareTypeHierarchy') {
            const hierarchyKind = kind === 'prepareTypeHierarchy' ? 'typeHierarchy' : 'callHierarchy';
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: _serializeHierarchyItems(provider, value, hierarchyKind),
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        if (kind === 'callHierarchyIncoming' || kind === 'callHierarchyOutgoing') {
            const cached = _cachedHierarchyProvider(msg.item || {}, 'callHierarchy');
            const entries = cached
                ? [{ provider: cached.provider, item: cached.item }]
                : providers.map(entry => ({
                    provider: entry.provider,
                    item: _callHierarchyItemFromPayload(msg.item || {}),
                }));
            for (const entry of entries) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, entry.item, token);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: _serializeCallHierarchyCalls(
                                provider,
                                value,
                                kind === 'callHierarchyIncoming' ? 'incoming' : 'outgoing'),
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        if (kind === 'typeHierarchySupertypes' || kind === 'typeHierarchySubtypes') {
            const cached = _cachedHierarchyProvider(msg.item || {}, 'typeHierarchy');
            const entries = cached
                ? [{ provider: cached.provider, item: cached.item }]
                : providers.map(entry => ({
                    provider: entry.provider,
                    item: _typeHierarchyItemFromPayload(msg.item || {}),
                }));
            for (const entry of entries) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, entry.item, token);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: _serializeHierarchyItems(provider, value, 'typeHierarchy'),
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        if (kind === 'prepareRename' || kind === 'rename') {
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = kind === 'rename'
                        ? await fn.call(provider, document, position, String(msg.newName || ''), token)
                        : await fn.call(provider, document, position, token);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: _serializeLanguageValue(value),
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        if (kind === 'documentLink') {
            const values = [];
            const rawResolveCount = Number(msg.linkResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawLinks = _normalizeProviderItems(await fn.call(provider, document, token));
                    for (let link of rawLinks) {
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveDocumentLink === 'function') {
                                const resolved = await provider.resolveDocumentLink.call(provider, link, token);
                                if (resolved !== undefined && resolved !== null) link = resolved;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(link);
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: _serializeLanguageValue(values),
            });
            return;
        }

        if (kind === 'codeLens') {
            const values = [];
            const rawResolveCount = Number(msg.itemResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawLenses = _normalizeProviderItems(await fn.call(provider, document, token));
                    for (let lens of rawLenses) {
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveCodeLens === 'function') {
                                const resolved = await provider.resolveCodeLens.call(provider, lens, token);
                                if (resolved !== undefined && resolved !== null) lens = resolved;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(lens);
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: _serializeLanguageValue(values),
            });
            return;
        }

        if (kind === 'prepareDocumentPaste') {
            const dataTransfer = _dataTransferFromPayload(msg.dataTransfer);
            const pasteRanges = Array.isArray(msg.ranges)
                ? msg.ranges.map(_rangeFromPayload)
                : [range];
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    await fn.call(provider, document, pasteRanges, dataTransfer, token);
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: { dataTransfer: _dataTransferToPayload(dataTransfer) },
            });
            return;
        }

        if (kind === 'documentPaste') {
            const values = [];
            const dataTransfer = _dataTransferFromPayload(msg.dataTransfer);
            const pasteRanges = Array.isArray(msg.ranges)
                ? msg.ranges.map(_rangeFromPayload)
                : [range];
            const rawResolveCount = Number(msg.pasteResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            const pasteContext = Object.assign({
                triggerKind: 0,
                only: undefined,
            }, msg.context || {});
            if (pasteContext.only !== undefined && pasteContext.only !== null) {
                pasteContext.only = _dropOrPasteKindFromPayload(pasteContext.only);
            }
            for (const entry of providers) {
                if (!_dataTransferMatchesMetadata(dataTransfer, entry.metadata, 'pasteMimeTypes')) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawEdits = _normalizeProviderItems(
                        await fn.call(provider, document, pasteRanges, dataTransfer, pasteContext, token));
                    for (let edit of rawEdits) {
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveDocumentPasteEdit === 'function') {
                                const resolved = await provider.resolveDocumentPasteEdit.call(provider, edit, token);
                                if (resolved !== undefined && resolved !== null) edit = resolved;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(edit);
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: _serializeLanguageValue(values),
            });
            return;
        }

        if (kind === 'documentDrop') {
            const values = [];
            const dataTransfer = _dataTransferFromPayload(msg.dataTransfer);
            const rawResolveCount = Number(msg.dropResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            for (const entry of providers) {
                if (!_dataTransferMatchesMetadata(dataTransfer, entry.metadata, 'dropMimeTypes')) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawEdits = _normalizeProviderItems(
                        await fn.call(provider, document, position, dataTransfer, token));
                    for (let edit of rawEdits) {
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveDocumentDropEdit === 'function') {
                                const resolved = await provider.resolveDocumentDropEdit.call(provider, edit, token);
                                if (resolved !== undefined && resolved !== null) edit = resolved;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(edit);
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: _serializeLanguageValue(values),
            });
            return;
        }

        if (kind === 'semanticTokens' || kind === 'semanticTokensRange') {
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = kind === 'semanticTokensRange'
                        ? await fn.call(provider, document, range, token)
                        : await fn.call(provider, document, token);
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: {
                                tokens: _serializeLanguageValue(value),
                                legend: _serializeLanguageValue(entry.metadata || null),
                            },
                        });
                        return;
                    }
                } catch (err) {
                    log(`language provider ${kind} error: ${err.message}`);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: null,
            });
            return;
        }

        const values = [];
        for (const entry of providers) {
            const provider = entry.provider;
            const fn = provider && provider[methodName];
            if (typeof fn !== 'function') continue;
            try {
                let value;
                if (kind === 'codeActions') {
                    value = await fn.call(provider, document, range, {
                        diagnostics: msg.diagnostics || [],
                        only: msg.only,
                        triggerKind: msg.triggerKind,
                    }, token);
                } else if (kind === 'references') {
                    value = await fn.call(provider, document, position, Object.assign({
                        includeDeclaration: true,
                    }, msg.context || {}), token);
                } else if (kind === 'formatting') {
                    value = await fn.call(provider, document, msg.options || {}, token);
                } else if (kind === 'rangeFormatting') {
                    value = await fn.call(provider, document, range, msg.options || {}, token);
                } else if (kind === 'onTypeFormatting') {
                    const triggers = (entry.triggers || []).map(item => String(item));
                    if (trigger && triggers.length && !triggers.includes(trigger)) continue;
                    value = await fn.call(provider, document, position, trigger, msg.options || {}, token);
                } else if (kind === 'inlayHint') {
                    value = await fn.call(provider, document, range, token);
                } else if (kind === 'inlineCompletion') {
                    value = await fn.call(provider, document, position, Object.assign({
                        triggerKind: 1,
                        selectedCompletionInfo: undefined,
                    }, msg.context || {}), token);
                } else if (kind === 'foldingRange') {
                    value = await fn.call(provider, document, msg.context || {}, token);
                } else if (kind === 'selectionRange') {
                    value = await fn.call(provider, document, positions, token);
                } else if (kind === 'documentColor') {
                    value = await fn.call(provider, document, token);
                } else if (kind === 'colorPresentation') {
                    value = await fn.call(provider, color, { document, range }, token);
                } else if (kind === 'documentSymbol') {
                    value = await fn.call(provider, document, token);
                } else {
                    value = await fn.call(provider, document, position, token);
                }
                values.push(..._normalizeProviderItems(value));
            } catch (err) {
                log(`language provider ${kind} error: ${err.message}`);
            }
        }
        send({
            type: 'language_provider_response',
            requestId,
            ok: true,
            kind,
            value: _serializeLanguageValue(values),
        });
    } catch (err) {
        send({
            type: 'language_provider_response',
            requestId,
            ok: false,
            kind,
            error: err?.message || String(err),
        });
    }
}

async function handleTreeRequest(msg) {
    const requestId = String(msg.requestId || '');
    const viewId = String(msg.viewId || '');
    const op = String(msg.op || '');
    const reg = _treeDataProviders.get(viewId);
    const provider = reg?.provider;
    try {
        if (!provider) throw new Error(`Tree data provider not found: ${viewId}`);
        const element = msg.elementHandle
            ? _treeElementForHandle(viewId, msg.elementHandle)
            : undefined;
        let value;
        if (op === 'getChildren') {
            const fn = provider.getChildren;
            const raw = typeof fn === 'function' ? await fn.call(provider, element) : [];
            const children = raw === undefined || raw === null
                ? []
                : (Array.isArray(raw) ? raw : Array.from(raw));
            value = children.map(child => _serializeTreeElement(viewId, child));
        } else if (op === 'getTreeItem') {
            const raw = typeof provider.getTreeItem === 'function'
                ? await provider.getTreeItem(element)
                : element;
            value = _serializeTreeItem(viewId, raw, element);
        } else if (op === 'getParent') {
            const raw = typeof provider.getParent === 'function'
                ? await provider.getParent(element)
                : undefined;
            value = _serializeTreeElement(viewId, raw);
        } else {
            throw new Error(`Unsupported tree request op: ${op}`);
        }
        send({ type: 'tree_response', requestId, ok: true, value });
    } catch (err) {
        send({
            type: 'tree_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

function handleTreeViewEvent(msg) {
    const viewId = String(msg.viewId || '');
    const view = _treeViews.get(viewId);
    if (!view) return;
    const event = String(msg.event || '');
    const element = _treeElementForHandle(viewId, msg.elementHandle);
    if (event === 'selection') {
        const selection = Array.isArray(msg.selectionHandles)
            ? msg.selectionHandles
                .map(handle => _treeElementForHandle(viewId, handle))
                .filter(item => item !== undefined)
            : (element !== undefined ? [element] : []);
        view.selection = selection;
        view._onDidChangeSelection.fire({ selection });
    } else if (event === 'expand' && element !== undefined) {
        view._onDidExpandElement.fire({ element });
    } else if (event === 'collapse' && element !== undefined) {
        view._onDidCollapseElement.fire({ element });
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
        case 'executeCommand':
            await executeCommand(msg.commandId, msg.args, msg.requestId);
            break;
        case 'execute_command_response':
            handleExecuteCommandResponse(msg);
            break;
        case 'tree_request':
            await handleTreeRequest(msg);
            break;
        case 'language_provider_request':
            await handleLanguageProviderRequest(msg);
            break;
        case 'tree_view_event':
            handleTreeViewEvent(msg);
            break;
        case 'settings_sync':
            // Full settings replacement from Python
            if (msg.settings && typeof msg.settings === 'object') {
                _settings = msg.settings;
                log(`settings_sync: ${Object.keys(_settings).length} section(s)`);
            }
            break;
        case 'settings_changed': {
            // Incremental update: a single section/key changed
            const changedSection = msg.section;
            if (changedSection !== undefined) {
                if (msg.key !== undefined) {
                    if (!_settings[changedSection] || typeof _settings[changedSection] !== 'object') {
                        _settings[changedSection] = {};
                    }
                    _settings[changedSection][msg.key] = msg.value;
                } else if (msg.value !== undefined && typeof msg.value === 'object') {
                    _settings[changedSection] = msg.value;
                }
                // Fire onDidChangeConfiguration for listening extensions
                _onDidChangeConfigurationEmitter.fire({
                    affectsConfiguration(sect) {
                        if (!sect) return true;
                        return sect === changedSection || changedSection.startsWith(sect + '.') || sect.startsWith(changedSection + '.');
                    },
                });
            }
            break;
        }
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
