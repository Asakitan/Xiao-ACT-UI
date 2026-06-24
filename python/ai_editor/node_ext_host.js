/**
 * Node.js Extension Host — spawned by our Python backend.
 *
 * Loads and activates VSCode extensions that have Node.js entry points.
 * Communicates with Python over stdin/stdout JSON lines (one JSON object
 * per line). stderr is used for debug logging only.
 *
 * Inbound (from Python):
 *   register_extensions, activate, deactivate, webview_message, command,
 *   shutdown
 *
 * Outbound (to Python):
 *   activated, webview_html, webview_post_message, webview_dispose,
 *   command_registered, output, error, show_message, progress_start,
 *   progress_report, progress_done
 */
'use strict';

const path = require('node:path');
const fs = require('node:fs');
const Module = require('node:module');
const readline = require('node:readline');
const fsp = require('node:fs/promises');
const os = require('node:os');
const util = require('node:util');

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

function _formatConsoleArgs(args) {
    return args.map((arg) => {
        if (typeof arg === 'string') return arg;
        return util.inspect(arg, { depth: 5, breakLength: 120 });
    }).join(' ');
}

function _bridgeConsole(level, args) {
    const text = _formatConsoleArgs(args);
    if (!text) return;
    send({
        type: 'output',
        channel: 'Extension Console',
        text: `[${level}] ${text}\n`,
    });
}

['log', 'info', 'warn', 'error', 'debug'].forEach((level) => {
    console[level] = (...args) => _bridgeConsole(level, args);
});

// -------------------------------------------------------------------------
// EventEmitter (lightweight, mirrors vscode.EventEmitter)
// -------------------------------------------------------------------------
class EventEmitter {
    constructor() { this._listeners = []; }
    get event() {
        return (fn, thisArgs, disposables) => this._subscribe(fn, thisArgs, disposables);
    }
    _subscribe(fn, thisArgs, disposables) {
        if (typeof fn !== 'function') return new Disposable(() => {});
        const listener = thisArgs === undefined ? fn : (data) => fn.call(thisArgs, data);
        this._listeners.push(listener);
        const disposable = new Disposable(() => {
            const i = this._listeners.indexOf(listener);
            if (i >= 0) this._listeners.splice(i, 1);
        });
        if (Array.isArray(disposables)) disposables.push(disposable);
        return disposable;
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
        let result;
        if (this.scheme === 'file') {
            const p = this.path.startsWith('/') ? this.path : '/' + this.path;
            result = `file://${this.authority}${p}`;
        } else if (this.authority) {
            result = `${this.scheme}://${this.authority}${this.path}`;
        } else {
            result = `${this.scheme}:${this.path}`;
        }
        if (this.query) result += `?${this.query}`;
        if (this.fragment) result += `#${this.fragment}`;
        return result;
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

const WEBVIEW_RESOURCE_AUTHORITY_SUFFIX = '.vscode-resource.webview.local';
const WEBVIEW_CSP_SOURCE = "'self' https://webview.local https://*.vscode-resource.webview.local";

function _encodeWebviewResourceAuthority(authority) {
    return String(authority || '').replace(/./g, (ch) => {
        const code = ch.charCodeAt(0);
        if (
            (code >= 48 && code <= 57)
            || (code >= 65 && code <= 90)
            || (code >= 97 && code <= 122)
        ) {
            return ch;
        }
        return '-' + code.toString(16).padStart(4, '0');
    });
}

function _encodeWebviewResourcePath(resourcePath) {
    let value = String(resourcePath || '').replace(/\\/g, '/');
    if (!value.startsWith('/')) value = '/' + value;
    return value.split('/').map((part) => encodeURIComponent(part)).join('/');
}

function _asWebviewResourceUri(localUri) {
    const uri = localUri instanceof Uri ? localUri : _workspaceUriFromInput(localUri);
    if (uri.scheme === 'http' || uri.scheme === 'https') return uri;
    const authorityPrefix = encodeURIComponent(
        `${uri.scheme}+${_encodeWebviewResourceAuthority(uri.authority)}`);
    return new Uri(
        'https',
        `${authorityPrefix}${WEBVIEW_RESOURCE_AUTHORITY_SUFFIX}`,
        _encodeWebviewResourcePath(uri.path),
        uri.query,
        uri.fragment,
    );
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

const ProgressLocation = {
    SourceControl: 1,
    Window: 10,
    Notification: 15,
};

class LanguageModelTextPart {
    constructor(value = '', audience) {
        this.value = value === undefined || value === null ? '' : String(value);
        this.audience = audience;
    }
}

class LanguageModelToolCallPart {
    constructor(callId = '', name = '', input = {}) {
        this.callId = callId === undefined || callId === null ? '' : String(callId);
        this.name = name === undefined || name === null ? '' : String(name);
        this.input = input === undefined ? {} : input;
    }
}

class LanguageModelToolResultPart {
    constructor(callId = '', content = [], isError = false) {
        this.callId = callId === undefined || callId === null ? '' : String(callId);
        this.content = Array.isArray(content) ? content : [content];
        this.isError = !!isError;
    }
}

function _toUint8Array(value, encoding) {
    if (value instanceof Uint8Array) return value;
    if (Buffer.isBuffer(value)) return new Uint8Array(value);
    if (Array.isArray(value)) return Uint8Array.from(value.map(item => Number(item) & 0xff));
    if (value && typeof value === 'object' && Array.isArray(value.data)) {
        return Uint8Array.from(value.data.map(item => Number(item) & 0xff));
    }
    if (typeof value === 'string') {
        return new Uint8Array(Buffer.from(value, encoding === 'base64' ? 'base64' : 'utf8'));
    }
    return new Uint8Array();
}

class LanguageModelDataPart {
    constructor(data = new Uint8Array(), mimeType = 'application/octet-stream', audience) {
        this.mimeType = mimeType === undefined || mimeType === null
            ? 'application/octet-stream'
            : String(mimeType);
        this.data = _toUint8Array(data);
        this.audience = audience;
    }

    static image(data, mimeType = 'image/png') {
        return new LanguageModelDataPart(data, mimeType);
    }

    static json(value, mimeType = 'text/x-json') {
        return new LanguageModelDataPart(
            Buffer.from(JSON.stringify(value), 'utf8'), mimeType);
    }

    static text(value, mimeType = 'text/plain') {
        return new LanguageModelDataPart(Buffer.from(String(value ?? ''), 'utf8'), mimeType);
    }
}

class LanguageModelThinkingPart {
    constructor(value = '', id, metadata) {
        this.value = Array.isArray(value)
            ? value.map(item => String(item))
            : String(value ?? '');
        this.id = id;
        this.metadata = metadata;
    }
}

class LanguageModelPromptTsxPart {
    constructor(value) {
        this.value = value;
    }
}

class LanguageModelToolResult {
    constructor(content = []) {
        this.content = Array.isArray(content) ? content : [content];
    }
}

class LanguageModelChatMessage {
    constructor(role, content, name) {
        this.role = role;
        this.content = content;
        this.name = name;
    }

    set content(value) {
        if (typeof value === 'string') {
            this._content = [new LanguageModelTextPart(value)];
        } else if (Array.isArray(value)) {
            this._content = value;
        } else {
            this._content = [new LanguageModelTextPart(value === undefined ? '' : String(value))];
        }
    }

    get content() {
        return this._content;
    }

    static System(content, name) { return new LanguageModelChatMessage(0, content, name); }
    static User(content, name) { return new LanguageModelChatMessage(1, content, name); }
    static Assistant(content, name) { return new LanguageModelChatMessage(2, content, name); }
}

// -------------------------------------------------------------------------
// Memento (globalState / workspaceState)
// -------------------------------------------------------------------------
function _safeStorageSegment(value) {
    const raw = String(value || 'extension').trim() || 'extension';
    return raw.replace(/[^a-zA-Z0-9._-]+/g, '_').slice(0, 120) || 'extension';
}

function _defaultExtensionStorageBase() {
    if (process.env.SAO_AI_EDITOR_EXTENSION_STORAGE) {
        return process.env.SAO_AI_EDITOR_EXTENSION_STORAGE;
    }
    const userRoot = process.env.APPDATA || os.homedir() || os.tmpdir();
    return path.join(userRoot, 'SAO-UI', 'ai-editor', 'extension-storage');
}

function _extensionStoragePaths(extensionId, extensionPath, storageRoot) {
    const base = storageRoot || _defaultExtensionStorageBase();
    const segment = _safeStorageSegment(extensionId || path.basename(extensionPath));
    const root = path.join(base, segment);
    return {
        root,
        workspace: path.join(root, 'workspace'),
        global: path.join(root, 'global'),
        logs: path.join(root, 'logs'),
        workspaceState: path.join(root, 'workspace', 'state.json'),
        globalState: path.join(root, 'global', 'state.json'),
        secrets: path.join(root, 'global', 'secrets.json'),
    };
}

function _readJsonObject(filePath) {
    try {
        const parsed = JSON.parse(fs.readFileSync(filePath, 'utf8'));
        return parsed && typeof parsed === 'object' && !Array.isArray(parsed)
            ? parsed
            : {};
    } catch {
        return {};
    }
}

function _writeJsonObject(filePath, value) {
    try {
        fs.mkdirSync(path.dirname(filePath), { recursive: true });
        const tmpPath = `${filePath}.${process.pid}.${Date.now()}.tmp`;
        fs.writeFileSync(tmpPath, JSON.stringify(value || {}, null, 2), 'utf8');
        fs.renameSync(tmpPath, filePath);
    } catch (err) {
        log(`state write failed ${filePath}: ${err.message || err}`);
    }
}

class Memento {
    constructor(filePath) {
        this._filePath = filePath || '';
        this._data = this._filePath ? _readJsonObject(this._filePath) : {};
        this._syncKeys = [];
    }
    get(key, defaultValue) { return key in this._data ? this._data[key] : defaultValue; }
    update(key, value) {
        if (value === undefined) {
            delete this._data[key];
        } else {
            this._data[key] = value;
        }
        if (this._filePath) _writeJsonObject(this._filePath, this._data);
        return Promise.resolve();
    }
    keys() { return Object.keys(this._data); }
    setKeysForSync(keys) {
        this._syncKeys = Array.isArray(keys) ? Array.from(keys) : [];
    }
}

class SecretStorage {
    constructor(filePath) {
        this._filePath = filePath || '';
        this._data = this._filePath ? _readJsonObject(this._filePath) : {};
        this._emitter = new EventEmitter();
        this.onDidChange = this._emitter.event;
    }
    get(key) {
        return Promise.resolve(this._data[String(key)]);
    }
    store(key, value) {
        const normalized = String(key);
        this._data[normalized] = String(value);
        if (this._filePath) _writeJsonObject(this._filePath, this._data);
        this._emitter.fire({ key: normalized });
        return Promise.resolve();
    }
    delete(key) {
        const normalized = String(key);
        const existed = Object.prototype.hasOwnProperty.call(
            this._data, normalized);
        delete this._data[normalized];
        if (this._filePath) _writeJsonObject(this._filePath, this._data);
        if (existed) this._emitter.fire({ key: normalized });
        return Promise.resolve();
    }
}

// -------------------------------------------------------------------------
// Webview + WebviewView
// -------------------------------------------------------------------------
let _nextViewHandle = 1;

class Webview {
    constructor(viewId, options, defaultLocalResourceRoots) {
        this._viewId = viewId;
        this._html = '';
        this._options = options && typeof options === 'object' ? options : {};
        this._defaultLocalResourceRoots = Array.isArray(defaultLocalResourceRoots)
            ? defaultLocalResourceRoots
            : [];
        this._disposed = false;
        this._onDidReceiveMessage = new EventEmitter();
        this.onDidReceiveMessage = this._onDidReceiveMessage.event;
        this.cspSource = WEBVIEW_CSP_SOURCE;
    }
    _assertAlive() {
        if (this._disposed) throw new Error('Webview has been disposed');
    }
    _dispose() {
        if (this._disposed) return;
        this._disposed = true;
        this._html = '';
        this._onDidReceiveMessage.dispose();
    }
    get html() {
        this._assertAlive();
        return this._html;
    }
    set html(value) {
        this._assertAlive();
        this._html = value;
        send({
            type: 'webview_html',
            viewId: this._viewId,
            html: value,
            options: this._webviewOptionsPayload(),
            localResourceRoots: this._localResourceRootsPayload(),
        });
    }
    get options() {
        this._assertAlive();
        return this._options;
    }
    set options(value) {
        this._assertAlive();
        this._options = value && typeof value === 'object' ? value : {};
    }
    postMessage(message) {
        this._assertAlive();
        send({ type: 'webview_post_message', viewId: this._viewId, message });
        return Promise.resolve(true);
    }
    asWebviewUri(localUri) {
        this._assertAlive();
        return _asWebviewResourceUri(localUri);
    }
    _localResourceRootsPayload() {
        const roots = (
            this._options
            && Array.isArray(this._options.localResourceRoots)
        )
            ? this._options.localResourceRoots
            : this._defaultLocalResourceRoots;
        return roots.map((root) => {
            const uri = root instanceof Uri ? root : _workspaceUriFromInput(root);
            return {
                scheme: uri.scheme,
                path: uri.path,
                fsPath: uri.fsPath,
                uri: uri.toString(),
            };
        });
    }
    _webviewOptionsPayload() {
        const options = this._options || {};
        const payload = {};
        if ('enableScripts' in options) payload.enableScripts = !!options.enableScripts;
        if ('retainContextWhenHidden' in options) {
            payload.retainContextWhenHidden = !!options.retainContextWhenHidden;
        }
        const roots = this._localResourceRootsPayload();
        if (roots !== undefined) payload.localResourceRoots = roots;
        return payload;
    }
}

class WebviewView {
    constructor(viewId, viewType, webviewOptions, defaultLocalResourceRoots) {
        this.viewType = viewType;
        this.webview = new Webview(
            viewId, webviewOptions || {}, defaultLocalResourceRoots || []);
        this._title = '';
        this._description = '';
        this._badge = undefined;
        this.visible = true;
        this._disposed = false;
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
    show(preserveFocus) {
        if (this._disposed) throw new Error('WebviewView has been disposed');
        this.visible = true;
    }
    dispose() {
        if (this._disposed) return;
        this._disposed = true;
        this.visible = false;
        this.webview._dispose();
        this._onDidDispose.fire();
        this._onDidDispose.dispose();
        this._onDidChangeVisibility.dispose();
    }
}

function _defaultLocalResourceRoots(extensionPath) {
    const roots = [Uri.file(_workspaceRoot)];
    if (extensionPath) roots.push(Uri.file(extensionPath));
    return roots;
}

function _webviewPanelOptionsPayload(options) {
    const raw = options && typeof options === 'object' ? options : {};
    const payload = {};
    if ('enableFindWidget' in raw) payload.enableFindWidget = !!raw.enableFindWidget;
    if ('retainContextWhenHidden' in raw) {
        payload.retainContextWhenHidden = !!raw.retainContextWhenHidden;
    }
    return payload;
}

function _webviewPanelColumnFromShowOptions(showOptions, fallback = 1) {
    if (typeof showOptions === 'number') {
        return showOptions === -2 ? 2 : (showOptions > 0 ? showOptions : fallback);
    }
    if (showOptions && typeof showOptions === 'object') {
        const column = Number(showOptions.viewColumn);
        if (Number.isFinite(column)) return column === -2 ? 2 : (column > 0 ? column : fallback);
    }
    return fallback;
}

function _webviewPanelPreserveFocusFromShowOptions(showOptions) {
    return !!(showOptions && typeof showOptions === 'object' && showOptions.preserveFocus);
}

function _createWebviewPanelObject(
    viewType, title, viewId, webviewOptions, extensionPath, showOptions) {
    const view = new WebviewView(
        viewId, viewType, webviewOptions || {},
        _defaultLocalResourceRoots(extensionPath));
    view.title = title || '';
    _webviewViews.set(viewId, view);
    const viewStateEmitter = new EventEmitter();
    let disposed = false;
    let currentTitle = title || '';
    let iconPath = undefined;
    let visible = true;
    let active = !_webviewPanelPreserveFocusFromShowOptions(showOptions);
    let viewColumn = _webviewPanelColumnFromShowOptions(showOptions);
    function assertPanelAlive() {
        if (disposed) throw new Error('WebviewPanel has been disposed');
    }
    const panel = {
        viewType,
        get title() {
            assertPanelAlive();
            return currentTitle;
        },
        set title(value) {
            assertPanelAlive();
            currentTitle = String(value || '');
        },
        get iconPath() {
            assertPanelAlive();
            return iconPath;
        },
        set iconPath(value) {
            assertPanelAlive();
            iconPath = value;
        },
        get webview() {
            assertPanelAlive();
            return view.webview;
        },
        get visible() {
            assertPanelAlive();
            return visible;
        },
        get active() {
            assertPanelAlive();
            return active;
        },
        get viewColumn() {
            assertPanelAlive();
            return viewColumn;
        },
        options: _webviewPanelOptionsPayload(webviewOptions),
        onDidDispose: view.onDidDispose,
        onDidChangeViewState: viewStateEmitter.event,
        reveal(nextViewColumn, preserveFocus) {
            assertPanelAlive();
            visible = true;
            active = !preserveFocus;
            if (nextViewColumn !== undefined) {
                viewColumn = _webviewPanelColumnFromShowOptions(nextViewColumn, viewColumn);
            }
            viewStateEmitter.fire({ webviewPanel: panel });
        },
        dispose() {
            if (disposed) return;
            disposed = true;
            visible = false;
            active = false;
            view.dispose();
            _webviewViews.delete(viewId);
            viewStateEmitter.dispose();
            send({ type: 'webview_dispose', viewId });
        },
    };
    return panel;
}

function _safeViewIdPart(value) {
    return String(value || 'custom')
        .replace(/[^a-zA-Z0-9_.-]+/g, '-')
        .slice(0, 80) || 'custom';
}

function _customEditorDocumentKey(viewType, uriLike, viewId = '') {
    const uri = uriLike instanceof Uri ? uriLike : _workspaceUriFromInput(uriLike);
    const base = `${String(viewType || '')}|${uri.toString()}`;
    return viewId ? `${base}|${String(viewId)}` : base;
}

function _customEditorEntryForMessage(msg) {
    const viewId = String(msg.viewId || msg.view_id || '');
    if (viewId && _customEditorViewKeys.has(viewId)) {
        return _customEditorDocuments.get(_customEditorViewKeys.get(viewId)) || null;
    }
    const viewType = String(msg.viewType || msg.view_type || msg.customEditorId || '');
    const uriText = msg.uri || msg.resource || msg.path;
    if (viewType && uriText) {
        const key = _customEditorDocumentKey(viewType, uriText);
        const entry = _customEditorDocuments.get(key);
        if (entry) return entry;
        const uri = _workspaceUriFromInput(uriText).toString();
        for (const item of _customEditorDocuments.values()) {
            if (item.viewType === viewType && item.uri.toString() === uri) return item;
        }
    }
    if (uriText) {
        const uri = _workspaceUriFromInput(uriText).toString();
        for (const entry of _customEditorDocuments.values()) {
            if (entry.uri.toString() === uri) return entry;
        }
    }
    return null;
}

function _customEditorStatePayload(entry, kind, extra = {}) {
    if (!entry) return {};
    const provider = entry.provider || {};
    return {
        viewType: entry.viewType,
        viewId: entry.viewId,
        uri: entry.uri.toString(),
        dirty: !!entry.dirty,
        editable: !!entry.editable,
        textEditor: !!entry.textEditor,
        supportsSave: !!entry.textEditor || typeof provider.saveCustomDocument === 'function',
        supportsSaveAs: !!entry.textEditor || typeof provider.saveCustomDocumentAs === 'function',
        supportsRevert: !!entry.textEditor || typeof provider.revertCustomDocument === 'function',
        supportsBackup: typeof provider.backupCustomDocument === 'function',
        kind: kind || entry.lastKind || '',
        label: entry.lastLabel || '',
        editId: entry.lastEditId || 0,
        edits: entry.edits.length,
        currentEditIndex: entry.currentEditIndex,
        canUndo: entry.currentEditIndex >= 0,
        canRedo: entry.currentEditIndex < entry.edits.length - 1,
        backupId: entry.backupId || '',
        ...extra,
    };
}

function _sendCustomEditorState(entry, kind, extra = {}) {
    send({
        type: 'custom_editor_changed',
        ..._customEditorStatePayload(entry, kind, extra),
    });
}

function _isCustomEditorEditEvent(event) {
    return !!(
        event
        && event.document
        && typeof event.undo === 'function'
        && typeof event.redo === 'function'
    );
}

function _customEditorEntryForDocument(viewType, document) {
    if (document && document.uri) {
        const key = _customEditorDocumentKey(viewType, document.uri);
        const entry = _customEditorDocuments.get(key);
        if (entry) return entry;
    }
    for (const entry of _customEditorDocuments.values()) {
        if (entry.viewType === viewType && entry.document === document) return entry;
    }
    return null;
}

function _refreshCustomEditorDirty(entry) {
    entry.dirty = !!entry.contentDirty || entry.currentEditIndex >= 0;
    return entry.dirty;
}

function _handleCustomDocumentChange(viewType, event) {
    const entry = _customEditorEntryForDocument(viewType, event?.document);
    if (!entry) return;
    const editEvent = _isCustomEditorEditEvent(event);
    entry.lastKind = editEvent ? 'edit' : 'content';
    entry.lastLabel = String(event?.label || '');
    if (editEvent) {
        const spliceStart = entry.currentEditIndex + 1;
        if (spliceStart < entry.edits.length) entry.edits.splice(spliceStart);
        const editId = _nextCustomEditorEditHandle++;
        entry.lastEditId = editId;
        entry.edits.push({
            id: editId,
            label: entry.lastLabel,
            undo: event.undo,
            redo: event.redo,
        });
        entry.currentEditIndex = entry.edits.length - 1;
    } else {
        entry.contentDirty = true;
    }
    _refreshCustomEditorDirty(entry);
    _sendCustomEditorState(entry, entry.lastKind);
}

async function _disposeCustomEditorBackup(entry) {
    const backup = entry?.backup;
    entry.backup = null;
    entry.backupId = '';
    if (backup && typeof backup.delete === 'function') {
        try {
            await Promise.resolve(backup.delete());
        } catch (err) {
            log(`custom editor backup delete failed: ${err?.message || err}`);
        }
    }
}

async function _customEditorBackupDestination(entry) {
    const backupDir = path.join(os.tmpdir(), 'sao-ai-editor-custom-editor-backups');
    try { await fsp.mkdir(backupDir, { recursive: true }); } catch {}
    const name = [
        _safeViewIdPart(entry.viewId || entry.viewType),
        Date.now(),
        Math.floor(Math.random() * 1000000),
    ].join('-') + '.bak';
    return Uri.file(path.join(backupDir, name));
}

function _markCustomEditorClean(entry) {
    entry.contentDirty = false;
    entry.dirty = false;
    entry.edits.length = 0;
    entry.currentEditIndex = -1;
    entry.lastEditId = 0;
}

function _customTextEditorEntriesForDocument(document) {
    if (!document || !document.uri) return [];
    const uri = document.uri.toString();
    return Array.from(_customEditorDocuments.values()).filter(entry => (
        entry.textEditor && entry.uri.toString() === uri
    ));
}

function _markCustomTextEditorsDirty(document, kind = 'textChange') {
    if (!document) return;
    document.isDirty = true;
    for (const entry of _customTextEditorEntriesForDocument(document)) {
        entry.contentDirty = true;
        entry.dirty = true;
        entry.lastKind = kind;
        _sendCustomEditorState(entry, kind);
    }
}

function _markCustomTextEditorsSaved(document, kind = 'save') {
    if (!document) return;
    document.isDirty = false;
    for (const entry of _customTextEditorEntriesForDocument(document)) {
        _markCustomEditorClean(entry);
        entry.lastKind = kind;
        _sendCustomEditorState(entry, kind, { dirty: false });
    }
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
const _knownExtensions = new Map();      // extensionId -> { extensionPath, manifest, extensionKind }
const _configurationDefaults = {};       // contributed default settings by dotted path
const _configurationLanguageDefaults = {}; // languageId -> defaults by dotted path
const _extensionActivationRequests = new Map(); // requestId -> pending activation request
const _extensionActivationInFlight = new Map(); // extensionId -> pending activation request
const _commands = new Map();             // commandId -> handler
const _pythonCommandRequests = new Map(); // requestId -> { resolve, reject, timer }
const _pythonLmRequests = new Map();      // requestId -> { resolve, reject, timer }
const _windowDialogRequests = new Map();  // requestId -> { resolve, timer, cleanup, kind }
const _envClipboardRequests = new Map();  // requestId -> { resolve, timer, cleanup, action }
const _webviewViewProviders = new Map(); // viewType -> { provider, options }
const _webviewViews = new Map();         // viewId -> WebviewView
const _webviewPanelSerializers = new Map(); // viewType -> { serializer, extensionId, extensionPath }
const _customEditorProviders = new Map(); // viewType -> { provider, options, extensionId }
const _customEditorDocuments = new Map(); // viewType|uri|viewId -> resolved custom document state
const _customEditorViewKeys = new Map();  // viewId -> custom editor document key
const _treeDataProviders = new Map();    // viewId -> { provider, disposable? }
const _treeViews = new Map();            // viewId -> TreeView-like object
const _treeElementStores = new Map();    // viewId -> element handle store
const _outputChannels = new Map();       // name -> OutputChannel
const _fileSystemProviders = new Map();  // scheme -> { provider, options, extensionId }
const _textDocumentContentProviders = new Map(); // scheme -> { provider, extensionId }
const _uriHandlers = new Map();          // extensionId -> { handler }
const _languageProviders = [];           // { kind, selector, provider, triggers?, disposable }
const _lmTools = new Map();              // name -> { handle, tool, extensionId, metadata }
const _chatParticipants = new Map();     // id -> { handle, handler, extensionId }
let _nextLanguageProviderHandle = 1;
let _nextLmToolHandle = 1;
let _nextChatParticipantHandle = 1;
let _nextPythonCommandRequestHandle = 1;
let _nextPythonLmRequestHandle = 1;
let _nextExtensionActivationRequestHandle = 1;
let _nextWindowDialogRequestHandle = 1;
let _nextEnvClipboardRequestHandle = 1;
let _envClipboardFallbackText = '';
const _languageDocumentTextCache = new Map(); // uri -> { version, text }
const _workspaceTextDocuments = new Map(); // uri -> TextDocument-like object
const _onDidOpenTextDocumentEmitter = new EventEmitter();
const _onDidCloseTextDocumentEmitter = new EventEmitter();
const _onDidChangeTextDocumentEmitter = new EventEmitter();
const _onDidSaveTextDocumentEmitter = new EventEmitter();
const _onDidChangeLmToolsEmitter = new EventEmitter();
const _workspaceRoot = path.resolve(process.cwd());
const _workspaceName = path.basename(_workspaceRoot) || _workspaceRoot;
const _workspaceDefaultSkipDirs = new Set(['.git', 'node_modules', '__pycache__', '.venv', 'venv']);
const _workspaceSymbolCache = new Map(); // handle -> { provider, symbol }
let _nextWorkspaceSymbolHandle = 1;
const _hierarchyItemCache = new Map(); // handle -> { provider, item, kind }
let _nextHierarchyItemHandle = 1;
const _diagnosticCollections = new Map(); // name -> DiagnosticCollection
const _onDidChangeDiagnosticsEmitter = new EventEmitter();
let _nextCustomEditorEditHandle = 1;
const _debugAdapterFactories = new Map();   // type -> factory
const _debugConfigProviders = new Map();    // type -> provider
const _taskProviders = new Map();           // type -> provider
const _scmProviders = new Map();            // id -> SourceControl
const _authenticationProviders = new Map(); // id -> { label, provider, options, listener }
const _authenticationSessions = new Map();  // id -> AuthenticationSession[]
const _onDidChangeAuthenticationSessionsEmitter = new EventEmitter();
const _onDidChangeConfigurationEmitter = new EventEmitter();
let _nextUntitledDocument = 1;
let _nextProgressHandle = 1;
let _nextQuickInputHandle = 1;
let _activeQuickInput = null;
const _quickInputs = new Map();          // quickInput id -> QuickInputBase

function _workspaceDocumentIsOpened(document) {
    return !!document && document.__opened !== false;
}

function _workspacePromoteTextDocument(document) {
    if (!document || _workspaceDocumentIsOpened(document)) return document;
    document.__opened = true;
    _onDidOpenTextDocumentEmitter.fire(document);
    return document;
}

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

function _extensionDiagnosticsEnabled() {
    const direct = _settings && _settings.extensions;
    const nested = _settings && _settings.ai_editor && _settings.ai_editor.extensions;
    return !!(
        (direct && direct.diagnostics_enabled === true)
        || (nested && nested.diagnostics_enabled === true)
    );
}

function _sendDiagnosticEvent(category, startedAt, ok, detail, error) {
    if (!_extensionDiagnosticsEnabled()) return;
    send({
        type: 'diagnostic_event',
        category,
        elapsedMs: Math.max(0, Date.now() - startedAt),
        ok: !!ok,
        detail: detail === undefined || detail === null ? '' : String(detail).slice(0, 200),
        error: error ? String(error).slice(0, 300) : '',
    });
}

async function _diagnoseAsync(category, detail, fn) {
    if (!_extensionDiagnosticsEnabled()) return fn();
    const startedAt = Date.now();
    try {
        const value = await fn();
        _sendDiagnosticEvent(category, startedAt, true, detail, '');
        return value;
    } catch (err) {
        _sendDiagnosticEvent(
            category,
            startedAt,
            false,
            detail,
            err && err.message ? err.message : String(err),
        );
        throw err;
    }
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

function _requestPythonLm(action, payload, timeoutMs = 30000, token) {
    const requestId = `pylm-${_nextPythonLmRequestHandle++}`;
    if (token?.isCancellationRequested) {
        return Promise.reject(new Error(`Python LM request cancelled: ${action}`));
    }
    return new Promise((resolve, reject) => {
        let cancelSubscription;
        let timer;
        const cleanup = () => {
            _pythonLmRequests.delete(requestId);
            if (timer) clearTimeout(timer);
            try { cancelSubscription?.dispose?.(); } catch {}
        };
        const cancelPending = (reason) => {
            const pending = _pythonLmRequests.get(requestId);
            if (!pending) return;
            cleanup();
            send({
                type: 'lm_model_cancel',
                requestId,
                action,
            });
            reject(new Error(reason));
        };
        timer = setTimeout(() => {
            cancelPending(`Python LM request timed out: ${action}`);
        }, timeoutMs);
        if (token && typeof token.onCancellationRequested === 'function') {
            cancelSubscription = token.onCancellationRequested(() => {
                cancelPending(`Python LM request cancelled: ${action}`);
            });
        }
        _pythonLmRequests.set(requestId, { resolve, reject, timer, cleanup });
        send(Object.assign({
            type: 'lm_model_request',
            requestId,
            action,
        }, payload || {}));
    });
}

function _handlePythonLmResponse(msg) {
    const requestId = String(msg.requestId || '');
    const pending = _pythonLmRequests.get(requestId);
    if (!pending) return;
    pending.cleanup?.();
    if (msg.ok) {
        pending.resolve(_deserializeArgFromPython(msg.value));
    } else {
        pending.reject(new Error(msg.error || 'Python LM request failed'));
    }
}

function _normalizeLmCapabilities(value) {
    const caps = value && typeof value === 'object' ? value : {};
    return {
        supportsImageToText: !!(
            caps.supportsImageToText
            ?? caps.supports_image_to_text
            ?? caps.vision),
        supportsToolCalling: !!(
            caps.supportsToolCalling
            ?? caps.supports_tool_calling
            ?? caps.toolCalling
            ?? caps.tools
            ?? true),
        editToolsHint: !!(caps.editToolsHint ?? caps.edit_tools_hint),
    };
}

function _languageModelTextFromChunk(chunk) {
    if (chunk === undefined || chunk === null) return '';
    if (chunk instanceof LanguageModelTextPart) return chunk.value;
    if (typeof chunk === 'string' || typeof chunk === 'number' || typeof chunk === 'boolean') {
        return String(chunk);
    }
    if (chunk && typeof chunk === 'object') {
        const text = chunk.value ?? chunk.text ?? chunk.content;
        if (text !== undefined && text !== null) return String(text);
    }
    return String(chunk);
}

function _languageModelDataPartFromPayload(chunk) {
    const data = chunk.data ?? chunk.value ?? chunk.content ?? '';
    const encoding = chunk.encoding || (chunk.base64 ? 'base64' : undefined);
    return new LanguageModelDataPart(
        _toUint8Array(data, encoding),
        chunk.mimeType ?? chunk.mime_type ?? chunk.mime ?? 'application/octet-stream',
        chunk.audience);
}

function _languageModelPartFromPayload(chunk) {
    if (
        chunk instanceof LanguageModelTextPart
        || chunk instanceof LanguageModelToolCallPart
        || chunk instanceof LanguageModelDataPart
        || chunk instanceof LanguageModelThinkingPart
        || chunk instanceof LanguageModelToolResultPart
    ) {
        return chunk;
    }
    if (chunk === undefined || chunk === null
        || typeof chunk === 'string'
        || typeof chunk === 'number'
        || typeof chunk === 'boolean') {
        return new LanguageModelTextPart(_languageModelTextFromChunk(chunk));
    }
    if (chunk && typeof chunk === 'object') {
        const type = String(chunk.type ?? chunk.kind ?? '').toLowerCase();
        if (type === 'tool_use' || type === 'tool_call' || type === 'toolcall'
            || (chunk.name && (chunk.toolCallId || chunk.callId || chunk.id))) {
            return new LanguageModelToolCallPart(
                chunk.toolCallId ?? chunk.callId ?? chunk.id ?? '',
                chunk.name,
                chunk.parameters ?? chunk.input ?? chunk.arguments ?? {});
        }
        if (type === 'data' || chunk.mimeType || chunk.mime_type || chunk.mime) {
            return _languageModelDataPartFromPayload(chunk);
        }
        if (type === 'thinking') {
            return new LanguageModelThinkingPart(
                chunk.value ?? chunk.text ?? chunk.content ?? '',
                chunk.id,
                chunk.metadata);
        }
        if (type === 'tool_result' || type === 'tool_result_part') {
            return new LanguageModelToolResultPart(
                chunk.toolCallId ?? chunk.callId ?? chunk.id ?? '',
                Array.isArray(chunk.content) ? chunk.content.map(_languageModelPartFromPayload) : [],
                chunk.isError);
        }
        if (chunk.value !== undefined || chunk.text !== undefined || chunk.content !== undefined) {
            return new LanguageModelTextPart(_languageModelTextFromChunk(chunk), chunk.audience);
        }
    }
    return chunk;
}

function _languageModelResponsePartText(part) {
    if (part instanceof LanguageModelTextPart) return part.value;
    return '';
}

function _languageModelResponseFromPayload(value) {
    const payload = value && typeof value === 'object' ? value : {};
    const rawChunks = Array.isArray(payload.chunks)
        ? payload.chunks
        : (payload.text !== undefined && payload.text !== null ? [payload.text] : []);
    const chunks = rawChunks.map(_languageModelPartFromPayload);
    const stream = async function* () {
        for (const chunk of chunks) {
            yield chunk;
        }
    };
    const text = async function* () {
        for (const chunk of chunks) {
            const partText = _languageModelResponsePartText(chunk);
            if (partText) yield partText;
        }
    };
    const fallbackText = chunks.map(_languageModelResponsePartText).join('');
    return {
        text: text(),
        value: String(payload.text ?? fallbackText),
        stream: stream(),
    };
}

function _languageModelChatFromPayload(model) {
    const meta = model && typeof model === 'object' ? model : {};
    const id = String(meta.id || '');
    const apiObject = {
        id,
        name: String(meta.name || id),
        vendor: String(meta.vendor || ''),
        family: String(meta.family || ''),
        version: String(meta.version || ''),
        maxInputTokens: Number(meta.maxInputTokens ?? meta.max_input_tokens ?? 0) || undefined,
        capabilities: _normalizeLmCapabilities(meta.capabilities),
        countTokens(text, token) {
            return _requestPythonLm('countTokens', {
                modelId: id,
                text: _serializeLanguageValue(text),
            }, 5000, token);
        },
        async sendRequest(messages, options, token) {
            const value = await _requestPythonLm('sendRequest', {
                modelId: id,
                messages: _serializeLanguageValue(messages || []),
                options: _serializeLanguageValue(options || {}),
            }, 120000, token);
            return _languageModelResponseFromPayload(value);
        },
    };
    return Object.freeze(apiObject);
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

function _serializeWindowDialogOptions(kind, options = {}) {
    const result = {
        title: options?.title === undefined ? undefined : String(options.title),
        defaultPath: _pathFromUriLike(options?.defaultUri),
        filters: _serializeLanguageValue(options?.filters || {}),
    };
    if (kind === 'open') {
        result.openLabel = options?.openLabel === undefined ? undefined : String(options.openLabel);
        result.canSelectFiles = options?.canSelectFiles !== false;
        result.canSelectFolders = !!options?.canSelectFolders;
        result.canSelectMany = !!options?.canSelectMany;
    } else {
        result.saveLabel = options?.saveLabel === undefined ? undefined : String(options.saveLabel);
    }
    return result;
}

function _urisFromDialogPaths(paths) {
    return (Array.isArray(paths) ? paths : [paths])
        .filter(value => value !== undefined && value !== null && String(value))
        .map(value => Uri.file(String(value)));
}

function _handleWindowDialogResponse(msg) {
    const requestId = String(msg.requestId || '');
    const pending = _windowDialogRequests.get(requestId);
    if (!pending) return;
    pending.cleanup?.();
    if (!msg.ok) {
        log(`window dialog ${pending.kind} failed: ${msg.error || 'unknown error'}`);
        pending.resolve(undefined);
        return;
    }
    const value = msg.value && typeof msg.value === 'object' ? msg.value : {};
    if (value.cancelled) {
        pending.resolve(undefined);
    } else if (pending.kind === 'open') {
        const paths = value.paths !== undefined ? value.paths : value.path;
        const uris = _urisFromDialogPaths(paths);
        pending.resolve(uris.length ? uris : undefined);
    } else {
        const paths = _urisFromDialogPaths(value.path || value.paths);
        pending.resolve(paths[0]);
    }
}

function _requestWindowDialog(kind, options = {}, token = undefined) {
    if (token?.isCancellationRequested) return Promise.resolve(undefined);
    const requestId = `wndlg-${_nextWindowDialogRequestHandle++}`;
    return new Promise(resolve => {
        let timer;
        let cancelSubscription;
        const cleanup = () => {
            _windowDialogRequests.delete(requestId);
            if (timer) clearTimeout(timer);
            try { cancelSubscription?.dispose?.(); } catch {}
        };
        const finishUndefined = () => {
            const pending = _windowDialogRequests.get(requestId);
            if (!pending) return;
            cleanup();
            resolve(undefined);
        };
        timer = setTimeout(finishUndefined, 30000);
        if (token && typeof token.onCancellationRequested === 'function') {
            cancelSubscription = token.onCancellationRequested(finishUndefined);
        }
        _windowDialogRequests.set(requestId, { resolve, timer, cleanup, kind });
        send({
            type: 'window_dialog_request',
            requestId,
            kind,
            options: _serializeWindowDialogOptions(kind, options),
        });
    });
}

function _handleEnvClipboardResponse(msg) {
    const requestId = String(msg.requestId || '');
    const pending = _envClipboardRequests.get(requestId);
    if (!pending) return;
    pending.cleanup?.();
    if (msg.ok && pending.action === 'read') {
        const value = msg.value && typeof msg.value === 'object' ? msg.value : {};
        const text = value.text ?? msg.text ?? '';
        _envClipboardFallbackText = String(text);
        pending.resolve(_envClipboardFallbackText);
    } else {
        if (!msg.ok) {
            log(`env.clipboard.${pending.action} failed: ${msg.error || 'unknown error'}`);
        }
        pending.resolve(
            pending.action === 'read' ? _envClipboardFallbackText : undefined);
    }
}

function _requestEnvClipboard(action, text = '', token = undefined) {
    if (action === 'write') _envClipboardFallbackText = String(text ?? '');
    if (token?.isCancellationRequested) {
        return Promise.resolve(
            action === 'read' ? _envClipboardFallbackText : undefined);
    }
    const requestId = `clip-${_nextEnvClipboardRequestHandle++}`;
    return new Promise(resolve => {
        let timer;
        const cleanup = () => {
            _envClipboardRequests.delete(requestId);
            if (timer) clearTimeout(timer);
        };
        timer = setTimeout(() => {
            const pending = _envClipboardRequests.get(requestId);
            if (!pending) return;
            cleanup();
            resolve(action === 'read' ? _envClipboardFallbackText : undefined);
        }, 3000);
        _envClipboardRequests.set(requestId, { resolve, timer, cleanup, action });
        send({
            type: 'env_clipboard_request',
            requestId,
            action,
            text: String(text ?? ''),
        });
    });
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
    if (value instanceof LanguageModelChatMessage) {
        const result = {
            role: value.role,
            content: _serializeLanguageValue(value.content),
        };
        if (value.name !== undefined) result.name = value.name;
        return result;
    }
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

function _lmToolDefinitionName(definition) {
    if (typeof definition === 'string') return definition;
    return String(definition?.name || definition?.id || '');
}

function _lmToolInputSchema(tool, definition) {
    const schema = definition?.inputSchema || definition?.schema
        || tool?.inputSchema || tool?.schema;
    return schema && typeof schema === 'object' ? schema : { type: 'object', properties: {} };
}

function _lmToolDescription(name, tool, definition) {
    return String(
        definition?.modelDescription
        || definition?.description
        || definition?.displayName
        || tool?.description
        || name
    );
}

function _serializeLanguageModelContentPart(part) {
    if (part === undefined || part === null) return { type: 'text', text: '' };
    if (typeof part === 'string' || typeof part === 'number' || typeof part === 'boolean') {
        return { type: 'text', text: String(part) };
    }
    if (part instanceof Uri) return { type: 'text', text: part.toString() };
    if (part && typeof part === 'object') {
        const text = part.text ?? part.value ?? part.markdown;
        if (text !== undefined && text !== null) {
            return { type: 'text', text: String(text) };
        }
        const serialized = _serializeLanguageValue(part);
        if (serialized && typeof serialized === 'object') return serialized;
    }
    return { type: 'text', text: String(part) };
}

function _serializeLanguageModelToolResult(value) {
    if (value === undefined || value === null) {
        return { content: [] };
    }
    if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') {
        return { content: [{ type: 'text', text: String(value) }] };
    }
    if (Array.isArray(value?.content)) {
        return {
            content: value.content.map(_serializeLanguageModelContentPart),
        };
    }
    return _serializeLanguageValue(value);
}

async function _invokeLmToolEntry(entry, input, token) {
    const tool = entry?.tool;
    if (!tool) throw new Error(`Language model tool not found: ${entry?.name || ''}`);
    const options = {
        input: _deserializeArgFromPython(input),
        toolInvocationToken: token,
    };
    let result;
    if (typeof tool.invoke === 'function') {
        result = tool.invoke(options, token);
    } else if (typeof tool === 'function') {
        result = tool(options, token);
    } else {
        throw new Error(`Language model tool has no invoke handler: ${entry.name}`);
    }
    return result && typeof result.then === 'function' ? await result : result;
}

function _chatPartToText(part) {
    if (part === undefined || part === null) return '';
    if (typeof part === 'string' || typeof part === 'number' || typeof part === 'boolean') {
        return String(part);
    }
    if (part instanceof Uri) return part.toString();
    if (part && typeof part === 'object') {
        const text = part.value ?? part.text ?? part.markdown ?? part.content;
        if (text !== undefined && text !== null) return String(text);
    }
    return String(part);
}

function _createChatResponseStream(parts) {
    const append = (value) => {
        const text = _chatPartToText(value);
        parts.push(text);
        return text;
    };
    return {
        markdown: append,
        text: append,
        progress() {},
        warning: append,
        info: append,
        anchor(value, title) { append(title || value); },
        button() {},
        reference() {},
        reference2() {},
        filetree() {},
        codeblockUri(value) { append(value); },
        codeCitation(value) { append(value); },
        textEdit() {},
        confirmation() {},
        notebookEdit() {},
        workspaceEdit() {},
        thinkingProgress: append,
        beginToolInvocation() {},
        updateToolInvocation() {},
        push: append,
    };
}

function _normalizeProgressOptions(options) {
    const opts = options && typeof options === 'object' ? options : {};
    return {
        location: opts.location,
        title: opts.title === undefined || opts.title === null ? '' : String(opts.title),
        cancellable: !!opts.cancellable,
    };
}

function _normalizeProgressReport(value) {
    if (value === undefined || value === null) return {};
    if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') {
        return { message: String(value) };
    }
    if (value && typeof value === 'object') {
        const report = {};
        if (value.message !== undefined && value.message !== null) {
            report.message = String(value.message);
        }
        const rawIncrement = value.increment ?? value.worked;
        if (rawIncrement !== undefined && rawIncrement !== null) {
            const increment = Number(rawIncrement);
            if (Number.isFinite(increment)) report.increment = increment;
        }
        return report;
    }
    return { message: String(value) };
}

function _withProgress(options, task) {
    const progressId = `progress-${_nextProgressHandle++}`;
    const normalizedOptions = _normalizeProgressOptions(options);
    const token = {
        isCancellationRequested: false,
        onCancellationRequested: new EventEmitter().event,
    };
    send({
        type: 'progress_start',
        progressId,
        options: normalizedOptions,
        message: normalizedOptions.title,
    });
    const progress = {
        report(value) {
            const report = _normalizeProgressReport(value);
            send(Object.assign({
                type: 'progress_report',
                progressId,
                options: normalizedOptions,
            }, report));
        },
    };
    return Promise.resolve()
        .then(() => task(progress, token))
        .then(
            (value) => {
                send({ type: 'progress_done', progressId, ok: true, options: normalizedOptions });
                return value;
            },
            (error) => {
                send({
                    type: 'progress_done',
                    progressId,
                    ok: false,
                    options: normalizedOptions,
                    error: String(error && error.message || error),
                });
                throw error;
            });
}

function _chatRequestFromPayload(msg) {
    const request = msg.request && typeof msg.request === 'object'
        ? _deserializeArgFromPython(msg.request)
        : {};
    return Object.assign({
        prompt: String(msg.prompt ?? request.prompt ?? ''),
        command: String(msg.command ?? request.command ?? ''),
        references: [],
        toolReferences: [],
        attempt: 0,
        enableCommandDetection: true,
        location: 1,
        tools: {},
    }, request);
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
        if (/^[a-zA-Z]:[\\/]/.test(value)) return Uri.file(value);
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

function _normalizeFileSystemScheme(scheme) {
    return String(scheme || '').trim().toLowerCase();
}

function _contentToUint8Array(value) {
    if (value instanceof Uint8Array) return value;
    if (Buffer.isBuffer(value)) return new Uint8Array(value);
    if (Array.isArray(value)) return Uint8Array.from(value);
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (value === undefined || value === null) return new Uint8Array();
    return new TextEncoder().encode(String(value));
}

async function _fileSystemProviderForUri(uri) {
    const scheme = _normalizeFileSystemScheme(uri?.scheme);
    if (!scheme || scheme === 'file') return null;
    let entry = _fileSystemProviders.get(scheme);
    if (entry) return entry;
    await _activateKnownExtensionsForEvent(`onFileSystem:${scheme}`);
    entry = _fileSystemProviders.get(scheme);
    if (!entry) {
        throw new Error(`No filesystem provider registered for scheme: ${scheme}`);
    }
    return entry;
}

async function _callFileSystemProvider(uri, methodNames, args, fallback) {
    const entry = await _fileSystemProviderForUri(uri);
    if (!entry) return fallback();
    for (const name of methodNames) {
        const method = entry.provider && entry.provider[name];
        if (typeof method === 'function') {
            return await method.apply(entry.provider, args);
        }
    }
    throw new Error(`Filesystem provider for ${uri.scheme} is missing ${methodNames[0]}`);
}

async function _workspaceFsReadFile(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const value = await _callFileSystemProvider(
        uri,
        ['readFile', 'read_file'],
        [uri],
        () => fsp.readFile(uri.fsPath),
    );
    return _contentToUint8Array(value);
}

async function _workspaceFsWriteFile(uriInput, content) {
    const uri = _workspaceUriFromInput(uriInput);
    const bytes = _contentToUint8Array(content);
    return _callFileSystemProvider(
        uri,
        ['writeFile', 'write_file'],
        [uri, bytes, { create: true, overwrite: true }],
        async () => {
            await fsp.writeFile(uri.fsPath, bytes);
            const cached = _workspaceTextDocuments.get(uri.toString());
            if (cached) {
                const oldText = cached.getText();
                const text = _workspaceContentToText(bytes);
                _workspaceSetDocumentText(uri, text, _languageIdForUri(uri), [{
                    range: _workspaceFullDocumentRange(oldText),
                    rangeOffset: 0,
                    rangeLength: oldText.length,
                    text,
                }]);
            }
        },
    );
}

async function _workspaceFsStat(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const value = await _callFileSystemProvider(
        uri,
        ['stat'],
        [uri],
        async () => {
            const s = await fsp.stat(uri.fsPath);
            return {
                type: s.isDirectory() ? 2 : 1,
                size: s.size,
                ctime: s.ctimeMs,
                mtime: s.mtimeMs,
            };
        },
    );
    return Object.assign({ type: 0, size: 0, ctime: 0, mtime: 0 }, value || {});
}

async function _workspaceFsReadDirectory(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    return _callFileSystemProvider(
        uri,
        ['readDirectory', 'read_directory'],
        [uri],
        () => fsp.readdir(uri.fsPath, { withFileTypes: true })
            .then(ents => ents.map(e => [e.name, e.isDirectory() ? 2 : 1])),
    );
}

async function _workspaceFsCreateDirectory(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    return _callFileSystemProvider(
        uri,
        ['createDirectory', 'create_directory'],
        [uri],
        () => fsp.mkdir(uri.fsPath, { recursive: true }),
    );
}

async function _workspaceFsDelete(uriInput, options) {
    const uri = _workspaceUriFromInput(uriInput);
    return _callFileSystemProvider(
        uri,
        ['delete', 'deleteFile', 'delete_file'],
        [uri, options || {}],
        async () => {
            await fsp.rm(uri.fsPath, { force: true });
            _workspaceCloseTextDocument(uri);
        },
    );
}

async function _workspaceFsRename(srcInput, dstInput, options) {
    const src = _workspaceUriFromInput(srcInput);
    const dst = _workspaceUriFromInput(dstInput);
    if (src.scheme !== dst.scheme) {
        throw new Error('rename across filesystem providers is unsupported');
    }
    return _callFileSystemProvider(
        src,
        ['rename', 'renameFile', 'rename_file'],
        [src, dst, options || {}],
        async () => {
            await fsp.rename(src.fsPath, dst.fsPath);
            const cached = _workspaceCloseTextDocument(src);
            if (cached) {
                const text = await fsp.readFile(dst.fsPath, 'utf8');
                _workspaceStoreTextDocument(_createLanguageDocument({
                    uri: dst,
                    text,
                    languageId: _languageIdForUri(dst),
                    version: Date.now(),
                }), true);
            }
        },
    );
}

async function _workspaceFsCopy(srcInput, dstInput, options) {
    const src = _workspaceUriFromInput(srcInput);
    const dst = _workspaceUriFromInput(dstInput);
    if (src.scheme !== dst.scheme) {
        throw new Error('copy across filesystem providers is unsupported');
    }
    const entry = await _fileSystemProviderForUri(src);
    if (entry) {
        const copy = entry.provider && entry.provider.copy;
        if (typeof copy === 'function') {
            return await copy.call(entry.provider, src, dst, options || {});
        }
        const content = await _workspaceFsReadFile(src);
        return _workspaceFsWriteFile(dst, content);
    }
    return fsp.copyFile(src.fsPath, dst.fsPath);
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
        const uri = new Uri('untitled', '', `/Untitled-${_nextUntitledDocument++}`, '', '');
        const doc = _createLanguageDocument({
            uri,
            text: String(uriOrPath.content || ''),
            languageId: uriOrPath.language || 'plaintext',
            version: Date.now(),
        });
        return _workspaceStoreTextDocument(doc, true);
    }
    const uri = _workspaceUriFromInput(uriOrPath);
    const contentEntry = _textDocumentContentProviders.get(
        _normalizeFileSystemScheme(uri.scheme));
    if (contentEntry) {
        return _workspaceOpenTextDocumentFromContentProvider(
            uri, contentEntry, true);
    }
    const cached = _workspaceTextDocuments.get(uri.toString());
    if (cached) return _workspacePromoteTextDocument(cached);
    const providerText = await _workspaceFileSystemDocumentText(uri);
    const text = providerText === undefined
        ? (uri.scheme === 'file' ? await fsp.readFile(uri.fsPath, 'utf8') : '')
        : providerText;
    const doc = _createLanguageDocument({
        uri,
        text,
        languageId: _languageIdForUri(uri),
        version: Date.now(),
    });
    return _workspaceStoreTextDocument(doc, true);
}

async function _workspaceFileSystemDocumentText(uri) {
    const scheme = _normalizeFileSystemScheme(uri?.scheme);
    if (!scheme || scheme === 'file' || scheme === 'untitled') {
        return undefined;
    }
    let entry = _fileSystemProviders.get(scheme);
    if (!entry) {
        await _activateKnownExtensionsForEvent(`onFileSystem:${scheme}`);
        entry = _fileSystemProviders.get(scheme);
    }
    if (!entry) return undefined;
    const content = await _workspaceFsReadFile(uri);
    return _workspaceContentToText(content);
}

function _textDocumentProviderToken() {
    return {
        isCancellationRequested: false,
        onCancellationRequested: new EventEmitter().event,
    };
}

async function _provideTextDocumentContent(uri, entry) {
    const provider = entry && entry.provider;
    if (!provider || typeof provider.provideTextDocumentContent !== 'function') {
        throw new Error(`No text document content provider for scheme: ${uri.scheme}`);
    }
    const value = await provider.provideTextDocumentContent.call(
        provider, uri, _textDocumentProviderToken());
    return value === undefined || value === null ? '' : String(value);
}

async function _workspaceOpenTextDocumentFromContentProvider(uri, entry, fireOpen) {
    const text = await _provideTextDocumentContent(uri, entry);
    const existing = _workspaceTextDocuments.get(uri.toString());
    if (existing && typeof existing._setText === 'function') {
        existing._setText(text, Date.now());
        existing.isDirty = false;
        existing.__contentProviderScheme = _normalizeFileSystemScheme(uri.scheme);
        return fireOpen ? _workspacePromoteTextDocument(existing) : existing;
    }
    const doc = _createLanguageDocument({
        uri,
        text,
        languageId: _languageIdForUri(uri),
        version: Date.now(),
    });
    doc.isDirty = false;
    doc.__contentProviderScheme = _normalizeFileSystemScheme(uri.scheme);
    return _workspaceStoreTextDocument(doc, fireOpen);
}

async function _refreshTextDocumentContentProvider(scheme, uriLike) {
    const uri = _workspaceUriFromInput(uriLike);
    const normalized = _normalizeFileSystemScheme(scheme);
    if (_normalizeFileSystemScheme(uri.scheme) !== normalized) {
        log(`content provider ${normalized} ignored change for ${uri.scheme}`);
        return;
    }
    const key = uri.toString();
    const doc = _workspaceTextDocuments.get(key);
    if (!_workspaceDocumentIsOpened(doc)) return;
    const entry = _textDocumentContentProviders.get(normalized);
    if (!entry) return;
    const previousText = doc.getText();
    const nextText = await _provideTextDocumentContent(uri, entry);
    if (nextText === previousText) return;
    if (typeof doc._setText === 'function') {
        doc._setText(nextText, Date.now());
    }
    doc.isDirty = false;
    _onDidChangeTextDocumentEmitter.fire({
        document: doc,
        contentChanges: [{
            range: _workspaceFullDocumentRange(previousText),
            rangeOffset: 0,
            rangeLength: previousText.length,
            text: nextText,
        }],
    });
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

function _workspaceEditKind(entry) {
    if (!entry || typeof entry !== 'object') return 'text';
    const kind = String(entry.kind || entry.type || entry.operation || '').toLowerCase();
    if (kind === 'createfile' || kind === 'create') return 'create';
    if (kind === 'deletefile' || kind === 'delete') return 'delete';
    if (kind === 'renamefile' || kind === 'rename') return 'rename';
    return 'text';
}

function _workspaceEditUri(entry) {
    return _workspaceUriFromInput(entry && (
        entry.uri || entry.resource || entry.path || entry.oldUri || entry.oldResource));
}

function _workspaceEditTargetUri(entry) {
    return _workspaceUriFromInput(entry && (
        entry.newUri || entry.target || entry.to || entry.newResource || entry.destination));
}

function _workspaceOpenDetail(uriOrPath) {
    if (uriOrPath && typeof uriOrPath === 'object'
        && !(uriOrPath instanceof Uri)
        && (uriOrPath.content !== undefined || uriOrPath.language !== undefined)) {
        return `untitled:${uriOrPath.language || 'plaintext'}`;
    }
    return _workspaceRelativePath(uriOrPath);
}

function _workspaceContentToText(content) {
    if (content === undefined || content === null) return '';
    if (typeof content === 'string') return content;
    if (Buffer.isBuffer(content)) return content.toString('utf8');
    if (content instanceof Uint8Array || Array.isArray(content)) {
        return Buffer.from(content).toString('utf8');
    }
    return String(content);
}

function _workspaceStoreTextDocument(doc, fireOpen) {
    if (!doc || !doc.uri) return doc;
    const key = doc.uri.toString();
    const existing = _workspaceTextDocuments.get(key);
    const wasOpen = _workspaceDocumentIsOpened(existing);
    if (fireOpen) {
        doc.__opened = true;
    } else if (doc.__opened === undefined) {
        doc.__opened = wasOpen;
    }
    _workspaceTextDocuments.set(key, doc);
    if (fireOpen && !wasOpen) _onDidOpenTextDocumentEmitter.fire(doc);
    return doc;
}

function _workspaceCloseTextDocument(uriOrDoc) {
    const uri = uriOrDoc && uriOrDoc.uri ? uriOrDoc.uri : _workspaceUriFromInput(uriOrDoc);
    if (!uri) return undefined;
    const key = uri.toString();
    const doc = _workspaceTextDocuments.get(key);
    if (!doc) return undefined;
    _workspaceTextDocuments.delete(key);
    if (_workspaceDocumentIsOpened(doc)) _onDidCloseTextDocumentEmitter.fire(doc);
    return doc;
}

function _workspaceRetargetTextDocument(document, targetUri) {
    if (!document || !targetUri) return document;
    const oldKey = document.uri ? document.uri.toString() : '';
    if (oldKey) _workspaceTextDocuments.delete(oldKey);
    document.uri = targetUri;
    document.fileName = targetUri.scheme === 'file' ? targetUri.fsPath : targetUri.toString();
    document.languageId = _languageIdForUri(targetUri);
    document.isUntitled = targetUri.scheme === 'untitled';
    return _workspaceStoreTextDocument(document, false);
}

function _workspaceFullDocumentRange(text) {
    return new Range(0, 0, _positionAt(text, text.length).line, _positionAt(text, text.length).character);
}

function _workspaceSetDocumentText(uri, text, languageId, contentChanges) {
    const key = uri.toString();
    let doc = _workspaceTextDocuments.get(key);
    const version = Date.now();
    if (doc && typeof doc._setText === 'function') {
        doc._setText(text, version);
    } else {
        doc = _createLanguageDocument({
            uri,
            text,
            languageId: languageId || _languageIdForUri(uri),
            version,
        });
        doc.__opened = false;
        _workspaceTextDocuments.set(key, doc);
    }
    const changes = Array.isArray(contentChanges) ? contentChanges : [{
        range: _workspaceFullDocumentRange(text),
        rangeOffset: 0,
        rangeLength: text.length,
        text,
    }];
    doc.isDirty = true;
    if (_workspaceDocumentIsOpened(doc)) {
        _onDidChangeTextDocumentEmitter.fire({
            document: doc,
            contentChanges: changes,
        });
    }
    _markCustomTextEditorsDirty(doc, 'textChange');
    return doc;
}

async function _workspacePathExists(fsPath) {
    try {
        await fsp.stat(fsPath);
        return true;
    } catch (err) {
        if (err && err.code === 'ENOENT') return false;
        throw err;
    }
}

async function _workspaceApplyFileOperation(entry) {
    const kind = _workspaceEditKind(entry);
    const options = entry && typeof entry.options === 'object' ? entry.options : {};
    const uri = _workspaceEditUri(entry);
    if (!uri || uri.scheme !== 'file') return false;
    if (kind === 'create') {
        const exists = await _workspacePathExists(uri.fsPath);
        if (exists && options.ignoreIfExists) return true;
        if (exists && !options.overwrite) return false;
        await fsp.mkdir(path.dirname(uri.fsPath), { recursive: true });
        await fsp.writeFile(uri.fsPath, '', 'utf8');
        _workspaceCloseTextDocument(uri);
        return true;
    }
    if (kind === 'delete') {
        const exists = await _workspacePathExists(uri.fsPath);
        if (!exists && options.ignoreIfNotExists) return true;
        if (!exists) return false;
        await fsp.rm(uri.fsPath, {
            recursive: !!options.recursive,
            force: !!options.ignoreIfNotExists,
        });
        _workspaceCloseTextDocument(uri);
        return true;
    }
    if (kind === 'rename') {
        const target = _workspaceEditTargetUri(entry);
        if (!target || target.scheme !== 'file') return false;
        const sourceExists = await _workspacePathExists(uri.fsPath);
        if (!sourceExists) return false;
        const targetExists = await _workspacePathExists(target.fsPath);
        if (targetExists && options.ignoreIfExists) return true;
        if (targetExists && !options.overwrite) return false;
        if (targetExists && options.overwrite) {
            await fsp.rm(target.fsPath, { recursive: true, force: true });
        }
        await fsp.mkdir(path.dirname(target.fsPath), { recursive: true });
        const oldKey = uri.toString();
        let cached = _workspaceTextDocuments.get(oldKey);
        await fsp.rename(uri.fsPath, target.fsPath);
        if (cached) {
            _workspaceTextDocuments.delete(oldKey);
            if (cached.isDirty) {
                _workspaceRetargetTextDocument(cached, target);
            } else {
                const text = await fsp.readFile(target.fsPath, 'utf8');
                if (typeof cached._setText === 'function') {
                    cached._setText(text, Date.now());
                    cached.isDirty = false;
                    _workspaceRetargetTextDocument(cached, target);
                } else {
                    _workspaceStoreTextDocument(_createLanguageDocument({
                        uri: target,
                        text,
                        languageId: _languageIdForUri(target),
                        version: Date.now(),
                    }), _workspaceDocumentIsOpened(cached));
                }
            }
        }
        return true;
    }
    return false;
}

async function _workspaceApplyEdit(edit) {
    const entries = _workspaceEditEntries(edit);
    if (!entries.length) return true;
    const grouped = new Map();
    async function flushTextEdits() {
        try {
            for (const group of grouped.values()) {
                const cachedDoc = _workspaceTextDocuments.get(group.uri.toString());
                let text = cachedDoc ? cachedDoc.getText() : '';
                if (!cachedDoc) {
                    try { text = await fsp.readFile(group.uri.fsPath, 'utf8'); }
                    catch (err) {
                        if (!err || err.code !== 'ENOENT') throw err;
                    }
                }
                const edits = group.edits.slice().sort((a, b) => {
                    const ao = _offsetAt(text, a.range.start);
                    const bo = _offsetAt(text, b.range.start);
                    if (ao !== bo) return bo - ao;
                    return _offsetAt(text, b.range.end) - _offsetAt(text, a.range.end);
                });
                const contentChanges = [];
                for (const item of edits) {
                    const start = _offsetAt(text, item.range.start);
                    const end = _offsetAt(text, item.range.end);
                    contentChanges.unshift({
                        range: item.range,
                        rangeOffset: start,
                        rangeLength: Math.max(0, end - start),
                        text: item.text,
                    });
                    text = text.slice(0, start) + item.text + text.slice(end);
                }
                if (cachedDoc) {
                    _workspaceSetDocumentText(
                        group.uri, text, _languageIdForUri(group.uri), contentChanges);
                } else {
                    const hiddenDoc = _createLanguageDocument({
                        uri: group.uri,
                        text,
                        languageId: _languageIdForUri(group.uri),
                        version: Date.now(),
                    });
                    hiddenDoc.__opened = false;
                    _workspaceStoreTextDocument(hiddenDoc, false);
                    _workspaceSetDocumentText(
                        group.uri, text, _languageIdForUri(group.uri), contentChanges);
                }
            }
            grouped.clear();
            return true;
        } catch (err) {
            log(`workspace.applyEdit failed: ${err && err.message || err}`);
            return false;
        }
    }
    for (const entry of entries) {
        const kind = _workspaceEditKind(entry);
        if (kind !== 'text') {
            if (!(await flushTextEdits())) return false;
            try {
                if (!(await _workspaceApplyFileOperation(entry))) return false;
            } catch (err) {
                log(`workspace.applyEdit file operation failed: ${err && err.message || err}`);
                return false;
            }
            continue;
        }
        const uri = _workspaceEditUri(entry);
        if (!uri || uri.scheme !== 'file' || !entry.range) return false;
        const key = uri.toString();
        if (!grouped.has(key)) grouped.set(key, { uri, edits: [] });
        grouped.get(key).edits.push({
            range: _rangeFromPayload(entry.range),
            text: _workspaceEditText(entry),
        });
    }
    return flushTextEdits();
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
    let version = Number(msg.version || 1);
    let text = '';
    if (Object.prototype.hasOwnProperty.call(msg, 'text')) {
        text = String(msg.text ?? '');
        _languageDocumentTextCache.set(uriKey, { version, text });
    } else {
        const cached = _languageDocumentTextCache.get(uriKey);
        text = cached && cached.version === version ? cached.text : '';
    }
    const languageId = String(msg.languageId || _languageIdForUri(uri));
    const document = {
        uri,
        fileName: uri.scheme === 'file' ? uri.fsPath : uri.toString(),
        languageId,
        get version() { return version; },
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
        async save() {
            const saveUri = document.uri || uri;
            if (saveUri.scheme === 'file') {
                await fsp.mkdir(path.dirname(saveUri.fsPath), { recursive: true });
                await fsp.writeFile(saveUri.fsPath, text, 'utf8');
            }
            document.isDirty = false;
            document.isUntitled = false;
            _onDidSaveTextDocumentEmitter.fire(document);
            _markCustomTextEditorsSaved(document, 'save');
            return true;
        },
        _setText(nextText, nextVersion) {
            text = String(nextText ?? '');
            version = Number(nextVersion || Date.now());
            const textKey = document.uri ? document.uri.toString() : uriKey;
            _languageDocumentTextCache.set(textKey, { version, text });
        },
    };
    return document;
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

function _extensionKindValue(raw) {
    const value = Array.isArray(raw) ? raw[0] : raw;
    return String(value || '').toLowerCase() === 'ui' ? 1 : 2;
}

function _registerKnownExtensions(extensions) {
    for (const item of Array.isArray(extensions) ? extensions : []) {
        const id = String(item?.extensionId || item?.id || '').trim();
        const extensionPath = String(item?.extensionPath || item?.path || '').trim();
        if (!id || !extensionPath) continue;
        const manifest = item?.manifest && typeof item.manifest === 'object'
            ? item.manifest
            : {};
        _knownExtensions.set(id, {
            extensionId: id,
            extensionPath,
            manifest,
            storageRoot: String(item?.storageRoot || ''),
            extensionKind: item?.extensionKind || manifest.extensionKind || 'workspace',
        });
        _registerConfigurationDefaultsFromManifest(manifest);
    }
}

function _completeExtensionActivation(extensionId, ok, error) {
    const id = String(extensionId || '');
    const pending = _extensionActivationInFlight.get(id);
    if (!pending) return;
    _extensionActivationInFlight.delete(id);
    _extensionActivationRequests.delete(pending.requestId);
    clearTimeout(pending.timer);
    if (ok) {
        const active = _extensions.get(id);
        pending.resolve(active ? active.activationExports : undefined);
    } else {
        pending.reject(new Error(error || `Extension activation failed: ${id}`));
    }
}

function _handleActivateExtensionResponse(msg) {
    const requestId = String(msg.requestId || '');
    const pending = _extensionActivationRequests.get(requestId);
    if (!pending) return;
    if (!msg.ok) {
        _completeExtensionActivation(
            pending.extensionId,
            false,
            msg.error || 'Extension activation request failed');
    }
}

async function _activateKnownExtension(id, visiting = new Set()) {
    const extensionId = String(id || '');
    const active = _extensions.get(extensionId);
    if (active) return active.activationExports;
    if (!_knownExtensions.has(extensionId)) {
        throw new Error(`Unknown extension: ${extensionId}`);
    }
    const existing = _extensionActivationInFlight.get(extensionId);
    if (existing) return existing.promise;
    if (visiting.has(extensionId)) return undefined;
    visiting.add(extensionId);

    const known = _knownExtensions.get(extensionId);
    const dependencyIds = Array.isArray(known?.manifest?.extensionDependencies)
        ? known.manifest.extensionDependencies
        : [];
    for (const depId of dependencyIds) {
        const depKey = String(depId || '');
        if (!depKey || !_knownExtensions.has(depKey)) continue;
        await _activateKnownExtension(depKey, visiting);
    }
    visiting.delete(extensionId);

    const requestId = `extact-${_nextExtensionActivationRequestHandle++}`;
    let resolvePromise;
    let rejectPromise;
    const promise = new Promise((resolve, reject) => {
        resolvePromise = resolve;
        rejectPromise = reject;
    });
    const pending = {
        requestId,
        extensionId,
        promise,
        resolve: resolvePromise,
        reject: rejectPromise,
        timer: setTimeout(() => {
            _completeExtensionActivation(
                extensionId,
                false,
                `Extension activation timed out: ${extensionId}`);
        }, 5000),
    };
    _extensionActivationRequests.set(requestId, pending);
    _extensionActivationInFlight.set(extensionId, pending);
    send({ type: 'activate_extension', requestId, extensionId });
    return promise;
}

function _activationEventMatches(events, event) {
    if (!Array.isArray(events)) return false;
    if (events.includes(event)) return true;
    if (event.includes(':')) {
        const wildcard = event.split(':')[0] + ':*';
        return events.includes(wildcard);
    }
    return false;
}

async function _activateKnownExtensionsForEvent(event) {
    const targets = [];
    for (const [extensionId, known] of _knownExtensions.entries()) {
        if (_extensions.has(extensionId)) continue;
        if (_activationEventMatches(known?.manifest?.activationEvents, event)) {
            targets.push(extensionId);
        }
    }
    for (const extensionId of targets) {
        try {
            await _activateKnownExtension(extensionId);
        } catch (err) {
            log(`activation event ${event} failed for ${extensionId}: ${err.message}`);
        }
    }
    return targets.length;
}

function _extensionIdKey(value) {
    return String(value || '').trim().toLowerCase();
}

function _targetExtensionIdForUri(uri) {
    const parsed = _workspaceUriFromInput(uri);
    if (parsed.scheme !== 'vscode' && parsed.scheme !== 'sao-ai-editor') {
        return '';
    }
    const authority = _extensionIdKey(parsed.authority);
    if (authority) return authority;
    const firstPathPart = String(parsed.path || '')
        .replace(/^\/+/, '')
        .split('/')[0];
    return _extensionIdKey(firstPathPart);
}

async function _dispatchUriHandler(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const targetId = _targetExtensionIdForUri(uri);
    if (!targetId) return false;
    let entry = _uriHandlers.get(targetId);
    if (!entry) {
        await _activateKnownExtensionsForEvent(`onUri:${targetId}`);
        entry = _uriHandlers.get(targetId);
    }
    const handler = entry && entry.handler;
    if (!handler || typeof handler.handleUri !== 'function') {
        return false;
    }
    await handler.handleUri(uri);
    return true;
}

function _quickPickItemIsSeparator(item) {
    return !!(
        item
        && typeof item === 'object'
        && item.kind === 1
    );
}

async function _windowShowQuickPick(itemsOrPromise, options = {}, token = undefined) {
    const rawItems = await Promise.resolve(itemsOrPromise);
    const items = Array.isArray(rawItems) ? Array.from(rawItems) : [];
    const pickable = items.filter(item => !_quickPickItemIsSeparator(item));
    if (!pickable.length) return undefined;
    if (token?.isCancellationRequested) return undefined;
    const input = new QuickPickInput();
    const picked = pickable.filter(item => item && typeof item === 'object' && item.picked);
    const initialActive = picked[0] || pickable[0];
    let settled = false;
    const disposables = [];
    function notifySelected(item) {
        if (item === undefined || typeof options?.onDidSelectItem !== 'function') return;
        try { options.onDidSelectItem(item); }
        catch (err) { log(`showQuickPick onDidSelectItem failed: ${err.message}`); }
    }
    function finish(resolve, value) {
        if (settled) return;
        settled = true;
        resolve(value);
        queueMicrotask(() => {
            disposables.forEach(disposable => disposable?.dispose?.());
            input.dispose();
        });
    }
    return new Promise(resolve => {
        input._title = options?.title === undefined ? undefined : String(options.title);
        input._placeholder = options?.placeHolder === undefined ? undefined : String(options.placeHolder);
        input._prompt = options?.prompt === undefined ? undefined : String(options.prompt);
        input._ignoreFocusOut = !!options?.ignoreFocusOut;
        input._canSelectMany = !!options?.canPickMany;
        input._matchOnDescription = !!options?.matchOnDescription;
        input._matchOnDetail = !!options?.matchOnDetail;
        input._items = items;
        input._activeItems = initialActive === undefined ? [] : [initialActive];
        input._selectedItems = input._canSelectMany
            ? (picked.length ? picked : [pickable[0]])
            : (picked[0] ? [picked[0]] : []);
        disposables.push(input.onDidChangeActive(activeItems => {
            notifySelected(Array.isArray(activeItems) ? activeItems[0] : undefined);
        }));
        disposables.push(input.onDidAccept(() => {
            if (input._canSelectMany) {
                finish(resolve, input.selectedItems.filter(item => !_quickPickItemIsSeparator(item)));
                return;
            }
            const active = (Array.isArray(input.activeItems) ? input.activeItems[0] : undefined)
                || (Array.isArray(input.selectedItems) ? input.selectedItems[0] : undefined);
            finish(resolve, active);
        }));
        disposables.push(input.onDidHide(() => finish(resolve, undefined)));
        if (token && typeof token.onCancellationRequested === 'function') {
            disposables.push(token.onCancellationRequested(() => {
                input.hide();
                finish(resolve, undefined);
            }));
        }
        input.show();
        notifySelected(initialActive);
    });
}

async function _windowShowInputBox(options = {}, token = undefined) {
    if (token?.isCancellationRequested) return undefined;
    const input = new InputBoxInput();
    let settled = false;
    let validationRun = 0;
    const disposables = [];
    function finish(resolve, value) {
        if (settled) return;
        settled = true;
        resolve(value);
        queueMicrotask(() => {
            disposables.forEach(disposable => disposable?.dispose?.());
            input.dispose();
        });
    }
    async function validateCurrentValue() {
        if (typeof options?.validateInput !== 'function') return undefined;
        const run = ++validationRun;
        let validation;
        try {
            validation = await options.validateInput(String(input._value ?? ''));
        } catch (err) {
            validation = String(err?.message || err || 'Invalid input');
            log(`showInputBox validateInput failed: ${validation}`);
        }
        if (run === validationRun && !settled) {
            input.validationMessage = validation || undefined;
        }
        return validation;
    }
    return new Promise(resolve => {
        input._title = options?.title === undefined ? undefined : String(options.title);
        input._value = String(options?.value ?? '');
        input._valueSelection = Array.isArray(options?.valueSelection)
            ? [Number(options.valueSelection[0]) || 0, Number(options.valueSelection[1]) || 0]
            : undefined;
        input._placeholder = options?.placeHolder === undefined ? undefined : String(options.placeHolder);
        input._prompt = options?.prompt === undefined ? undefined : String(options.prompt);
        input._password = !!options?.password;
        input._ignoreFocusOut = !!options?.ignoreFocusOut;
        disposables.push(input.onDidChangeValue(() => { void validateCurrentValue(); }));
        disposables.push(input.onDidAccept(async () => {
            const validation = await validateCurrentValue();
            if (validation) return;
            finish(resolve, input.value);
        }));
        disposables.push(input.onDidHide(() => finish(resolve, undefined)));
        if (token && typeof token.onCancellationRequested === 'function') {
            disposables.push(token.onCancellationRequested(() => {
                input.hide();
                finish(resolve, undefined);
            }));
        }
        input.show();
    });
}

async function _windowShowWorkspaceFolderPick(options = {}, token = undefined) {
    const folders = [_workspaceFolder()].filter(folder => folder && folder.uri);
    if (!folders.length || token?.isCancellationRequested) return undefined;
    const items = folders.map(folder => ({
        label: folder.name || path.basename(folder.uri.fsPath || folder.uri.path || ''),
        description: folder.uri.fsPath || folder.uri.toString(),
        folder,
    }));
    const picked = await _windowShowQuickPick(items, {
        placeHolder: options?.placeHolder || 'Select workspace folder',
        ignoreFocusOut: !!options?.ignoreFocusOut,
    }, token);
    return picked && picked.folder ? picked.folder : undefined;
}

function _quickInputSafePayload(value) {
    try {
        return _serializeLanguageValue(value);
    } catch {
        if (Array.isArray(value)) return value.map(item => _quickInputSafePayload(item));
        if (value && typeof value === 'object') {
            const result = {};
            for (const [key, item] of Object.entries(value)) {
                if (typeof item !== 'function') result[key] = String(item);
            }
            return result;
        }
        return value === undefined ? null : value;
    }
}

function _quickInputEventPayload(input, event, extra = {}) {
    return {
        type: 'quick_input',
        event,
        id: input._id,
        kind: input._kind,
        visible: !!input._visible,
        state: _quickInputSafePayload(input._snapshot()),
        ..._quickInputSafePayload(extra),
    };
}

class QuickInputBase {
    constructor(kind) {
        this._kind = kind;
        this._id = `quick-input-${_nextQuickInputHandle++}`;
        this._visible = false;
        this._disposed = false;
        this._title = undefined;
        this._step = undefined;
        this._totalSteps = undefined;
        this._enabled = true;
        this._busy = false;
        this._ignoreFocusOut = false;
        this._onDidHideEmitter = new EventEmitter();
        this.onDidHide = this._onDidHideEmitter.event;
        _quickInputs.set(this._id, this);
    }
    _assertAlive() {
        if (this._disposed) throw new Error('QuickInput has been disposed');
    }
    _send(event, extra = {}) {
        try { send(_quickInputEventPayload(this, event, extra)); } catch {}
    }
    get title() { this._assertAlive(); return this._title; }
    set title(value) {
        this._assertAlive();
        this._title = value === undefined ? undefined : String(value);
        this._send('update', { changed: 'title' });
    }
    get step() { this._assertAlive(); return this._step; }
    set step(value) {
        this._assertAlive();
        const next = Number(value);
        this._step = Number.isFinite(next) ? next : undefined;
        this._send('update', { changed: 'step' });
    }
    get totalSteps() { this._assertAlive(); return this._totalSteps; }
    set totalSteps(value) {
        this._assertAlive();
        const next = Number(value);
        this._totalSteps = Number.isFinite(next) ? next : undefined;
        this._send('update', { changed: 'totalSteps' });
    }
    get enabled() { this._assertAlive(); return this._enabled; }
    set enabled(value) {
        this._assertAlive();
        this._enabled = value !== false;
        this._send('update', { changed: 'enabled' });
    }
    get busy() { this._assertAlive(); return this._busy; }
    set busy(value) {
        this._assertAlive();
        this._busy = !!value;
        this._send('update', { changed: 'busy' });
    }
    get ignoreFocusOut() { this._assertAlive(); return this._ignoreFocusOut; }
    set ignoreFocusOut(value) {
        this._assertAlive();
        this._ignoreFocusOut = !!value;
        this._send('update', { changed: 'ignoreFocusOut' });
    }
    show() {
        this._assertAlive();
        if (_activeQuickInput && _activeQuickInput !== this) {
            _activeQuickInput.hide();
        }
        _activeQuickInput = this;
        this._visible = true;
        this._send('show');
    }
    hide() {
        this._assertAlive();
        if (!this._visible) return;
        this._visible = false;
        if (_activeQuickInput === this) _activeQuickInput = null;
        this._send('hide');
        this._onDidHideEmitter.fire();
    }
    dispose() {
        if (this._disposed) return;
        if (this._visible) this.hide();
        this._disposed = true;
        if (_activeQuickInput === this) _activeQuickInput = null;
        _quickInputs.delete(this._id);
        this._send('dispose');
        this._onDidHideEmitter.dispose();
    }
    _snapshot() {
        return {
            title: this._title,
            step: this._step,
            totalSteps: this._totalSteps,
            enabled: this._enabled,
            busy: this._busy,
            ignoreFocusOut: this._ignoreFocusOut,
        };
    }
}

class QuickPickInput extends QuickInputBase {
    constructor() {
        super('quickPick');
        this._value = '';
        this._placeholder = undefined;
        this._prompt = undefined;
        this._buttons = [];
        this._items = [];
        this._canSelectMany = false;
        this._matchOnDescription = false;
        this._matchOnDetail = false;
        this._keepScrollPosition = false;
        this._activeItems = [];
        this._selectedItems = [];
        this._onDidChangeValueEmitter = new EventEmitter();
        this._onDidAcceptEmitter = new EventEmitter();
        this._onDidTriggerButtonEmitter = new EventEmitter();
        this._onDidTriggerItemButtonEmitter = new EventEmitter();
        this._onDidChangeActiveEmitter = new EventEmitter();
        this._onDidChangeSelectionEmitter = new EventEmitter();
        this.onDidChangeValue = this._onDidChangeValueEmitter.event;
        this.onDidAccept = this._onDidAcceptEmitter.event;
        this.onDidTriggerButton = this._onDidTriggerButtonEmitter.event;
        this.onDidTriggerItemButton = this._onDidTriggerItemButtonEmitter.event;
        this.onDidChangeActive = this._onDidChangeActiveEmitter.event;
        this.onDidChangeSelection = this._onDidChangeSelectionEmitter.event;
    }
    get value() { this._assertAlive(); return this._value; }
    set value(value) {
        this._assertAlive();
        const next = String(value ?? '');
        if (next === this._value) return;
        this._value = next;
        this._onDidChangeValueEmitter.fire(this._value);
        this._send('changeValue', { value: this._value });
    }
    get placeholder() { this._assertAlive(); return this._placeholder; }
    set placeholder(value) {
        this._assertAlive();
        this._placeholder = value === undefined ? undefined : String(value);
        this._send('update', { changed: 'placeholder' });
    }
    get prompt() { this._assertAlive(); return this._prompt; }
    set prompt(value) {
        this._assertAlive();
        this._prompt = value === undefined ? undefined : String(value);
        this._send('update', { changed: 'prompt' });
    }
    get buttons() { this._assertAlive(); return this._buttons; }
    set buttons(value) {
        this._assertAlive();
        this._buttons = Array.isArray(value) ? Array.from(value) : [];
        this._send('update', { changed: 'buttons' });
    }
    get items() { this._assertAlive(); return this._items; }
    set items(value) {
        this._assertAlive();
        this._items = Array.isArray(value) ? Array.from(value) : [];
        this._send('update', { changed: 'items' });
    }
    get canSelectMany() { this._assertAlive(); return this._canSelectMany; }
    set canSelectMany(value) {
        this._assertAlive();
        this._canSelectMany = !!value;
        this._send('update', { changed: 'canSelectMany' });
    }
    get matchOnDescription() { this._assertAlive(); return this._matchOnDescription; }
    set matchOnDescription(value) {
        this._assertAlive();
        this._matchOnDescription = !!value;
        this._send('update', { changed: 'matchOnDescription' });
    }
    get matchOnDetail() { this._assertAlive(); return this._matchOnDetail; }
    set matchOnDetail(value) {
        this._assertAlive();
        this._matchOnDetail = !!value;
        this._send('update', { changed: 'matchOnDetail' });
    }
    get keepScrollPosition() { this._assertAlive(); return this._keepScrollPosition; }
    set keepScrollPosition(value) {
        this._assertAlive();
        this._keepScrollPosition = !!value;
        this._send('update', { changed: 'keepScrollPosition' });
    }
    get activeItems() { this._assertAlive(); return this._activeItems; }
    set activeItems(value) {
        this._assertAlive();
        this._activeItems = Array.isArray(value) ? Array.from(value) : [];
        this._onDidChangeActiveEmitter.fire(this._activeItems);
        this._send('changeActive', { activeItems: this._activeItems });
    }
    get selectedItems() { this._assertAlive(); return this._selectedItems; }
    set selectedItems(value) {
        this._assertAlive();
        this._selectedItems = Array.isArray(value) ? Array.from(value) : [];
        this._onDidChangeSelectionEmitter.fire(this._selectedItems);
        this._send('changeSelection', { selectedItems: this._selectedItems });
    }
    _accept() {
        this._assertAlive();
        this._onDidAcceptEmitter.fire();
        this._send('accept');
    }
    _triggerButton(button) {
        this._assertAlive();
        this._onDidTriggerButtonEmitter.fire(button);
        this._send('triggerButton', { button });
    }
    _triggerItemButton(item, button) {
        this._assertAlive();
        const event = { item, button };
        this._onDidTriggerItemButtonEmitter.fire(event);
        this._send('triggerItemButton', event);
    }
    dispose() {
        if (this._disposed) return;
        super.dispose();
        this._onDidChangeValueEmitter.dispose();
        this._onDidAcceptEmitter.dispose();
        this._onDidTriggerButtonEmitter.dispose();
        this._onDidTriggerItemButtonEmitter.dispose();
        this._onDidChangeActiveEmitter.dispose();
        this._onDidChangeSelectionEmitter.dispose();
    }
    _snapshot() {
        return {
            ...super._snapshot(),
            value: this._value,
            placeholder: this._placeholder,
            prompt: this._prompt,
            buttons: this._buttons,
            items: this._items,
            canSelectMany: this._canSelectMany,
            matchOnDescription: this._matchOnDescription,
            matchOnDetail: this._matchOnDetail,
            keepScrollPosition: this._keepScrollPosition,
            activeItems: this._activeItems,
            selectedItems: this._selectedItems,
        };
    }
}

class InputBoxInput extends QuickInputBase {
    constructor() {
        super('inputBox');
        this._value = '';
        this._valueSelection = undefined;
        this._placeholder = undefined;
        this._password = false;
        this._buttons = [];
        this._prompt = undefined;
        this._validationMessage = undefined;
        this._onDidChangeValueEmitter = new EventEmitter();
        this._onDidAcceptEmitter = new EventEmitter();
        this._onDidTriggerButtonEmitter = new EventEmitter();
        this.onDidChangeValue = this._onDidChangeValueEmitter.event;
        this.onDidAccept = this._onDidAcceptEmitter.event;
        this.onDidTriggerButton = this._onDidTriggerButtonEmitter.event;
    }
    get value() { this._assertAlive(); return this._value; }
    set value(value) {
        this._assertAlive();
        const next = String(value ?? '');
        if (next === this._value) return;
        this._value = next;
        this._onDidChangeValueEmitter.fire(this._value);
        this._send('changeValue', { value: this._value });
    }
    get valueSelection() { this._assertAlive(); return this._valueSelection; }
    set valueSelection(value) {
        this._assertAlive();
        this._valueSelection = Array.isArray(value)
            ? [Number(value[0]) || 0, Number(value[1]) || 0]
            : undefined;
        this._send('update', { changed: 'valueSelection' });
    }
    get placeholder() { this._assertAlive(); return this._placeholder; }
    set placeholder(value) {
        this._assertAlive();
        this._placeholder = value === undefined ? undefined : String(value);
        this._send('update', { changed: 'placeholder' });
    }
    get password() { this._assertAlive(); return this._password; }
    set password(value) {
        this._assertAlive();
        this._password = !!value;
        this._send('update', { changed: 'password' });
    }
    get buttons() { this._assertAlive(); return this._buttons; }
    set buttons(value) {
        this._assertAlive();
        this._buttons = Array.isArray(value) ? Array.from(value) : [];
        this._send('update', { changed: 'buttons' });
    }
    get prompt() { this._assertAlive(); return this._prompt; }
    set prompt(value) {
        this._assertAlive();
        this._prompt = value === undefined ? undefined : String(value);
        this._send('update', { changed: 'prompt' });
    }
    get validationMessage() { this._assertAlive(); return this._validationMessage; }
    set validationMessage(value) {
        this._assertAlive();
        this._validationMessage = value;
        this._send('update', { changed: 'validationMessage' });
    }
    _accept() {
        this._assertAlive();
        this._onDidAcceptEmitter.fire();
        this._send('accept');
    }
    _triggerButton(button) {
        this._assertAlive();
        this._onDidTriggerButtonEmitter.fire(button);
        this._send('triggerButton', { button });
    }
    dispose() {
        if (this._disposed) return;
        super.dispose();
        this._onDidChangeValueEmitter.dispose();
        this._onDidAcceptEmitter.dispose();
        this._onDidTriggerButtonEmitter.dispose();
    }
    _snapshot() {
        return {
            ...super._snapshot(),
            value: this._value,
            valueSelection: this._valueSelection,
            placeholder: this._placeholder,
            password: this._password,
            buttons: this._buttons,
            prompt: this._prompt,
            validationMessage: this._validationMessage,
        };
    }
}

function _quickInputItemForIndex(input, index) {
    const items = Array.isArray(input?._items) ? input._items : [];
    const i = Number(index);
    return Number.isInteger(i) && i >= 0 && i < items.length ? items[i] : undefined;
}

function _quickInputItemsForIndices(input, indices) {
    const source = Array.isArray(indices) ? indices : [];
    return source
        .map(index => _quickInputItemForIndex(input, index))
        .filter(item => item !== undefined);
}

function _quickInputButtonForIndex(input, index) {
    const buttons = Array.isArray(input?._buttons) ? input._buttons : [];
    const i = Number(index);
    return Number.isInteger(i) && i >= 0 && i < buttons.length ? buttons[i] : undefined;
}

function handleQuickInputAction(msg) {
    const id = String(msg.id || '');
    const action = String(msg.action || '');
    const input = _quickInputs.get(id);
    if (!input) {
        log(`quick_input_action: unknown id=${id}`);
        return;
    }
    try {
        if (action === 'changeValue') {
            input.value = String(msg.value ?? '');
        } else if (action === 'changeActive' && input instanceof QuickPickInput) {
            input.activeItems = _quickInputItemsForIndices(input, msg.itemIndices || msg.indices);
        } else if (action === 'changeSelection' && input instanceof QuickPickInput) {
            input.selectedItems = _quickInputItemsForIndices(input, msg.itemIndices || msg.indices);
        } else if (action === 'triggerButton') {
            const button = _quickInputButtonForIndex(input, msg.buttonIndex);
            if (button !== undefined && typeof input._triggerButton === 'function') {
                input._triggerButton(button);
            }
        } else if (action === 'triggerItemButton' && input instanceof QuickPickInput) {
            const item = _quickInputItemForIndex(input, msg.itemIndex);
            const buttons = Array.isArray(item?.buttons) ? item.buttons : [];
            const buttonIndex = Number(msg.buttonIndex);
            const button = Number.isInteger(buttonIndex) ? buttons[buttonIndex] : undefined;
            if (item !== undefined && button !== undefined) {
                input._triggerItemButton(item, button);
            }
        } else if (action === 'accept' && typeof input._accept === 'function') {
            input._accept();
        } else if (action === 'hide') {
            input.hide();
        }
    } catch (err) {
        log(`quick_input_action ${action || '?'} failed: ${err?.message || err}`);
    }
}

function _messageArgIsItem(value) {
    return typeof value === 'string'
        || !!(value && typeof value === 'object'
            && Object.prototype.hasOwnProperty.call(value, 'title'));
}

function _windowShowMessage(level, message, args) {
    const rawArgs = Array.isArray(args) ? args : [];
    let options = {};
    let items = rawArgs;
    if (rawArgs.length && !_messageArgIsItem(rawArgs[0])) {
        options = rawArgs[0] || {};
        items = rawArgs.slice(1);
    }
    const normalizedItems = items
        .filter(_messageArgIsItem)
        .map((item, index) => typeof item === 'string'
            ? { title: item, handle: index, isCloseAffordance: false }
            : {
                title: String(item.title || ''),
                handle: index,
                isCloseAffordance: !!item.isCloseAffordance,
            });
    send({
        type: 'show_message',
        level,
        message: String(message),
        options: {
            modal: !!options.modal,
            detail: options.detail ? String(options.detail) : '',
        },
        items: normalizedItems,
    });
    return Promise.resolve(items.find(_messageArgIsItem));
}

function _windowSetStatusBarMessage(text, hideAfterTimeoutOrThenable) {
    const id = 'sbm-node-' + (++_sbiCounter);
    let closed = false;
    let timer = null;
    function clearTimer() {
        if (timer !== null) {
            clearTimeout(timer);
            timer = null;
        }
    }
    function close(messageType) {
        if (closed) return;
        closed = true;
        clearTimer();
        send({ type: messageType, id });
    }
    send({
        type: 'status_bar_show',
        id,
        text: String(text ?? ''),
        tooltip: '',
        command: '',
        alignment: 1,
        priority: Number.MAX_SAFE_INTEGER,
        color: '',
        backgroundColor: '',
    });
    if (typeof hideAfterTimeoutOrThenable === 'number') {
        const timeout = Math.max(0, Number(hideAfterTimeoutOrThenable));
        if (Number.isFinite(timeout)) {
            timer = setTimeout(() => close('status_bar_hide'), timeout);
        }
    } else if (
        hideAfterTimeoutOrThenable
        && typeof hideAfterTimeoutOrThenable.then === 'function'
    ) {
        Promise.resolve(hideAfterTimeoutOrThenable).then(
            () => close('status_bar_hide'),
            () => close('status_bar_hide'),
        );
    }
    return new Disposable(() => close('status_bar_dispose'));
}

function _authProviderMethod(provider, names) {
    for (const name of names) {
        if (provider && typeof provider[name] === 'function') {
            return provider[name].bind(provider);
        }
    }
    return null;
}

function _normalizeAuthenticationSession(raw, providerId, scopes = []) {
    if (!raw || typeof raw !== 'object') return null;
    const account = raw.account && typeof raw.account === 'object'
        ? raw.account
        : {};
    const accountLabel = account.label || raw.accountLabel || raw.accountName || 'Account';
    const accountId = account.id || raw.accountId || accountLabel;
    return {
        id: String(raw.id || `${providerId}:${accountId}`),
        accessToken: String(raw.accessToken || raw.access_token || raw.token || ''),
        account: {
            id: String(accountId),
            label: String(accountLabel),
        },
        scopes: Array.isArray(raw.scopes) ? raw.scopes.map(String) : scopes.map(String),
    };
}

async function _authProviderSessions(providerId, scopes = [], options = {}) {
    const entry = _authenticationProviders.get(String(providerId || ''));
    if (!entry) return [];
    const getter = _authProviderMethod(entry.provider, ['getSessions', 'get_sessions']);
    if (!getter) {
        return _authenticationSessions.get(providerId) || [];
    }
    const raw = await getter(scopes || [], options || {}) || [];
    const sessions = (Array.isArray(raw) ? raw : [])
        .map(item => _normalizeAuthenticationSession(item, providerId, scopes || []))
        .filter(Boolean);
    _authenticationSessions.set(providerId, sessions);
    return sessions;
}

async function _authGetSession(providerId, scopes = [], options = {}) {
    const normalizedId = String(providerId || '');
    const normalizedScopes = Array.isArray(scopes) ? scopes.map(String) : [];
    let sessions = await _authProviderSessions(
        normalizedId, normalizedScopes, options || {});
    if (sessions.length && !options?.forceNewSession) return sessions[0];
    const entry = _authenticationProviders.get(normalizedId);
    const creator = _authProviderMethod(entry?.provider, ['createSession', 'create_session']);
    if (creator && (options?.createIfNone || options?.forceNewSession)) {
        const created = _normalizeAuthenticationSession(
            await creator(normalizedScopes, options || {}),
            normalizedId,
            normalizedScopes,
        );
        if (created) {
            sessions = [...(_authenticationSessions.get(normalizedId) || []), created];
            _authenticationSessions.set(normalizedId, sessions);
            _onDidChangeAuthenticationSessionsEmitter.fire({
                provider: normalizedId,
                added: [created],
                removed: [],
                changed: [],
            });
            return created;
        }
    }
    return undefined;
}

function _authRegisterProvider(id, label, provider, options) {
    const providerId = String(id || '');
    if (!providerId) throw new Error('Authentication provider id is required');
    if (_authenticationProviders.has(providerId)) {
        throw new Error(`Authentication provider already registered: ${providerId}`);
    }
    let listener = null;
    const eventSource = provider && (
        provider.onDidChangeSessions || provider.on_did_change_sessions);
    if (typeof eventSource === 'function') {
        listener = eventSource.call(provider, (event = {}) => {
            _authProviderSessions(providerId, [], {})
                .catch(err => log(
                    `authentication session refresh failed for ${providerId}: ${err.message}`));
            _onDidChangeAuthenticationSessionsEmitter.fire({
                provider: providerId,
                added: event.added || [],
                removed: event.removed || [],
                changed: event.changed || [],
            });
        });
    }
    const entry = {
        label: String(label || providerId),
        provider,
        options: options || {},
        listener,
    };
    _authenticationProviders.set(providerId, entry);
    return new Disposable(() => {
        const current = _authenticationProviders.get(providerId);
        if (current === entry) {
            current.listener?.dispose?.();
            _authenticationProviders.delete(providerId);
            _authenticationSessions.delete(providerId);
        }
    });
}

function _extensionApiObject(id) {
    const active = _extensions.get(id);
    const known = _knownExtensions.get(id);
    if (!active && !known) return undefined;
    const extensionPath = active
        ? active.context.extensionPath
        : known.extensionPath;
    const extensionUri = Uri.file(extensionPath);
    const manifest = active
        ? (active.desc.manifest || {})
        : (known.manifest || {});
    const exportsValue = active ? active.activationExports : undefined;
    return {
        id,
        extensionUri,
        extensionPath,
        isActive: !!active,
        packageJSON: manifest,
        exports: exportsValue,
        extensionKind: active
            ? (active.context.extension?.extensionKind || 2)
            : _extensionKindValue(known.extensionKind),
        activate: () => _activateKnownExtension(id),
    };
}

function buildVscodeModule(extDesc, extensionPath, storageRoot) {
    const subscriptions = [];
    const storagePaths = _extensionStoragePaths(
        extDesc.extensionId || '',
        extensionPath,
        storageRoot,
    );
    try {
        fs.mkdirSync(storagePaths.root, { recursive: true });
    } catch {}
    const globalState = new Memento(storagePaths.globalState);
    const workspaceState = new Memento(storagePaths.workspaceState);
    const secretStorage = new SecretStorage(storagePaths.secrets);
    const extensionUri = Uri.file(extensionPath);
    const extensionKind = _extensionKindValue(extDesc.manifest?.extensionKind);

    const context = {
        subscriptions,
        extensionPath,
        extensionUri,
        asAbsolutePath(relativePath) {
            return path.resolve(extensionPath, relativePath || '');
        },
        globalState,
        workspaceState,
        secrets: secretStorage,
        storagePath: storagePaths.workspace,
        storageUri: Uri.file(storagePaths.workspace),
        globalStoragePath: storagePaths.global,
        globalStorageUri: Uri.file(storagePaths.global),
        logPath: storagePaths.logs,
        logUri: Uri.file(storagePaths.logs),
        extensionMode: 1, // Production
        extension: {
            id: extDesc.extensionId || '',
            extensionUri,
            extensionPath,
            isActive: true,
            packageJSON: extDesc.manifest || {},
            extensionKind,
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
        QuickPickItemKind: { Separator: 1 },
        InputBoxValidationSeverity: { Info: 1, Warning: 2, Error: 3 },
        QuickInputButtonLocation: { Title: 1, Inline: 2, Input: 3 },
        QuickInputButtons: {
            Back: Object.freeze({
                iconPath: Object.freeze({ id: 'arrow-left' }),
                tooltip: 'Back',
                location: 1,
            }),
        },
        TreeItemCollapsibleState: { None: 0, Collapsed: 1, Expanded: 2 },
        ExtensionKind: { UI: 1, Workspace: 2 },
        ExtensionMode: { Production: 1, Development: 2, Test: 3 },
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
        DocumentHighlightKind: { Text: 0, Read: 1, Write: 2 },
        ProgressLocation,
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
        LanguageModelChatMessageRole: { System: 0, User: 1, Assistant: 2 },
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
            registerWebviewPanelSerializer(viewType, serializer) {
                const normalized = String(viewType || '');
                if (!normalized) {
                    throw new Error('Webview panel serializer viewType is required');
                }
                if (!serializer || typeof serializer.deserializeWebviewPanel !== 'function') {
                    throw new Error('WebviewPanelSerializer must implement deserializeWebviewPanel');
                }
                if (_webviewPanelSerializers.has(normalized)) {
                    throw new Error(
                        `WebviewPanelSerializer already registered for viewType: ${normalized}`);
                }
                const entry = {
                    serializer,
                    extensionId: extDesc.extensionId || '',
                    extensionPath,
                };
                _webviewPanelSerializers.set(normalized, entry);
                send({
                    type: 'webview_panel_serializer_registered',
                    viewType: normalized,
                    extensionId: extDesc.extensionId || '',
                });
                log(`registered WebviewPanelSerializer: ${normalized}`);
                return new Disposable(() => {
                    if (_webviewPanelSerializers.get(normalized) !== entry) return;
                    _webviewPanelSerializers.delete(normalized);
                    send({
                        type: 'webview_panel_serializer_disposed',
                        viewType: normalized,
                        extensionId: extDesc.extensionId || '',
                    });
                });
            },
            registerWebviewViewProvider(viewType, provider, options) {
                _webviewViewProviders.set(viewType, {
                    provider,
                    options: options || {},
                    extensionPath,
                });
                log(`registered WebviewViewProvider: ${viewType}`);
                return new Disposable(() => _webviewViewProviders.delete(viewType));
            },
            registerCustomEditorProvider(viewType, provider, options) {
                const normalized = String(viewType || '');
                let changeSubscription = null;
                if (provider && typeof provider.onDidChangeCustomDocument === 'function') {
                    changeSubscription = provider.onDidChangeCustomDocument((event) => {
                        _handleCustomDocumentChange(normalized, event);
                    });
                }
                _customEditorProviders.set(normalized, {
                    provider,
                    options: options || {},
                    extensionId: extDesc.extensionId || '',
                    extensionPath,
                    changeSubscription,
                });
                send({
                    type: 'custom_editor_provider_registered',
                    viewType: normalized,
                    extensionId: extDesc.extensionId || '',
                    options: options || {},
                });
                log(`registered CustomEditorProvider: ${normalized}`);
                return new Disposable(() => {
                    changeSubscription?.dispose?.();
                    _customEditorProviders.delete(normalized);
                    for (const [key, entry] of [..._customEditorDocuments.entries()]) {
                        if (entry.viewType !== normalized) continue;
                        _customEditorDocuments.delete(key);
                        if (entry.viewId) _customEditorViewKeys.delete(entry.viewId);
                        try { entry.document?.dispose?.(); } catch {}
                    }
                    send({
                        type: 'custom_editor_provider_disposed',
                        viewType: normalized,
                        extensionId: extDesc.extensionId || '',
                    });
                });
            },
            createWebviewPanel(viewType, title, showOptions, options) {
                const viewId = `panel-${_nextViewHandle++}`;
                log(`stub: createWebviewPanel ${viewType} -> ${viewId}`);
                return _createWebviewPanelObject(
                    viewType, title, viewId, options || {}, extensionPath,
                    showOptions);
            },
            createOutputChannel(name, options) {
                const ch = new OutputChannel(typeof options === 'string' ? `${name} (${options})` : name);
                _outputChannels.set(ch.name, ch);
                return ch;
            },
            showInformationMessage(message, ...items) {
                return _windowShowMessage('info', message, items);
            },
            showWarningMessage(message, ...items) {
                return _windowShowMessage('warn', message, items);
            },
            showErrorMessage(message, ...items) {
                return _windowShowMessage('error', message, items);
            },
            showQuickPick(items, options, token) {
                return _windowShowQuickPick(items, options || {}, token);
            },
            showInputBox(options, token) {
                return _windowShowInputBox(options || {}, token);
            },
            showOpenDialog(options, token) {
                return _requestWindowDialog('open', options || {}, token);
            },
            showSaveDialog(options, token) {
                return _requestWindowDialog('save', options || {}, token);
            },
            showWorkspaceFolderPick(options, token) {
                return _windowShowWorkspaceFolderPick(options || {}, token);
            },
            createQuickPick() {
                return new QuickPickInput();
            },
            createInputBox() {
                return new InputBoxInput();
            },
            withProgress: _withProgress,
            withScmProgress(task) {
                return _withProgress(
                    { location: ProgressLocation.SourceControl },
                    (progress) => task({
                        report(value) {
                            progress.report({ increment: Number(value) || 0 });
                        },
                    }));
            },
            setStatusBarMessage(text, hideAfterTimeoutOrThenable) {
                return _windowSetStatusBarMessage(text, hideAfterTimeoutOrThenable);
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
            registerUriHandler(handler) {
                const id = _extensionIdKey(extDesc.extensionId);
                if (!id) throw new Error('Cannot register URI handler without extension id');
                if (_uriHandlers.has(id)) {
                    throw new Error(`Protocol handler already registered for extension ${id}`);
                }
                const entry = { handler };
                _uriHandlers.set(id, entry);
                const disposable = new Disposable(() => {
                    if (_uriHandlers.get(id) === entry) {
                        _uriHandlers.delete(id);
                    }
                });
                subscriptions.push(disposable);
                return disposable;
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
            getConfiguration(section, scope) {
                return _createConfigProxy(
                    section, _configOverrideIdentifierFromScope(scope));
            },
            get workspaceFolders() { return [_workspaceFolder()]; },
            get name() { return _workspaceName; },
            get rootPath() { return _workspaceRoot; },
            get textDocuments() {
                return Array.from(_workspaceTextDocuments.values()).filter(
                    _workspaceDocumentIsOpened);
            },
            onDidOpenTextDocument: _onDidOpenTextDocumentEmitter.event,
            onDidCloseTextDocument: _onDidCloseTextDocumentEmitter.event,
            onDidChangeTextDocument: _onDidChangeTextDocumentEmitter.event,
            onDidSaveTextDocument: _onDidSaveTextDocumentEmitter.event,
            fs: {
                readFile: (uri) => _diagnoseAsync(
                    'workspace.fs.readFile', _workspaceRelativePath(uri),
                    () => _workspaceFsReadFile(uri)),
                writeFile: (uri, content) => _diagnoseAsync(
                    'workspace.fs.writeFile', _workspaceRelativePath(uri),
                    () => _workspaceFsWriteFile(uri, content)),
                stat: (uri) => _diagnoseAsync(
                    'workspace.fs.stat', _workspaceRelativePath(uri),
                    () => _workspaceFsStat(uri)),
                readDirectory: (uri) => _diagnoseAsync(
                    'workspace.fs.readDirectory', _workspaceRelativePath(uri),
                    () => _workspaceFsReadDirectory(uri)),
                createDirectory: (uri) => _diagnoseAsync(
                    'workspace.fs.createDirectory', _workspaceRelativePath(uri),
                    () => _workspaceFsCreateDirectory(uri)),
                delete: (uri, options) => _diagnoseAsync(
                    'workspace.fs.delete', _workspaceRelativePath(uri),
                    () => _workspaceFsDelete(uri, options)),
                rename: (src, dst, options) => _diagnoseAsync(
                    'workspace.fs.rename',
                    `${_workspaceRelativePath(src)} -> ${_workspaceRelativePath(dst)}`,
                    () => _workspaceFsRename(src, dst, options)),
                copy: (src, dst, options) => _diagnoseAsync(
                    'workspace.fs.copy',
                    `${_workspaceRelativePath(src)} -> ${_workspaceRelativePath(dst)}`,
                    () => _workspaceFsCopy(src, dst, options)),
            },
            registerFileSystemProvider(scheme, provider, options) {
                const normalized = _normalizeFileSystemScheme(scheme);
                if (!normalized || normalized === 'file') {
                    throw new Error(`Invalid filesystem provider scheme: ${scheme}`);
                }
                const entry = {
                    provider,
                    options: options || {},
                    extensionId: extDesc.extensionId || '',
                };
                _fileSystemProviders.set(normalized, entry);
                return new Disposable(() => {
                    if (_fileSystemProviders.get(normalized) === entry) {
                        _fileSystemProviders.delete(normalized);
                    }
                });
            },
            onDidChangeConfiguration: _onDidChangeConfigurationEmitter.event,
            onDidChangeWorkspaceFolders: new EventEmitter().event,
            findFiles(include, exclude, maxResults) {
                return _diagnoseAsync(
                    'workspace.findFiles',
                    _workspacePatternText(include),
                    () => _workspaceFindFiles(include, exclude, maxResults),
                );
            },
            applyEdit(edit) {
                return _diagnoseAsync(
                    'workspace.applyEdit',
                    String(_workspaceEditEntries(edit).length),
                    () => _workspaceApplyEdit(edit),
                );
            },
            openTextDocument(uriOrPath) {
                return _diagnoseAsync(
                    'workspace.openTextDocument',
                    _workspaceOpenDetail(uriOrPath),
                    () => _workspaceOpenTextDocument(uriOrPath),
                );
            },
            asRelativePath(pathOrUri, includeWorkspaceFolder) {
                return _workspaceRelativePath(pathOrUri, includeWorkspaceFolder);
            },
            registerTextDocumentContentProvider(scheme, provider) {
                const normalized = _normalizeFileSystemScheme(scheme);
                if (!normalized || normalized === 'file' || normalized === 'untitled') {
                    throw new Error(`Invalid text document provider scheme: ${scheme}`);
                }
                if (_textDocumentContentProviders.has(normalized)) {
                    throw new Error(`Text document provider already registered: ${normalized}`);
                }
                const entry = {
                    provider,
                    extensionId: extDesc.extensionId || '',
                };
                _textDocumentContentProviders.set(normalized, entry);
                let changeSubscription;
                if (provider && typeof provider.onDidChange === 'function') {
                    changeSubscription = provider.onDidChange((uri) => {
                        _refreshTextDocumentContentProvider(normalized, uri)
                            .catch(err => log(
                                `content provider refresh failed for ${normalized}: ${err.message}`));
                    });
                }
                return new Disposable(() => {
                    if (_textDocumentContentProviders.get(normalized) === entry) {
                        _textDocumentContentProviders.delete(normalized);
                    }
                    changeSubscription?.dispose?.();
                });
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
                readText: () => _requestEnvClipboard('read'),
                writeText: value => _requestEnvClipboard('write', value),
            },
            async openExternal(uri) {
                const parsed = _workspaceUriFromInput(uri);
                if (await _dispatchUriHandler(parsed)) return true;
                await _activateKnownExtensionsForEvent(
                    `onOpenExternalUri:${parsed.scheme}`);
                log(`stub: openExternal ${parsed.toString()}`);
                return true;
            },
            asExternalUri(uri) {
                return Promise.resolve(_workspaceUriFromInput(uri));
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
                return _extensionApiObject(String(id || ''));
            },
            get all() {
                const ids = new Set([
                    ..._knownExtensions.keys(),
                    ..._extensions.keys(),
                ]);
                return [...ids].map(id => _extensionApiObject(id))
                    .filter(ext => !!ext);
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
            async selectChatModels(selector) {
                const models = await _requestPythonLm('selectChatModels', {
                    selector: _serializeLanguageValue(selector || {}),
                    extensionId: extDesc.extensionId || '',
                }, 5000);
                return (Array.isArray(models) ? models : [])
                    .map(_languageModelChatFromPayload)
                    .filter(model => !!model.id);
            },
            registerTool(name, tool) {
                const toolName = String(name || '');
                if (!toolName) throw new Error('Language model tool name is required');
                const entry = {
                    handle: _nextLmToolHandle++,
                    name: toolName,
                    tool,
                    extensionId: extDesc.extensionId || '',
                    metadata: {
                        description: _lmToolDescription(toolName, tool, null),
                        inputSchema: _lmToolInputSchema(tool, null),
                        tags: Array.isArray(tool?.tags) ? tool.tags : [],
                    },
                };
                _lmTools.set(toolName, entry);
                send({
                    type: 'lm_tool_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    name: entry.name,
                    description: entry.metadata.description,
                    inputSchema: entry.metadata.inputSchema,
                    tags: entry.metadata.tags,
                });
                _onDidChangeLmToolsEmitter.fire({ added: [toolName], removed: [] });
                const d = new Disposable(() => {
                    if (_lmTools.get(toolName) === entry) {
                        _lmTools.delete(toolName);
                        send({
                            type: 'lm_tool_disposed',
                            handle: entry.handle,
                            extensionId: entry.extensionId,
                            name: entry.name,
                        });
                        _onDidChangeLmToolsEmitter.fire({ added: [], removed: [toolName] });
                    }
                });
                subscriptions.push(d);
                return d;
            },
            registerToolDefinition(definition, tool) {
                const toolName = _lmToolDefinitionName(definition);
                if (!toolName) throw new Error('Language model tool definition name is required');
                const entry = {
                    handle: _nextLmToolHandle++,
                    name: toolName,
                    tool,
                    extensionId: extDesc.extensionId || '',
                    definition: definition && typeof definition === 'object'
                        ? _serializeLanguageValue(definition)
                        : { name: toolName },
                    metadata: {
                        description: _lmToolDescription(toolName, tool, definition),
                        inputSchema: _lmToolInputSchema(tool, definition),
                        tags: Array.isArray(definition?.tags) ? definition.tags : [],
                    },
                };
                _lmTools.set(toolName, entry);
                send({
                    type: 'lm_tool_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    name: entry.name,
                    definition: entry.definition,
                    description: entry.metadata.description,
                    inputSchema: entry.metadata.inputSchema,
                    tags: entry.metadata.tags,
                });
                _onDidChangeLmToolsEmitter.fire({ added: [toolName], removed: [] });
                const d = new Disposable(() => {
                    if (_lmTools.get(toolName) === entry) {
                        _lmTools.delete(toolName);
                        send({
                            type: 'lm_tool_disposed',
                            handle: entry.handle,
                            extensionId: entry.extensionId,
                            name: entry.name,
                        });
                        _onDidChangeLmToolsEmitter.fire({ added: [], removed: [toolName] });
                    }
                });
                subscriptions.push(d);
                return d;
            },
            async invokeTool(nameOrInfo, options, token) {
                const toolName = typeof nameOrInfo === 'string'
                    ? nameOrInfo
                    : String(nameOrInfo?.name || nameOrInfo?.id || '');
                const entry = _lmTools.get(toolName);
                if (!entry) throw new Error(`Language model tool not found: ${toolName}`);
                const result = await _invokeLmToolEntry(
                    entry,
                    options && Object.prototype.hasOwnProperty.call(options, 'input')
                        ? options.input
                        : options,
                    token || { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event },
                );
                return new LanguageModelToolResult(
                    (_serializeLanguageModelToolResult(result).content || [])
                        .map(part => new LanguageModelTextPart(part.text ?? part.value ?? '')),
                );
            },
            get tools() {
                return Array.from(_lmTools.values()).map(entry => ({
                    name: entry.name,
                    id: entry.name,
                    extensionId: entry.extensionId,
                    description: entry.metadata.description,
                    inputSchema: entry.metadata.inputSchema,
                    tags: entry.metadata.tags || [],
                }));
            },
            onDidChangeChatModels: new EventEmitter().event,
            onDidChangeTools: _onDidChangeLmToolsEmitter.event,
        },

        // --- Namespace: chat ---
        chat: {
            createChatParticipant(id, handler) {
                const participantId = String(id || '');
                if (!participantId) throw new Error('Chat participant id is required');
                const entry = {
                    handle: _nextChatParticipantHandle++,
                    id: participantId,
                    handler,
                    extensionId: extDesc.extensionId || '',
                };
                _chatParticipants.set(participantId, entry);
                send({
                    type: 'chat_participant_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    id: entry.id,
                });
                const participant = {
                    id,
                    iconPath: undefined,
                    onDidReceiveFeedback: new EventEmitter().event,
                    requestHandler: handler,
                    dispose() {
                        if (_chatParticipants.get(participantId) === entry) {
                            _chatParticipants.delete(participantId);
                            send({
                                type: 'chat_participant_disposed',
                                handle: entry.handle,
                                extensionId: entry.extensionId,
                                id: entry.id,
                            });
                        }
                    },
                };
                const d = new Disposable(() => participant.dispose());
                subscriptions.push(d);
                return participant;
            },
        },

        // --- Namespace: authentication ---
        authentication: {
            getSession(providerId, scopes, options) {
                return _authGetSession(providerId, scopes || [], options || {});
            },
            registerAuthenticationProvider(id, label, provider, options) {
                return _authRegisterProvider(id, label, provider, options || {});
            },
            onDidChangeSessions: _onDidChangeAuthenticationSessionsEmitter.event,
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
        LanguageModelTextPart,
        LanguageModelToolCallPart,
        LanguageModelDataPart,
        LanguageModelThinkingPart,
        LanguageModelPromptTsxPart,
        LanguageModelToolResult,
        LanguageModelToolResultPart,
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
        WorkspaceEdit: class {
            constructor() { this._edits = []; }
            replace(uri, range, text) { this._edits.push({ uri, range, text }); }
            insert(uri, pos, text) { this._edits.push({ uri, range: new Range(pos, pos), text }); }
            delete(uri, range) { this._edits.push({ uri, range, text: '' }); }
            set(uri, edits) { for (const e of edits) this._edits.push({ uri, ...e }); }
            createFile(uri, options, metadata) {
                this._edits.push({ kind: 'create', uri, options: options || {}, metadata });
            }
            deleteFile(uri, options, metadata) {
                this._edits.push({ kind: 'delete', uri, options: options || {}, metadata });
            }
            renameFile(oldUri, newUri, options, metadata) {
                this._edits.push({ kind: 'rename', oldUri, newUri, options: options || {}, metadata });
            }
        },
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

        LanguageModelChatMessage,
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
const _CONFIG_MISSING = Symbol('configMissing');
const _AI_EDITOR_SECTION_ALIASES = new Set([
    'claude_code',
    'codex',
    'mcp',
    'terminal',
    'extensions',
    'customization',
]);

function _configPath(value) {
    if (value === undefined || value === null) return [];
    return String(value).replace(/^\.+|\.+$/g, '').split('.').filter(Boolean);
}

function _configLookup(data, pathParts) {
    let current = data;
    for (const part of pathParts) {
        if (current && typeof current === 'object'
                && Object.prototype.hasOwnProperty.call(current, part)) {
            current = current[part];
        } else {
            return _CONFIG_MISSING;
        }
    }
    return current;
}

function _configSet(data, pathParts, value) {
    if (!pathParts.length) return;
    let current = data;
    for (const part of pathParts.slice(0, -1)) {
        let child = current[part];
        if (!child || typeof child !== 'object' || Array.isArray(child)) {
            child = {};
            current[part] = child;
        }
        current = child;
    }
    current[pathParts[pathParts.length - 1]] = value;
}

function _configCloneValue(value) {
    if (!value || typeof value !== 'object') return value;
    try {
        return JSON.parse(JSON.stringify(value));
    } catch {
        return Array.isArray(value) ? Array.from(value) : Object.assign({}, value);
    }
}

function _configSetDefault(pathParts, value) {
    if (!Array.isArray(pathParts) || !pathParts.length) return;
    _configSet(_configurationDefaults, pathParts, _configCloneValue(value));
    _configMirrorDefaultsAlias(pathParts, value);
}

function _configSetLanguageDefault(languageId, pathParts, value) {
    const id = String(languageId || '').trim();
    if (!id || !Array.isArray(pathParts) || !pathParts.length) return;
    if (!_configurationLanguageDefaults[id]) _configurationLanguageDefaults[id] = {};
    _configSet(
        _configurationLanguageDefaults[id],
        pathParts,
        _configCloneValue(value));
    _configMirrorLanguageDefaultsAlias(id, pathParts, value);
}

function _configLanguageOverrideKey(overrideIdentifier) {
    const id = String(overrideIdentifier || '').trim();
    return id ? `[${id}]` : '';
}

function _configLanguageOverrideStore(overrideIdentifier, create = false) {
    const key = _configLanguageOverrideKey(overrideIdentifier);
    if (!key) return null;
    let store = _settings[key];
    if (!store || typeof store !== 'object' || Array.isArray(store)) {
        if (!create) return null;
        store = {};
        _settings[key] = store;
    }
    return store;
}

function _configSetLanguageOverridePath(overrideIdentifier, pathParts, value) {
    const store = _configLanguageOverrideStore(overrideIdentifier, true);
    if (!store || !Array.isArray(pathParts) || !pathParts.length) return;
    store[pathParts.join('.')] = _configCloneValue(value);
}

function _configDeleteLanguageOverridePath(overrideIdentifier, pathParts) {
    const key = _configLanguageOverrideKey(overrideIdentifier);
    const store = _configLanguageOverrideStore(overrideIdentifier, false);
    if (!key || !store || !Array.isArray(pathParts) || !pathParts.length) return;
    delete store[pathParts.join('.')];
    _configDelete(store, pathParts);
    if (!Object.keys(store).length) delete _settings[key];
}

function _configSetLanguageOverride(overrideIdentifier, pathParts, value) {
    _configSetLanguageOverridePath(overrideIdentifier, pathParts, value);
    _configMirrorLanguageOverrideAlias(overrideIdentifier, pathParts, value, false);
}

function _configDeleteLanguageOverride(overrideIdentifier, pathParts) {
    _configDeleteLanguageOverridePath(overrideIdentifier, pathParts);
    _configMirrorLanguageOverrideAlias(overrideIdentifier, pathParts, undefined, true);
}

function _configMirrorDefaultsAlias(pathParts, value) {
    let mirrorPath = null;
    if (pathParts[0] === 'ai_editor'
            && _AI_EDITOR_SECTION_ALIASES.has(pathParts[1])) {
        mirrorPath = pathParts.slice(1);
    } else if (_AI_EDITOR_SECTION_ALIASES.has(pathParts[0])) {
        mirrorPath = ['ai_editor'].concat(pathParts);
    }
    if (!mirrorPath || mirrorPath.join('.') === pathParts.join('.')) return;
    _configSet(_configurationDefaults, mirrorPath, _configCloneValue(value));
}

function _configMirrorLanguageDefaultsAlias(languageId, pathParts, value) {
    let mirrorPath = null;
    if (pathParts[0] === 'ai_editor'
            && _AI_EDITOR_SECTION_ALIASES.has(pathParts[1])) {
        mirrorPath = pathParts.slice(1);
    } else if (_AI_EDITOR_SECTION_ALIASES.has(pathParts[0])) {
        mirrorPath = ['ai_editor'].concat(pathParts);
    }
    if (!mirrorPath || mirrorPath.join('.') === pathParts.join('.')) return;
    if (!_configurationLanguageDefaults[languageId]) {
        _configurationLanguageDefaults[languageId] = {};
    }
    _configSet(
        _configurationLanguageDefaults[languageId],
        mirrorPath,
        _configCloneValue(value));
}

function _configMirrorLanguageOverrideAlias(
        overrideIdentifier, pathParts, value, remove) {
    let mirrorPath = null;
    if (pathParts[0] === 'ai_editor'
            && _AI_EDITOR_SECTION_ALIASES.has(pathParts[1])) {
        mirrorPath = pathParts.slice(1);
    } else if (_AI_EDITOR_SECTION_ALIASES.has(pathParts[0])) {
        mirrorPath = ['ai_editor'].concat(pathParts);
    }
    if (!mirrorPath || mirrorPath.join('.') === pathParts.join('.')) return;
    if (remove) {
        _configDeleteLanguageOverridePath(overrideIdentifier, mirrorPath);
    } else {
        _configSetLanguageOverridePath(overrideIdentifier, mirrorPath, value);
    }
}

function _configMergeObjects(defaultValue, configuredValue) {
    const base = defaultValue && typeof defaultValue === 'object'
        && !Array.isArray(defaultValue) ? _configCloneValue(defaultValue) : {};
    if (configuredValue && typeof configuredValue === 'object'
            && !Array.isArray(configuredValue)) {
        for (const [key, value] of Object.entries(configuredValue)) {
            if (value && typeof value === 'object' && !Array.isArray(value)
                    && base[key] && typeof base[key] === 'object'
                    && !Array.isArray(base[key])) {
                base[key] = _configMergeObjects(base[key], value);
            } else {
                base[key] = _configCloneValue(value);
            }
        }
        return base;
    }
    return configuredValue === _CONFIG_MISSING ? base : _configCloneValue(configuredValue);
}

function _configMergeLayer(base, value) {
    if (value === _CONFIG_MISSING) return base;
    if (base === _CONFIG_MISSING) return _configCloneValue(value);
    if (base && typeof base === 'object' && !Array.isArray(base)
            && value && typeof value === 'object' && !Array.isArray(value)) {
        return _configMergeObjects(base, value);
    }
    return _configCloneValue(value);
}

function _configLanguageOverrideLookup(pathParts, overrideIdentifier) {
    const store = _configLanguageOverrideStore(overrideIdentifier, false);
    if (!store || !Array.isArray(pathParts) || !pathParts.length) {
        return _CONFIG_MISSING;
    }
    const dotted = pathParts.join('.');
    if (Object.prototype.hasOwnProperty.call(store, dotted)) {
        return store[dotted];
    }
    return _configLookup(store, pathParts);
}

function _configLanguageOverrideSection(sectionPath, overrideIdentifier) {
    const store = _configLanguageOverrideStore(overrideIdentifier, false);
    if (!store || !Array.isArray(sectionPath)) return _CONFIG_MISSING;
    if (!sectionPath.length) return _configCloneValue(store);
    const prefix = sectionPath.join('.');
    let result = _CONFIG_MISSING;
    const nested = _configLookup(store, sectionPath);
    if (nested !== _CONFIG_MISSING) {
        result = _configMergeLayer(result, nested);
    }
    for (const [key, value] of Object.entries(store)) {
        if (key === prefix) {
            result = _configMergeLayer(result, value);
            continue;
        }
        if (!key.startsWith(prefix + '.')) continue;
        const suffix = _configPath(key.slice(prefix.length + 1));
        if (!suffix.length) continue;
        if (result === _CONFIG_MISSING
                || !result || typeof result !== 'object'
                || Array.isArray(result)) {
            result = {};
        }
        _configSet(result, suffix, _configCloneValue(value));
    }
    return result;
}

function _configLanguageDefaultLookup(pathParts, overrideIdentifier) {
    const id = String(overrideIdentifier || '').trim();
    if (!id || !_configurationLanguageDefaults[id]) return _CONFIG_MISSING;
    return _configLookup(_configurationLanguageDefaults[id], pathParts);
}

function _configDefaultLookup(pathParts, overrideIdentifier) {
    const languageDefault = _configLanguageDefaultLookup(
        pathParts, overrideIdentifier);
    if (languageDefault !== _CONFIG_MISSING) return languageDefault;
    return _configLookup(_configurationDefaults, pathParts);
}

function _configEffectiveLookup(pathParts, overrideIdentifier) {
    const languageConfigured = _configLanguageOverrideLookup(
        pathParts, overrideIdentifier);
    if (languageConfigured !== _CONFIG_MISSING) return languageConfigured;
    const configured = _configLookup(_settings, pathParts);
    if (configured !== _CONFIG_MISSING) return configured;
    return _configDefaultLookup(pathParts, overrideIdentifier);
}

function _configEffectiveSection(sectionPath, overrideIdentifier) {
    const defaults = _configLookup(_configurationDefaults, sectionPath);
    const languageDefaults = _configLanguageDefaultLookup(
        sectionPath, overrideIdentifier);
    const configured = _configLookup(_settings, sectionPath);
    const languageConfigured = _configLanguageOverrideSection(
        sectionPath, overrideIdentifier);
    let result = _CONFIG_MISSING;
    result = _configMergeLayer(result, defaults);
    result = _configMergeLayer(result, languageDefaults);
    result = _configMergeLayer(result, configured);
    result = _configMergeLayer(result, languageConfigured);
    return result === _CONFIG_MISSING ? {} : _configCloneValue(result);
}

function _configDelete(data, pathParts) {
    if (!pathParts.length) return;
    let current = data;
    for (const part of pathParts.slice(0, -1)) {
        const child = current && current[part];
        if (!child || typeof child !== 'object') return;
        current = child;
    }
    delete current[pathParts[pathParts.length - 1]];
}

function _configMirrorAiEditorAlias(pathParts, value, remove) {
    let mirrorPath = null;
    if (pathParts[0] === 'ai_editor'
            && _AI_EDITOR_SECTION_ALIASES.has(pathParts[1])) {
        mirrorPath = pathParts.slice(1);
    } else if (_AI_EDITOR_SECTION_ALIASES.has(pathParts[0])) {
        mirrorPath = ['ai_editor'].concat(pathParts);
    }
    if (!mirrorPath || mirrorPath.join('.') === pathParts.join('.')) return;
    if (remove) {
        _configDelete(_settings, mirrorPath);
    } else {
        _configSet(_settings, mirrorPath, value);
    }
}

function _registerConfigurationDefaultsFromManifest(manifest) {
    const contributes = manifest && typeof manifest === 'object'
        ? manifest.contributes || {}
        : {};
    const rawConfigurations = contributes.configuration;
    const configurations = Array.isArray(rawConfigurations)
        ? rawConfigurations
        : (rawConfigurations ? [rawConfigurations] : []);
    for (const entry of configurations) {
        const properties = entry && typeof entry === 'object'
            ? entry.properties || {}
            : {};
        for (const [key, schema] of Object.entries(properties)) {
            if (!key || key.startsWith('[')) continue;
            if (schema && typeof schema === 'object'
                    && Object.prototype.hasOwnProperty.call(schema, 'default')) {
                _configSetDefault(_configPath(key), schema.default);
            }
        }
    }
    const defaults = contributes.configurationDefaults;
    if (defaults && typeof defaults === 'object' && !Array.isArray(defaults)) {
        for (const [key, value] of Object.entries(defaults)) {
            if (!key) continue;
            if (key.startsWith('[')) {
                for (const languageId of _configOverrideIdentifiersFromKey(key)) {
                    if (!value || typeof value !== 'object'
                            || Array.isArray(value)) continue;
                    for (const [settingKey, settingValue] of Object.entries(value)) {
                        _configSetLanguageDefault(
                            languageId, _configPath(settingKey), settingValue);
                    }
                }
            } else {
                _configSetDefault(_configPath(key), value);
            }
        }
    }
}

function _configOverrideIdentifiersFromKey(key) {
    const result = [];
    const text = String(key || '');
    const matcher = /\[([^\]]+)\]/g;
    let match;
    while ((match = matcher.exec(text)) !== null) {
        const id = String(match[1] || '').trim();
        if (id) result.push(id);
    }
    return result;
}

function _configOverrideIdentifierFromScope(scope) {
    if (scope && typeof scope === 'object'
            && typeof scope.languageId === 'string') {
        return scope.languageId;
    }
    return '';
}

function _configFullPath(section, key) {
    return _configPath(section).concat(_configPath(key));
}

function _fireConfigurationChanged(pathParts) {
    const fullKey = pathParts.join('.');
    _onDidChangeConfigurationEmitter.fire({
        affectsConfiguration(sect) {
            const probe = _configPath(sect).join('.');
            if (!probe) return true;
            return fullKey === probe
                || fullKey.startsWith(probe + '.')
                || probe.startsWith(fullKey + '.');
        },
    });
}

function _createConfigProxy(section, overrideIdentifier = '') {
    const sectionPath = _configPath(section);
    const sectionData = () => _configEffectiveSection(
        sectionPath, overrideIdentifier);
    return {
        get(key, defaultValue) {
            if (arguments.length === 0 || key === undefined) {
                const data = sectionData();
                return data && typeof data === 'object'
                    ? Object.assign({}, data)
                    : data;
            }
            const value = _configEffectiveLookup(
                _configFullPath(section, key), overrideIdentifier);
            return value === _CONFIG_MISSING ? defaultValue : value;
        },
        has(key) {
            return _configEffectiveLookup(
                _configFullPath(section, key), overrideIdentifier)
                !== _CONFIG_MISSING;
        },
        inspect(key) {
            const pathParts = _configFullPath(section, key);
            const value = _configLookup(_settings, pathParts);
            const languageValue = _configLanguageOverrideLookup(
                pathParts, overrideIdentifier);
            const defaultValue = _configLookup(_configurationDefaults, pathParts);
            const defaultLanguageValue = _configLanguageDefaultLookup(
                pathParts, overrideIdentifier);
            if (value === _CONFIG_MISSING
                    && languageValue === _CONFIG_MISSING
                    && defaultValue === _CONFIG_MISSING
                    && defaultLanguageValue === _CONFIG_MISSING) {
                return undefined;
            }
            return {
                key: pathParts.join('.'),
                defaultValue: defaultValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(defaultValue),
                defaultLanguageValue: defaultLanguageValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(defaultLanguageValue),
                globalValue: value === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(value),
                workspaceValue: value === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(value),
                globalLanguageValue: languageValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(languageValue),
                workspaceLanguageValue: languageValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(languageValue),
                workspaceFolderValue: undefined,
            };
        },
        update(key, value, configTarget, overrideInLanguage) {
            const pathParts = _configFullPath(section, key);
            const languageOverrideIdentifier = overrideInLanguage
                ? overrideIdentifier
                : '';
            if (languageOverrideIdentifier) {
                if (value === undefined) {
                    _configDeleteLanguageOverride(
                        languageOverrideIdentifier, pathParts);
                } else {
                    _configSetLanguageOverride(
                        languageOverrideIdentifier, pathParts, value);
                }
            } else {
                if (value === undefined) {
                    _configDelete(_settings, pathParts);
                    _configMirrorAiEditorAlias(pathParts, value, true);
                } else {
                    _configSet(_settings, pathParts, value);
                    _configMirrorAiEditorAlias(pathParts, value, false);
                }
            }
            const message = {
                type: 'config_set',
                section: section || '',
                key: String(key),
            };
            if (languageOverrideIdentifier) {
                message.overrideIdentifier = languageOverrideIdentifier;
            }
            if (value === undefined) {
                message.remove = true;
            } else {
                message.value = value;
            }
            send(message);
            _fireConfigurationChanged(pathParts);
            return Promise.resolve();
        },
    };
}

// -------------------------------------------------------------------------
// Extension loader
// -------------------------------------------------------------------------
async function activateExtension(msg) {
    const { extensionPath, extensionId, manifest, storageRoot } = msg;
    _registerKnownExtensions([{
        extensionId,
        extensionPath,
        manifest,
        storageRoot,
        extensionKind: manifest?.extensionKind || 'workspace',
    }]);
    if (_extensions.has(extensionId)) {
        _completeExtensionActivation(extensionId, true, '');
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
        storageRoot,
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
        let activationExports = undefined;

        if (typeof extModule.activate === 'function') {
            const result = extModule.activate(context);
            activationExports = result && typeof result.then === 'function'
                ? await result
                : result;
        }
        context.extension.exports = activationExports;

        _extensions.set(extensionId, {
            desc: { extensionId, manifest },
            module: extModule,
            context,
            deactivate: deactivateFn,
            vscode,
            activationExports,
        });

        // Resolve any pending webview view providers that were registered
        // during activate(). For each, resolve immediately so Python knows.
        for (const [viewType] of _webviewViewProviders) {
            resolveWebviewView(viewType);
        }

        _completeExtensionActivation(extensionId, true, '');
        send({ type: 'activated', extensionId, ok: true });
    } catch (err) {
        log(`activation failed for ${extensionId}: ${err.stack || err.message}`);
        _completeExtensionActivation(
            extensionId, false, err?.message || String(err));
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
    const view = new WebviewView(
        viewId, viewType, {},
        _defaultLocalResourceRoots(reg.extensionPath));
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

async function resolveCustomEditor(msg) {
    const viewType = String(msg.viewType || msg.customEditorId || '');
    const reg = _customEditorProviders.get(viewType);
    if (!reg) {
        const error = `no custom editor provider for viewType=${viewType}`;
        log(error);
        send({ type: 'custom_editor_resolved', requestId: msg.requestId, viewType, ok: false, error });
        return;
    }
    const uri = _workspaceUriFromInput(msg.uri || msg.resource || msg.path);
    const viewId = String(
        msg.viewId || `custom-${_safeViewIdPart(viewType)}-${_nextViewHandle++}`);
    const title = String(msg.title || path.basename(uri.fsPath || uri.path || viewType));
    const panel = _createWebviewPanelObject(
        viewType, title, viewId,
        (reg.options && reg.options.webviewOptions) || {},
        reg.extensionPath);
    const token = {
        isCancellationRequested: false,
        onCancellationRequested: new EventEmitter().event,
    };
    try {
        let document = null;
        let customEntry = null;
        if (typeof reg.provider.resolveCustomTextEditor === 'function') {
            document = await _workspaceOpenTextDocument(uri);
            await reg.provider.resolveCustomTextEditor(document, panel, token);
            const key = _customEditorDocumentKey(viewType, document.uri, viewId);
            customEntry = {
                viewType,
                uri: _workspaceUriFromInput(document.uri),
                viewId,
                provider: reg.provider,
                document,
                dirty: !!document.isDirty,
                contentDirty: !!document.isDirty,
                editable: true,
                textEditor: true,
                lastKind: 'resolved',
                lastLabel: '',
                lastEditId: 0,
                edits: [],
                currentEditIndex: -1,
                backup: null,
                backupId: '',
            };
            _customEditorDocuments.set(key, customEntry);
            _customEditorViewKeys.set(viewId, key);
            panel.onDidDispose(() => {
                _customEditorDocuments.delete(key);
                _customEditorViewKeys.delete(viewId);
            });
        } else {
            if (typeof reg.provider.openCustomDocument === 'function') {
                document = await reg.provider.openCustomDocument(
                    uri, { backupId: msg.backupId || undefined }, token);
            } else {
                document = { uri, dispose() {} };
            }
            if (typeof reg.provider.resolveCustomEditor !== 'function') {
                throw new Error(`custom editor provider ${viewType} has no resolver`);
            }
            await reg.provider.resolveCustomEditor(document, panel, token);
            if (!document.uri) document.uri = uri;
            const key = _customEditorDocumentKey(viewType, document.uri, viewId);
            customEntry = {
                viewType,
                uri: _workspaceUriFromInput(document.uri),
                viewId,
                provider: reg.provider,
                document,
                dirty: false,
                contentDirty: false,
                editable: typeof reg.provider.onDidChangeCustomDocument === 'function',
                lastKind: 'resolved',
                lastLabel: '',
                lastEditId: 0,
                edits: [],
                currentEditIndex: -1,
                backup: null,
                backupId: msg.backupId ? String(msg.backupId) : '',
            };
            _customEditorDocuments.set(key, customEntry);
            _customEditorViewKeys.set(viewId, key);
            panel.onDidDispose(() => {
                _customEditorDocuments.delete(key);
                _customEditorViewKeys.delete(viewId);
                try { document?.dispose?.(); } catch {}
            });
        }
        send({
            type: 'custom_editor_resolved',
            requestId: msg.requestId,
            viewType,
            viewId,
            uri: uri.toString(),
            ok: true,
            ...(customEntry ? _customEditorStatePayload(customEntry, 'resolved') : {
                editable: false,
                supportsSave: false,
                supportsSaveAs: false,
                supportsRevert: false,
                supportsBackup: false,
                dirty: false,
            }),
        });
    } catch (err) {
        const error = err && err.message ? err.message : String(err);
        _webviewViews.delete(viewId);
        log(`resolveCustomEditor error for ${viewType}: ${error}`);
        send({ type: 'custom_editor_resolved', requestId: msg.requestId, viewType, viewId, uri: uri.toString(), ok: false, error });
        send({ type: 'error', extensionId: viewType, error });
    }
}

async function deserializeWebviewPanel(msg) {
    const requestId = String(msg.requestId || '');
    const viewType = String(msg.viewType || '');
    const reg = _webviewPanelSerializers.get(viewType);
    const state = msg.state === undefined ? null : msg.state;
    let viewId = String(msg.viewId || '');
    let panel = null;
    if (!reg) {
        const error = `No webview panel serializer registered for viewType=${viewType}`;
        log(error);
        send({
            type: 'webview_panel_deserialized',
            requestId,
            viewType,
            viewId,
            ok: false,
            error,
        });
        return;
    }
    try {
        viewId = viewId || `serialized-${_safeViewIdPart(viewType)}-${_nextViewHandle++}`;
        panel = _createWebviewPanelObject(
            viewType,
            String(msg.title || viewType),
            viewId,
            (msg.webviewOptions || msg.options || {}),
            reg.extensionPath,
            msg.showOptions || msg.viewColumn || undefined);
        const result = reg.serializer.deserializeWebviewPanel(panel, state);
        if (result && typeof result.then === 'function') await result;
        send({
            type: 'webview_panel_deserialized',
            requestId,
            viewType,
            viewId,
            ok: true,
            state,
        });
    } catch (err) {
        const error = err && err.message ? err.message : String(err);
        try { panel?.dispose?.(); } catch {}
        _webviewViews.delete(viewId);
        log(`deserializeWebviewPanel error for ${viewType}: ${error}`);
        send({
            type: 'webview_panel_deserialized',
            requestId,
            viewType,
            viewId,
            ok: false,
            error,
        });
        send({ type: 'error', extensionId: viewType, error });
    }
}

async function handleCustomEditorLifecycle(msg) {
    const requestId = msg.requestId || '';
    const action = String(msg.action || '').trim();
    try {
        const entry = _customEditorEntryForMessage(msg);
        if (!entry) throw new Error('Custom editor document is not resolved');
        const provider = entry.provider || {};
        const token = {
            isCancellationRequested: false,
            onCancellationRequested: new EventEmitter().event,
        };
        let value = null;
        if (entry.textEditor && action === 'save') {
            if (typeof entry.document.save === 'function') {
                await entry.document.save();
            } else {
                _markCustomTextEditorsSaved(entry.document, 'save');
            }
            entry.lastKind = 'save';
            value = _customEditorStatePayload(entry, 'save', { dirty: false });
        } else if (entry.textEditor && action === 'saveAs') {
            const targetInput = msg.target || msg.targetUri || msg.target_uri || '';
            if (!targetInput) throw new Error('Custom editor saveAs target is required');
            const oldKey = entry.viewId ? _customEditorViewKeys.get(entry.viewId) : '';
            const target = _workspaceUriFromInput(targetInput);
            if (target.scheme !== 'file') {
                throw new Error('Custom text editor saveAs target must be a file URI');
            }
            await fsp.mkdir(path.dirname(target.fsPath), { recursive: true });
            await fsp.writeFile(target.fsPath, entry.document.getText(), 'utf8');
            if (oldKey) _customEditorDocuments.delete(oldKey);
            _workspaceRetargetTextDocument(entry.document, target);
            entry.uri = target;
            const newKey = _customEditorDocumentKey(entry.viewType, target, entry.viewId);
            _customEditorDocuments.set(newKey, entry);
            if (entry.viewId) _customEditorViewKeys.set(entry.viewId, newKey);
            _markCustomEditorClean(entry);
            entry.lastKind = 'saveAs';
            _sendCustomEditorState(entry, 'saveAs', { dirty: false, target: target.toString() });
            value = _customEditorStatePayload(entry, 'saveAs', {
                dirty: false,
                target: target.toString(),
            });
        } else if (entry.textEditor && action === 'revert') {
            if (entry.uri.scheme === 'file') {
                const text = await fsp.readFile(entry.uri.fsPath, 'utf8');
                if (typeof entry.document._setText === 'function') {
                    entry.document._setText(text, Date.now());
                }
            }
            _markCustomTextEditorsSaved(entry.document, 'revert');
            entry.lastKind = 'revert';
            value = _customEditorStatePayload(entry, 'revert', { dirty: false });
        } else if (entry.textEditor && action === 'state') {
            value = _customEditorStatePayload(entry, 'state');
        } else if (entry.textEditor && action === 'backup') {
            entry.lastKind = 'backup';
            value = _customEditorStatePayload(entry, 'backup');
        } else if (action === 'save') {
            if (typeof provider.saveCustomDocument !== 'function') {
                throw new Error('Custom editor provider does not implement saveCustomDocument');
            }
            await provider.saveCustomDocument(entry.document, token);
            await _disposeCustomEditorBackup(entry);
            _markCustomEditorClean(entry);
            entry.lastKind = 'save';
            _sendCustomEditorState(entry, 'save', { dirty: false });
            value = _customEditorStatePayload(entry, 'save');
        } else if (action === 'saveAs') {
            if (typeof provider.saveCustomDocumentAs !== 'function') {
                throw new Error('Custom editor provider does not implement saveCustomDocumentAs');
            }
            const targetInput = msg.target || msg.targetUri || msg.target_uri || '';
            if (!targetInput) throw new Error('Custom editor saveAs target is required');
            const target = _workspaceUriFromInput(targetInput);
            await provider.saveCustomDocumentAs(entry.document, target, token);
            await _disposeCustomEditorBackup(entry);
            _markCustomEditorClean(entry);
            entry.lastKind = 'saveAs';
            _sendCustomEditorState(entry, 'saveAs', { dirty: false, target: target.toString() });
            value = _customEditorStatePayload(entry, 'saveAs', { target: target.toString() });
        } else if (action === 'revert') {
            if (typeof provider.revertCustomDocument !== 'function') {
                throw new Error('Custom editor provider does not implement revertCustomDocument');
            }
            await provider.revertCustomDocument(entry.document, token);
            await _disposeCustomEditorBackup(entry);
            _markCustomEditorClean(entry);
            entry.lastKind = 'revert';
            _sendCustomEditorState(entry, 'revert', { dirty: false });
            value = _customEditorStatePayload(entry, 'revert');
        } else if (action === 'backup') {
            if (typeof provider.backupCustomDocument !== 'function') {
                throw new Error('Custom editor provider does not implement backupCustomDocument');
            }
            const destination = await _customEditorBackupDestination(entry);
            const backup = await provider.backupCustomDocument(
                entry.document, { destination }, token);
            entry.backup = backup || null;
            entry.backupId = backup && backup.id ? String(backup.id) : '';
            entry.lastKind = 'backup';
            _sendCustomEditorState(entry, 'backup', { backupId: entry.backupId });
            value = _customEditorStatePayload(entry, 'backup');
        } else if (action === 'undo') {
            let edit = null;
            if (msg.editId !== undefined && msg.editId !== null && msg.editId !== '') {
                const wantedId = Number(msg.editId);
                const wantedIndex = entry.edits.findIndex(item => item.id === wantedId);
                if (wantedIndex >= 0 && wantedIndex <= entry.currentEditIndex) {
                    entry.currentEditIndex = wantedIndex;
                }
            }
            if (entry.currentEditIndex >= 0) {
                edit = entry.edits[entry.currentEditIndex];
                await Promise.resolve(edit.undo());
                entry.currentEditIndex -= 1;
                entry.lastEditId = edit.id;
                entry.lastLabel = edit.label || '';
                entry.lastKind = 'undo';
                _refreshCustomEditorDirty(entry);
                if (!entry.dirty) await _disposeCustomEditorBackup(entry);
            } else {
                entry.lastKind = 'undo';
                _refreshCustomEditorDirty(entry);
            }
            _sendCustomEditorState(entry, 'undo', { editId: edit ? edit.id : 0 });
            value = _customEditorStatePayload(entry, 'undo', { editId: edit ? edit.id : 0 });
        } else if (action === 'redo') {
            let edit = null;
            const nextIndex = entry.currentEditIndex + 1;
            if (nextIndex < entry.edits.length) {
                edit = entry.edits[nextIndex];
                await Promise.resolve(edit.redo());
                entry.currentEditIndex = nextIndex;
                entry.lastEditId = edit.id;
                entry.lastLabel = edit.label || '';
                entry.lastKind = 'redo';
                _refreshCustomEditorDirty(entry);
            } else {
                entry.lastKind = 'redo';
                _refreshCustomEditorDirty(entry);
            }
            _sendCustomEditorState(entry, 'redo', { editId: edit ? edit.id : 0 });
            value = _customEditorStatePayload(entry, 'redo', { editId: edit ? edit.id : 0 });
        } else if (action === 'state') {
            value = _customEditorStatePayload(entry, 'state');
        } else {
            throw new Error(`Unsupported custom editor lifecycle action: ${action}`);
        }
        send({
            type: 'custom_editor_lifecycle_response',
            requestId,
            ok: true,
            action,
            state: value,
            ...value,
        });
    } catch (err) {
        const error = err && err.message ? err.message : String(err);
        log(`custom editor lifecycle ${action || '?'} failed: ${error}`);
        send({
            type: 'custom_editor_lifecycle_response',
            requestId,
            ok: false,
            action,
            error,
        });
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

async function handleLmToolRequest(msg) {
    const requestId = String(msg.requestId || '');
    const name = String(msg.name || msg.toolName || '');
    const entry = _lmTools.get(name);
    if (!entry) {
        send({
            type: 'lm_tool_response',
            requestId,
            ok: false,
            error: `Language model tool not found: ${name}`,
        });
        return;
    }
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const result = await _invokeLmToolEntry(entry, msg.input, token);
        send({
            type: 'lm_tool_response',
            requestId,
            ok: true,
            value: _serializeLanguageModelToolResult(result),
        });
    } catch (err) {
        send({
            type: 'lm_tool_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function handleChatParticipantRequest(msg) {
    const requestId = String(msg.requestId || '');
    const participantId = String(msg.participantId || msg.id || '');
    const entry = _chatParticipants.get(participantId);
    if (!entry || typeof entry.handler !== 'function') {
        send({
            type: 'chat_participant_response',
            requestId,
            ok: false,
            error: `Chat participant not found: ${participantId}`,
        });
        return;
    }
    const parts = [];
    const request = _chatRequestFromPayload(msg);
    const context = Object.assign({
        history: [],
        participant: participantId,
    }, msg.context && typeof msg.context === 'object'
        ? _deserializeArgFromPython(msg.context)
        : {});
    const stream = _createChatResponseStream(parts);
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const raw = entry.handler(request, context, stream, token);
        const result = raw && typeof raw.then === 'function' ? await raw : raw;
        send({
            type: 'chat_participant_response',
            requestId,
            ok: true,
            value: {
                content: parts.join(''),
                result: result === undefined ? null : _serializeLanguageValue(result),
            },
        });
    } catch (err) {
        send({
            type: 'chat_participant_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
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
            const rawResolveCount = Number(msg.itemResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            for (const entry of providers) {
                const triggers = (entry.triggers || []).map(item => String(item));
                if (trigger && !triggers.includes(trigger)) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token, context);
                    const normalized = _completionListFromProviderValue(value);
                    for (let item of normalized.items) {
                        if (remainingResolves > 0 && typeof provider.resolveCompletionItem === 'function') {
                            const resolved = await provider.resolveCompletionItem.call(provider, item, token);
                            if (resolved !== undefined && resolved !== null) item = resolved;
                            remainingResolves -= 1;
                        }
                        items.push(item);
                    }
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

        if (kind === 'inlayHint') {
            const values = [];
            const rawResolveCount = Number(msg.hintResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawHints = _normalizeProviderItems(await fn.call(provider, document, range, token));
                    for (let hint of rawHints) {
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveInlayHint === 'function') {
                                const resolved = await provider.resolveInlayHint.call(provider, hint, token);
                                if (resolved !== undefined && resolved !== null) hint = resolved;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(hint);
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

        if (kind === 'codeActions') {
            const values = [];
            const rawResolveCount = Number(msg.itemResolveCount || msg.resolveCount || 0);
            let remainingResolves = Number.isFinite(rawResolveCount) ? Math.max(0, rawResolveCount) : 0;
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawActions = _normalizeProviderItems(await fn.call(provider, document, range, {
                        diagnostics: msg.diagnostics || [],
                        only: msg.only,
                        triggerKind: msg.triggerKind,
                    }, token));
                    for (let action of rawActions) {
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveCodeAction === 'function') {
                                const resolved = await provider.resolveCodeAction.call(provider, action, token);
                                if (resolved !== undefined && resolved !== null) action = resolved;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(action);
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
        case 'register_extensions':
            _registerKnownExtensions(msg.extensions);
            break;
        case 'activate_extension_response':
            _handleActivateExtensionResponse(msg);
            break;
        case 'activate':
            await activateExtension(msg);
            break;
        case 'deactivate':
            await deactivateExtension(msg.extensionId);
            break;
        case 'webviewMessage':
        case 'webview_message':
            handleWebviewMessage(msg.viewId, msg.message);
            break;
        case 'resolve_webview_view':
            resolveWebviewView(msg.viewType, msg.state);
            break;
        case 'resolve_custom_editor':
            await resolveCustomEditor(msg);
            break;
        case 'deserialize_webview_panel':
            await deserializeWebviewPanel(msg);
            break;
        case 'custom_editor_lifecycle':
            await handleCustomEditorLifecycle(msg);
            break;
        case 'command':
        case 'executeCommand':
            await executeCommand(msg.commandId, msg.args, msg.requestId);
            break;
        case 'execute_command_response':
            handleExecuteCommandResponse(msg);
            break;
        case 'window_dialog_response':
            _handleWindowDialogResponse(msg);
            break;
        case 'env_clipboard_response':
            _handleEnvClipboardResponse(msg);
            break;
        case 'lm_model_response':
            _handlePythonLmResponse(msg);
            break;
        case 'tree_request':
            await handleTreeRequest(msg);
            break;
        case 'language_provider_request':
            await handleLanguageProviderRequest(msg);
            break;
        case 'lm_tool_request':
            await handleLmToolRequest(msg);
            break;
        case 'chat_participant_request':
            await handleChatParticipantRequest(msg);
            break;
        case 'tree_view_event':
            handleTreeViewEvent(msg);
            break;
        case 'quick_input_action':
            handleQuickInputAction(msg);
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
                const changedPath = _configFullPath(changedSection, msg.key);
                const changedOverrideIdentifier =
                    _configOverrideIdentifierFromScope(msg)
                        || String(msg.overrideIdentifier || '');
                if (changedOverrideIdentifier) {
                    if (msg.remove) {
                        _configDeleteLanguageOverride(
                            changedOverrideIdentifier, changedPath);
                    } else {
                        _configSetLanguageOverride(
                            changedOverrideIdentifier, changedPath, msg.value);
                    }
                } else if (msg.remove) {
                    _configDelete(_settings, changedPath);
                    _configMirrorAiEditorAlias(changedPath, undefined, true);
                } else if (msg.key !== undefined) {
                    _configSet(_settings, changedPath, msg.value);
                    _configMirrorAiEditorAlias(changedPath, msg.value, false);
                } else if (msg.value !== undefined && typeof msg.value === 'object') {
                    const sectionPath = _configPath(changedSection);
                    _configSet(_settings, sectionPath, msg.value);
                    _configMirrorAiEditorAlias(sectionPath, msg.value, false);
                }
                // Fire onDidChangeConfiguration for listening extensions
                _fireConfigurationChanged(changedPath.length
                    ? changedPath
                    : _configPath(changedSection));
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
