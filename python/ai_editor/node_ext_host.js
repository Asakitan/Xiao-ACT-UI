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
const childProcess = require('node:child_process');
const net = require('node:net');

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
    // Arrow-function class fields (auto-bound to `this` regardless of how
    // the reference is later called) rather than regular prototype
    // methods: real gap, confirmed root cause of "this._listeners is not
    // iterable" in ms-vscode.cmake-tools (2026-07-02 sweep) - it passes a
    // bare `someEmitter.fire` reference around as a plain callback (a very
    // common JS pattern; VS Code's own real EventEmitter is implemented
    // defensively for exactly this reason), and a regular method loses its
    // `this` binding when called that way, so `this._listeners` resolved
    // against the WRONG object. Converting fire/dispose to bound fields
    // makes detached references safe no matter who holds them.
    fire = (data) => {
        for (const fn of [...this._listeners]) {
            try { fn(data); } catch (e) { log('EventEmitter listener error:', e.message); }
        }
    };
    dispose = () => { this._listeners.length = 0; };
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
function _uriEncodePath(value, skipEncoding, scheme) {
    const raw = String(value || '').replace(/\\/g, '/');
    if (skipEncoding) return raw.replace(/[?#]/g, ch => encodeURIComponent(ch));
    const encoded = raw.split('/').map(part => encodeURIComponent(part)).join('/');
    return scheme === 'file' ? encoded.replace(/%3A/gi, ':') : encoded;
}

function _uriNormalizePath(value) {
    let raw = String(value || '').replace(/\\/g, '/');
    raw = raw.replace(/\/+/g, '/');
    if (/^[a-zA-Z]:\//.test(raw)) raw = '/' + raw;
    if (!raw.startsWith('/') && raw) raw = '/' + raw;
    return raw || '/';
}

function _uriFsPath(scheme, authority, uriPath) {
    let raw = String(uriPath || '').replace(/\//g, path.sep);
    if (String(authority || '')) {
        const suffix = raw.startsWith(path.sep) ? raw : path.sep + raw;
        return `${path.sep}${path.sep}${authority}${suffix}`;
    }
    if (/^[\\/][a-zA-Z]:[\\/]/.test(raw)) raw = raw.slice(1);
    return raw;
}

function _uriJoinPath(basePath, segments) {
    if (!basePath) throw new Error('Uri.joinPath requires a base path');
    const cleaned = segments.map(segment => String(segment || '').replace(/\\/g, '/'));
    const driveMatch = /^\/[a-zA-Z]:($|\/)/.exec(basePath);
    let joined = path.posix.normalize(path.posix.join(basePath, ...cleaned));
    if (driveMatch && !/^\/[a-zA-Z]:($|\/)/.test(joined)) {
        const driveRoot = driveMatch[0].replace(/\/$/, '');
        const withoutRoot = joined.replace(/^\/+/, '');
        joined = withoutRoot ? `${driveRoot}/${withoutRoot}` : driveRoot;
    }
    if (!joined.startsWith('/')) joined = '/' + joined;
    return joined;
}

class Uri {
    constructor(scheme, authority, path_, query, fragment) {
        this.scheme = scheme || 'file';
        this.authority = authority || '';
        this.path = path_ || '';
        this.query = query || '';
        this.fragment = fragment || '';
    }
    get fsPath() { return _uriFsPath(this.scheme, this.authority, this.path); }
    toString(skipEncoding) {
        let result;
        const encodedPath = _uriEncodePath(this.path, !!skipEncoding, this.scheme);
        if (this.scheme === 'file') {
            const p = encodedPath.startsWith('/') ? encodedPath : '/' + encodedPath;
            result = `file://${this.authority}${p}`;
        } else if (this.authority) {
            const p = encodedPath.startsWith('/') ? encodedPath : '/' + encodedPath;
            result = `${this.scheme}://${this.authority}${p}`;
        } else {
            result = `${this.scheme}:${encodedPath}`;
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
    toJSON() {
        return {
            scheme: this.scheme,
            authority: this.authority,
            path: this.path,
            query: this.query,
            fragment: this.fragment,
            fsPath: this.fsPath,
        };
    }
    static file(fsPath) {
        const raw = String(fsPath || '').replace(/\\/g, '/');
        const uncMatch = /^\/\/([^/]+)\/(.*)$/.exec(raw);
        if (uncMatch) return new Uri('file', uncMatch[1], '/' + uncMatch[2], '', '');
        return new Uri('file', '', _uriNormalizePath(raw), '', '');
    }
    static parse(value, strict) {
        if (strict && (!value || !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(String(value)))) {
            throw new Error(`Invalid URI: ${value}`);
        }
        try {
            const u = new URL(value);
            return new Uri(u.protocol.replace(/:$/, ''), u.host, decodeURIComponent(u.pathname), u.search.replace(/^\?/, ''), u.hash.replace(/^#/, ''));
        } catch {
            if (strict) throw new Error(`Invalid URI: ${value}`);
            return Uri.file(value);
        }
    }
    static from(value) {
        if (value instanceof Uri) return value;
        if (!value || typeof value !== 'object') {
            throw new Error('Uri.from requires URI components');
        }
        return new Uri(
            value.scheme || 'file',
            value.authority || '',
            value.path || '',
            value.query || '',
            value.fragment || '',
        );
    }
    static revive(value) { return Uri.from(value); }
    static joinPath(base, ...segments) {
        const uri = Uri.from(base);
        return uri.with({ path: _uriJoinPath(uri.path, segments) });
    }
}

function _fileSystemErrorMessage(messageOrUri) {
    if (messageOrUri instanceof Uri) return messageOrUri.toString();
    if (messageOrUri === undefined || messageOrUri === null) return '';
    return String(messageOrUri);
}

// vscode.l10n was entirely missing - real gap, confirmed as the shared root
// cause of "Cannot read properties of undefined (reading 't')" across SIX
// real installed extensions (2026-07-02 sweep): GitHub.copilot-chat,
// ms-dotnettools.csharp, ms-python.debugpy, ms-python.python,
// ms-python.vscode-pylance, ms-python.vscode-python-envs. Every major
// Microsoft extension bundles @vscode/l10n and calls l10n.t(...) at MODULE
// LOAD TIME (building static string tables), before activate() even runs -
// so a missing l10n.t crashed the require() itself. There's no translation
// bundle to look up here, so this mirrors real VS Code's own fallback
// behavior when no translation matches the current locale: return the
// original message with {0}/{name} placeholders substituted from args.
function _l10nFormatMessage(message, args) {
    const text = String(message ?? '');
    let record = null;
    let list = args;
    if (args.length === 1 && args[0] && typeof args[0] === 'object' && !Array.isArray(args[0])) {
        record = args[0];
        list = [];
    }
    return text.replace(/\{([^{}]+)\}/g, (match, key) => {
        if (record && Object.prototype.hasOwnProperty.call(record, key)) {
            return String(record[key]);
        }
        const idx = Number(key);
        if (Number.isInteger(idx) && idx >= 0 && idx < list.length) {
            return String(list[idx]);
        }
        return match;
    });
}
const l10n = {
    t(messageOrOptions, ...args) {
        if (messageOrOptions && typeof messageOrOptions === 'object' && !Array.isArray(messageOrOptions)) {
            const optArgs = messageOrOptions.args;
            // _l10nFormatMessage's own args-array param already treats a
            // single plain-object ELEMENT as the named-placeholder record -
            // so a named `options.args` object must be wrapped in an array,
            // while a positional `options.args` array is passed through.
            const wrapped = Array.isArray(optArgs) ? optArgs
                : (optArgs && typeof optArgs === 'object') ? [optArgs] : [];
            return _l10nFormatMessage(messageOrOptions.message, wrapped);
        }
        return _l10nFormatMessage(messageOrOptions, args);
    },
    bundle: undefined,
    uri: undefined,
};

class FileSystemError extends Error {
    constructor(messageOrUri, code = 'Unknown') {
        super(_fileSystemErrorMessage(messageOrUri));
        this.name = 'FileSystemError';
        this.code = code || 'Unknown';
    }
    static FileNotFound(messageOrUri) { return new FileSystemError(messageOrUri, 'FileNotFound'); }
    static FileExists(messageOrUri) { return new FileSystemError(messageOrUri, 'FileExists'); }
    static FileNotADirectory(messageOrUri) { return new FileSystemError(messageOrUri, 'FileNotADirectory'); }
    static FileIsADirectory(messageOrUri) { return new FileSystemError(messageOrUri, 'FileIsADirectory'); }
    static NoPermissions(messageOrUri) { return new FileSystemError(messageOrUri, 'NoPermissions'); }
    static Unavailable(messageOrUri) { return new FileSystemError(messageOrUri, 'Unavailable'); }
}

// vscode.CancellationError: thrown to signal a cancelled operation. Also
// entirely missing - real gap, confirmed as the second (of at least two)
// root cause of Vue.volar's activation failure, subclassed by its
// vscode-languageclient-derived bundle (e.g. `class LSPCancellationError
// extends vscode.CancellationError`).
class CancellationError extends Error {
    constructor() {
        super('Canceled');
        this.name = 'Canceled';
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
        uri.path,
        uri.query,
        uri.fragment,
    );
}

function _webviewResourceRootPath(root) {
    try {
        const uri = root instanceof Uri ? root : _workspaceUriFromInput(root);
        return path.resolve(uri.fsPath || '').toLowerCase();
    } catch {
        return '';
    }
}

function _webviewResourceWithinRoots(localUri, roots) {
    let fsPath = '';
    try {
        const uri = localUri instanceof Uri ? localUri : _workspaceUriFromInput(localUri);
        fsPath = path.resolve(uri.fsPath || '').toLowerCase();
    } catch {
        return false;
    }
    if (!fsPath) return false;
    return (Array.isArray(roots) ? roots : []).some(root => {
        const rootPath = _webviewResourceRootPath(root);
        return !!rootPath && (fsPath === rootPath || fsPath.startsWith(rootPath + path.sep));
    });
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

class TabInputText {
    constructor(uri) {
        this.uri = uri instanceof Uri ? uri : _uriFromPayload(uri);
    }
}

const NotebookCellKind = Object.freeze({ Markup: 1, Code: 2 });
const NotebookCellStatusBarAlignment = Object.freeze({ Left: 1, Right: 2 });
const NotebookControllerAffinity = Object.freeze({ Default: 1, Preferred: 2 });

class NotebookRange {
    constructor(start, end) {
        const s = Number(start);
        const e = Number(end);
        if (!Number.isFinite(s) || s < 0) throw new Error('start must be positive');
        if (!Number.isFinite(e) || e < 0) throw new Error('end must be positive');
        this.start = Math.min(s, e);
        this.end = Math.max(s, e);
    }
    get isEmpty() { return this.start === this.end; }
    with(change) {
        const nextStart = change && change.start !== undefined ? change.start : this.start;
        const nextEnd = change && change.end !== undefined ? change.end : this.end;
        if (nextStart === this.start && nextEnd === this.end) return this;
        return new NotebookRange(nextStart, nextEnd);
    }
}

class NotebookEdit {
    constructor(range, newCells, notebookMetadata, cellIndex, cellMetadata) {
        this.range = range instanceof NotebookRange
            ? range
            : (range ? new NotebookRange(range.start, range.end) : undefined);
        this.newCells = Array.isArray(newCells)
            ? newCells.map(_notebookCellDataFromPlain)
            : undefined;
        this.newNotebookMetadata = notebookMetadata;
        this.index = cellIndex;
        this.newCellMetadata = cellMetadata;
        this._notebookEdit = true;
        this.kind = 'notebook';
    }
    static replaceCells(range, newCells) {
        return new NotebookEdit(range, Array.isArray(newCells) ? newCells : []);
    }
    static insertCells(index, newCells) {
        const start = Math.max(0, Number(index || 0));
        return new NotebookEdit(new NotebookRange(start, start), Array.isArray(newCells) ? newCells : []);
    }
    static deleteCells(range) {
        return new NotebookEdit(range, []);
    }
    static updateNotebookMetadata(newNotebookMetadata) {
        return new NotebookEdit(undefined, undefined, newNotebookMetadata || {});
    }
    static updateCellMetadata(index, newCellMetadata) {
        return new NotebookEdit(undefined, undefined, undefined, Number(index || 0), newCellMetadata || {});
    }
}

let _nextNotebookOutputId = 1;

class NotebookCellOutputItem {
    constructor(data, mime = 'application/octet-stream') {
        this.data = _contentToUint8Array(data);
        this.mime = String(mime || 'application/octet-stream');
        if (!/^[^/\s]+\/[^;\s]+(?:\s*;.*)?$/.test(this.mime)) {
            throw new Error(`INVALID mime type: ${mime}. Must be in the format "type/subtype[;optionalparameter]"`);
        }
    }
    static bytes(value, mime = 'application/octet-stream') {
        return new NotebookCellOutputItem(value, mime);
    }
    static text(value, mime = 'text/plain') {
        return new NotebookCellOutputItem(new TextEncoder().encode(String(value ?? '')), mime);
    }
    static json(value, mime = 'text/x-json') {
        return NotebookCellOutputItem.text(JSON.stringify(value, undefined, '\t'), mime);
    }
    static stdout(value) {
        return NotebookCellOutputItem.text(value, 'application/vnd.code.notebook.stdout');
    }
    static stderr(value) {
        return NotebookCellOutputItem.text(value, 'application/vnd.code.notebook.stderr');
    }
    static error(err) {
        return NotebookCellOutputItem.json({
            name: err?.name,
            message: err?.message,
            stack: err?.stack,
        }, 'application/vnd.code.notebook.error');
    }
}

class NotebookCellOutput {
    constructor(items, idOrMetadata, metadata) {
        this.items = _dedupeNotebookOutputItems(
            (Array.isArray(items) ? items : []).map(_notebookOutputItemFromPlain));
        if (typeof idOrMetadata === 'string') {
            this.id = idOrMetadata;
            this.metadata = metadata && typeof metadata === 'object' ? metadata : undefined;
        } else {
            this.id = `output-${_nextNotebookOutputId++}`;
            this.metadata = idOrMetadata && typeof idOrMetadata === 'object'
                ? idOrMetadata
                : (metadata && typeof metadata === 'object' ? metadata : undefined);
        }
    }
    static ensureUniqueMimeTypes(items) {
        return _dedupeNotebookOutputItems((Array.isArray(items) ? items : []).map(_notebookOutputItemFromPlain));
    }
}

class NotebookCellStatusBarItem {
    constructor(text, alignment) {
        this.text = String(text ?? '');
        this.alignment = Number(alignment) === NotebookCellStatusBarAlignment.Right
            ? NotebookCellStatusBarAlignment.Right
            : NotebookCellStatusBarAlignment.Left;
        this.command = undefined;
        this.tooltip = undefined;
        this.priority = undefined;
        this.accessibilityInformation = undefined;
    }
}

class NotebookCellData {
    constructor(kind, value, languageId, mime, outputs, metadata, executionSummary) {
        this.kind = Number(kind);
        this.value = String(value ?? '');
        this.languageId = String(languageId ?? '');
        if (!Number.isFinite(this.kind)) {
            throw new Error("NotebookCellData MUST have 'kind' property");
        }
        if (!this.languageId) {
            throw new Error("NotebookCellData MUST have 'languageId' property");
        }
        this.mime = mime;
        this.outputs = Array.isArray(outputs) ? outputs : [];
        this.metadata = metadata && typeof metadata === 'object' ? metadata : {};
        this.executionSummary = executionSummary;
    }
}

class NotebookData {
    constructor(cells) {
        this.cells = Array.isArray(cells) ? cells : [];
        this.metadata = {};
    }
}

function _notebookCellDataFromPlain(cell) {
    if (cell instanceof NotebookCellData) return cell;
    const source = cell && typeof cell === 'object' ? cell : {};
    return new NotebookCellData(
        source.kind ?? NotebookCellKind.Code,
        source.value ?? source.text ?? '',
        source.languageId ?? source.language ?? 'plaintext',
        source.mime,
        (Array.isArray(source.outputs) ? source.outputs : []).map(_notebookOutputFromPlain),
        source.metadata && typeof source.metadata === 'object' ? source.metadata : {},
        source.executionSummary,
    );
}

function _notebookDataFromPlain(data) {
    if (data instanceof NotebookData) return data;
    const source = data && typeof data === 'object' ? data : {};
    const notebook = new NotebookData(
        (Array.isArray(source.cells) ? source.cells : []).map(_notebookCellDataFromPlain));
    notebook.metadata = source.metadata && typeof source.metadata === 'object'
        ? source.metadata
        : {};
    return notebook;
}

function _notebookCellDataPayload(cell) {
    const data = _notebookCellDataFromPlain(cell);
    return {
        kind: data.kind,
        value: data.value,
        languageId: data.languageId,
        mime: data.mime,
        outputs: (data.outputs || []).map(_notebookOutputPayload),
        metadata: _plainBridgeValue(data.metadata || {}),
        executionSummary: _plainBridgeValue(data.executionSummary),
    };
}

function _notebookCellDataClone(cell) {
    const data = _notebookCellDataFromPlain(cell);
    return new NotebookCellData(
        data.kind,
        data.value,
        data.languageId,
        data.mime,
        (data.outputs || []).map(_notebookOutputFromPlain),
        _plainBridgeValue(data.metadata || {}),
        _plainBridgeValue(data.executionSummary),
    );
}

function _filterNotebookMetadata(metadata, transient) {
    const source = metadata && typeof metadata === 'object' ? metadata : {};
    const hidden = transient && typeof transient === 'object' ? transient : {};
    const result = {};
    for (const [key, value] of Object.entries(source)) {
        if (hidden[key]) continue;
        result[key] = value;
    }
    return result;
}

function _notebookDataForSerializer(notebook, options = {}) {
    const data = new NotebookData([]);
    data.metadata = _filterNotebookMetadata(
        notebook?.metadata || notebook?._data?.metadata || {},
        options.transientDocumentMetadata,
    );
    const cells = notebook && typeof notebook.getCells === 'function'
        ? notebook.getCells()
        : [];
    data.cells = cells.map(cell => {
        const cellData = new NotebookCellData(
            cell.kind,
            cell.document?.getText ? cell.document.getText() : '',
            cell.document?.languageId || 'plaintext',
            cell.mime,
            options.transientOutputs ? [] : (cell.outputs || []).map(_notebookOutputFromPlain),
            _filterNotebookMetadata(cell.metadata || {}, options.transientCellMetadata),
            cell.executionSummary,
        );
        return cellData;
    });
    return data;
}

function _notebookDataPayload(data) {
    const notebook = _notebookDataFromPlain(data);
    return {
        cells: notebook.cells.map(_notebookCellDataPayload),
        metadata: _plainBridgeValue(notebook.metadata || {}),
    };
}

function _notebookBytesFromMessage(msg) {
    if (typeof msg.dataBase64 === 'string') {
        return Uint8Array.from(Buffer.from(msg.dataBase64, 'base64'));
    }
    if (Array.isArray(msg.data)) {
        return Uint8Array.from(msg.data.map(value => Number(value) & 255));
    }
    if (msg.dataText !== undefined && msg.dataText !== null) {
        return new TextEncoder().encode(String(msg.dataText));
    }
    return new Uint8Array();
}

function _notebookBytesPayload(value) {
    let bytes = value;
    if (bytes instanceof ArrayBuffer) bytes = new Uint8Array(bytes);
    if (ArrayBuffer.isView(bytes)) {
        const buffer = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        return {
            dataBase64: buffer.toString('base64'),
            dataText: buffer.toString('utf8'),
        };
    }
    const buffer = Buffer.from(String(bytes ?? ''), 'utf8');
    return {
        dataBase64: buffer.toString('base64'),
        dataText: buffer.toString('utf8'),
    };
}

function _notebookOutputItemFromPlain(item) {
    if (item instanceof NotebookCellOutputItem) return item;
    const source = item && typeof item === 'object' ? item : {};
    if (source.dataBase64 !== undefined) {
        return new NotebookCellOutputItem(
            Uint8Array.from(Buffer.from(String(source.dataBase64 || ''), 'base64')),
            source.mime || 'application/octet-stream',
        );
    }
    if (source.data !== undefined) {
        return new NotebookCellOutputItem(source.data, source.mime || 'application/octet-stream');
    }
    if (source.value !== undefined || source.text !== undefined) {
        return NotebookCellOutputItem.text(
            source.value !== undefined ? source.value : source.text,
            source.mime || 'text/plain',
        );
    }
    return new NotebookCellOutputItem(new Uint8Array(), source.mime || 'application/octet-stream');
}

function _isNotebookStreamMime(mime) {
    const normalized = String(mime || '').toLowerCase();
    return normalized === 'application/vnd.code.notebook.stdout'
        || normalized === 'application/vnd.code.notebook.stderr';
}

function _dedupeNotebookOutputItems(items) {
    const seen = new Set();
    const result = [];
    for (const rawItem of Array.isArray(items) ? items : []) {
        const item = _notebookOutputItemFromPlain(rawItem);
        const mime = String(item.mime || '').toLowerCase();
        if (seen.has(mime) && !_isNotebookStreamMime(mime)) continue;
        seen.add(mime);
        result.push(item);
    }
    return result;
}

function _notebookOutputFromPlain(output) {
    if (output instanceof NotebookCellOutput) return output;
    const source = output && typeof output === 'object' ? output : {};
    return new NotebookCellOutput(
        Array.isArray(source.items) ? source.items : [],
        typeof source.id === 'string' ? source.id : source.metadata,
        typeof source.id === 'string' ? source.metadata : undefined,
    );
}

function _notebookOutputItemPayload(item) {
    const outputItem = _notebookOutputItemFromPlain(item);
    const buffer = Buffer.from(
        outputItem.data.buffer,
        outputItem.data.byteOffset,
        outputItem.data.byteLength,
    );
    return {
        mime: outputItem.mime,
        dataBase64: buffer.toString('base64'),
        dataText: buffer.toString('utf8'),
    };
}

function _notebookOutputPayload(output) {
    const normalized = _notebookOutputFromPlain(output);
    return {
        id: normalized.id,
        items: normalized.items.map(_notebookOutputItemPayload),
        metadata: _plainBridgeValue(normalized.metadata || {}),
    };
}

function _notebookCellForDocument(notebook, cellData, index) {
    const cellUri = notebook.uri.with({
        scheme: 'vscode-notebook-cell',
        fragment: String(index),
    });
    const document = {
        uri: cellUri,
        languageId: cellData.languageId,
        version: notebook.version,
        getText: () => cellData.value,
    };
    return {
        index,
        notebook,
        kind: cellData.kind,
        document,
        outputs: cellData.outputs || [],
        metadata: cellData.metadata || {},
        executionSummary: cellData.executionSummary,
    };
}

function _fireNotebookChange(notebook, change) {
    if (!notebook || notebook.isClosed) return;
    notebook.version = Number(notebook.version || 0) + 1;
    notebook.isDirty = true;
    _onDidChangeNotebookDocumentEmitter.fire(Object.assign({
        notebook,
        metadata: undefined,
        cells: [],
        cellChanges: [],
    }, change || {}));
}

function _createNotebookDocument(viewType, uri, data, options = {}) {
    const notebookData = _notebookDataFromPlain(data || new NotebookData([]));
    const notebook = {
        uri,
        notebookType: String(viewType || ''),
        version: 1,
        isDirty: !!options.isDirty,
        isUntitled: uri.scheme === 'untitled',
        isClosed: false,
        _serializerHandle: options.serializerHandle,
        _serializerOptions: options.serializerOptions || {},
        metadata: notebookData.metadata || {},
        _data: notebookData,
        get cellCount() { return this._data.cells.length; },
        cellAt(index) {
            return _notebookCellForDocument(this, this._data.cells[index], index);
        },
        getCells(range) {
            const cells = this._data.cells.map((cell, index) => (
                _notebookCellForDocument(this, cell, index)));
            if (!range) return cells;
            const start = Math.max(0, Number(range.start ?? 0));
            const end = Math.min(cells.length, Number(range.end ?? cells.length));
            return cells.slice(start, end);
        },
        save: async () => {
            const serializer = _notebookSerializerByHandleOrViewType(
                notebook._serializerHandle,
                notebook.notebookType,
            );
            if (serializer && !notebook.isUntitled && notebook.uri.scheme === 'file') {
                const tokenSource = new CancellationTokenSource();
                const bytes = await serializer.entry.serializer.serializeNotebook(
                    _notebookDataForSerializer(notebook, serializer.entry.options || {}),
                    tokenSource.token,
                );
                await _workspaceFsWriteFile(notebook.uri, bytes);
                notebook._serializerHandle = serializer.handle;
                notebook._serializerOptions = serializer.entry.options || {};
            }
            notebook.isDirty = false;
            _onDidSaveNotebookDocumentEmitter.fire(notebook);
            return true;
        },
        close: async () => _closeNotebookDocument(notebook.uri),
        dispose: () => _closeNotebookDocument(notebook.uri),
    };
    if (!options.transientPreview) {
        _notebookDocuments.set(uri.toString(), notebook);
        _onDidOpenNotebookDocumentEmitter.fire(notebook);
    }
    return notebook;
}

function _closeNotebookDocument(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const key = uri.toString();
    const notebook = _notebookDocuments.get(key);
    if (!notebook || notebook.isClosed) return false;
    notebook.isClosed = true;
    _notebookDocuments.delete(key);
    _onDidCloseNotebookDocumentEmitter.fire(notebook);
    return true;
}

class ThemeColor {
    constructor(id) {
        this.id = id === undefined || id === null ? '' : String(id);
    }
}

class ThemeIcon {
    constructor(id, color) {
        this.id = id === undefined || id === null ? '' : String(id);
        if (color !== undefined && color !== null) this.color = color;
    }
}

class FileDecoration {
    constructor(badge, tooltip, color) {
        if (badge !== undefined && badge !== null) this.badge = String(badge);
        if (tooltip !== undefined && tooltip !== null) this.tooltip = String(tooltip);
        if (color !== undefined && color !== null) this.color = color;
        this.propagate = false;
    }
}

class DocumentHighlight {
    constructor(range, kind) {
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        this.kind = kind === undefined || kind === null ? 0 : Number(kind);
    }
}

class EvaluatableExpression {
    constructor(range, expression) {
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        if (expression !== undefined && expression !== null) {
            this.expression = String(expression);
        }
    }
}

class InlineValueText {
    constructor(range, text) {
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        this.text = text === undefined || text === null ? '' : String(text);
    }
}

class InlineValueVariableLookup {
    constructor(range, variableName, caseSensitiveLookup = true) {
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        if (variableName !== undefined && variableName !== null) {
            this.variableName = String(variableName);
        }
        this.caseSensitiveLookup = Boolean(caseSensitiveLookup);
    }
}

class InlineValueEvaluatableExpression {
    constructor(range, expression) {
        this.range = range instanceof Range ? range : _rangeFromPayload(range);
        if (expression !== undefined && expression !== null) {
            this.expression = String(expression);
        }
    }
}

class InlineValueContext {
    constructor(frameId, stoppedLocation) {
        this.frameId = Number.isFinite(Number(frameId)) ? Number(frameId) : 0;
        this.stoppedLocation = stoppedLocation instanceof Range
            ? stoppedLocation
            : _rangeFromPayload(stoppedLocation);
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

class CodeActionKind {
    constructor(value) {
        this.value = value === undefined || value === null ? '' : String(value);
    }
    append(...parts) {
        const suffix = parts.filter(part => part !== undefined && part !== null && String(part))
            .map(part => String(part).replace(/^\.+|\.+$/g, ''))
            .join('.');
        return new CodeActionKind([this.value, suffix].filter(Boolean).join('.'));
    }
    intersects(other) {
        const otherKind = _codeActionKindFromPayload(other);
        return this.contains(otherKind) || otherKind.contains(this);
    }
    contains(other) {
        const otherValue = _codeActionKindFromPayload(other).value;
        return otherValue === this.value || otherValue.startsWith(`${this.value}.`);
    }
    toString() { return this.value; }
}
CodeActionKind.Empty = new CodeActionKind('');
CodeActionKind.QuickFix = CodeActionKind.Empty.append('quickfix');
CodeActionKind.Refactor = CodeActionKind.Empty.append('refactor');
CodeActionKind.RefactorExtract = CodeActionKind.Refactor.append('extract');
CodeActionKind.RefactorInline = CodeActionKind.Refactor.append('inline');
CodeActionKind.RefactorMove = CodeActionKind.Refactor.append('move');
CodeActionKind.RefactorRewrite = CodeActionKind.Refactor.append('rewrite');
CodeActionKind.Source = CodeActionKind.Empty.append('source');
CodeActionKind.SourceOrganizeImports = CodeActionKind.Source.append('organizeImports');
CodeActionKind.SourceFixAll = CodeActionKind.Source.append('fixAll');
CodeActionKind.Notebook = CodeActionKind.Empty.append('notebook');

const CodeActionTriggerKind = {
    Invoke: 1,
    Automatic: 2,
};

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

class McpStdioServerDefinition {
    constructor(label, command, args = [], env = {}, version) {
        this.label = label === undefined || label === null ? '' : String(label);
        this.cwd = undefined;
        this.command = command === undefined || command === null ? '' : String(command);
        this.args = Array.isArray(args)
            ? args.map(item => String(item))
            : [];
        this.env = env && typeof env === 'object' && !Array.isArray(env)
            ? Object.assign({}, env)
            : {};
        if (version !== undefined) this.version = String(version);
    }
}

class McpHttpServerDefinition {
    constructor(label, uri, headers = {}, version) {
        this.label = label === undefined || label === null ? '' : String(label);
        this.uri = uri instanceof Uri ? uri : _uriFromPayload(uri || '');
        this.headers = headers && typeof headers === 'object' && !Array.isArray(headers)
            ? Object.assign({}, headers)
            : {};
        if (version !== undefined) this.version = String(version);
    }
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
// EnvironmentVariableCollection
// -------------------------------------------------------------------------
const EnvironmentVariableMutatorType = {
    Replace: 1,
    Append: 2,
    Prepend: 3,
};

function _environmentScopeKey(scope) {
    const folder = scope && scope.workspaceFolder;
    if (!folder || !folder.uri) return '';
    return folder.uri.toString();
}

function _normalizeEnvironmentMutatorOptions(options) {
    if (!options) return { applyAtProcessCreation: true };
    const normalized = {
        applyAtProcessCreation: options.applyAtProcessCreation ?? false,
        applyAtShellIntegration: options.applyAtShellIntegration ?? false,
    };
    if (!normalized.applyAtProcessCreation && !normalized.applyAtShellIntegration) {
        throw new Error(
            'EnvironmentVariableMutatorOptions must apply at either process creation or shell integration');
    }
    return normalized;
}

function _environmentCollectionShared(extensionId) {
    const key = String(extensionId || 'extension');
    let shared = _environmentVariableCollections.get(key);
    if (!shared) {
        shared = {
            extensionId: key,
            map: new Map(),
            descriptions: new Map(),
            persistent: true,
            emitter: new EventEmitter(),
        };
        _environmentVariableCollections.set(key, shared);
    }
    return shared;
}

function _environmentDescriptionValue(description) {
    if (description === undefined || description === null) return undefined;
    const raw = typeof description === 'string'
        ? description
        : String(description.value ?? description);
    return raw.split('\n\n')[0];
}

class EnvironmentVariableCollection {
    constructor(shared, scope) {
        this._shared = shared || {
            map: new Map(),
            descriptions: new Map(),
            persistent: true,
            emitter: new EventEmitter(),
        };
        this._scope = scope;
        this.onDidChangeCollection = this._shared.emitter.event;
    }

    get persistent() { return this._shared.persistent; }
    set persistent(value) {
        const normalized = !!value;
        if (this._shared.persistent !== normalized) {
            this._shared.persistent = normalized;
            this._shared.emitter.fire();
        }
    }

    get description() {
        return this._shared.descriptions.get(_environmentScopeKey(this._scope));
    }
    set description(value) {
        const key = _environmentScopeKey(this._scope);
        const normalized = _environmentDescriptionValue(value);
        if (normalized === undefined) {
            this._shared.descriptions.delete(key);
        } else {
            this._shared.descriptions.set(key, normalized);
        }
        this._shared.emitter.fire();
    }

    getScoped(scope) {
        return new EnvironmentVariableCollection(this._shared, scope);
    }

    _key(variable) {
        const name = String(variable);
        const scopeKey = _environmentScopeKey(this._scope);
        return scopeKey ? `${name}:::${scopeKey}` : name;
    }

    _set(variable, value, type, options) {
        const name = String(variable);
        this._shared.map.set(this._key(name), {
            variable: name,
            value: String(value),
            type,
            options: _normalizeEnvironmentMutatorOptions(options),
            scope: this._scope,
        });
        this._shared.emitter.fire();
    }

    replace(variable, value, options) {
        this._set(variable, value, EnvironmentVariableMutatorType.Replace, options);
    }

    append(variable, value, options) {
        this._set(variable, value, EnvironmentVariableMutatorType.Append, options);
    }

    prepend(variable, value, options) {
        this._set(variable, value, EnvironmentVariableMutatorType.Prepend, options);
    }

    get(variable) {
        const mutator = this._shared.map.get(this._key(variable));
        if (!mutator) return undefined;
        const { scope, ...publicMutator } = mutator;
        return publicMutator;
    }

    _variableMap() {
        const scopeKey = _environmentScopeKey(this._scope);
        const result = new Map();
        for (const mutator of this._shared.map.values()) {
            if (_environmentScopeKey(mutator.scope) === scopeKey) {
                const { scope, ...publicMutator } = mutator;
                result.set(mutator.variable, publicMutator);
            }
        }
        return result;
    }

    forEach(callback, thisArg) {
        if (typeof callback !== 'function') return;
        for (const [variable, mutator] of this._variableMap()) {
            callback.call(thisArg, variable, mutator, this);
        }
    }

    [Symbol.iterator]() {
        return this._variableMap().entries();
    }

    delete(variable) {
        this._shared.map.delete(this._key(variable));
        this._shared.emitter.fire();
    }

    clear() {
        const scopeKey = _environmentScopeKey(this._scope);
        for (const [key, mutator] of [...this._shared.map.entries()]) {
            if (_environmentScopeKey(mutator.scope) === scopeKey) {
                this._shared.map.delete(key);
            }
        }
        this._shared.descriptions.delete(scopeKey);
        this._shared.emitter.fire();
    }
}

// -------------------------------------------------------------------------
// Webview + WebviewView
// -------------------------------------------------------------------------
let _nextViewHandle = 1;
const WEBVIEW_ARRAY_BUFFER_REF = '$$vscode_array_buffer_reference$$';
const WEBVIEW_TYPED_ARRAY_CTORS = Object.freeze({
    Int8Array,
    Uint8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float32Array,
    Float64Array,
    BigInt64Array,
    BigUint64Array,
});

function _webviewTypedArrayType(value) {
    if (!value || !value.constructor) return '';
    const name = value.constructor.name;
    return Object.prototype.hasOwnProperty.call(WEBVIEW_TYPED_ARRAY_CTORS, name)
        ? name
        : '';
}

function _webviewArrayBufferToBase64(arrayBuffer) {
    return Buffer.from(new Uint8Array(arrayBuffer)).toString('base64');
}

function _webviewArrayBufferFromBase64(value) {
    const buffer = Buffer.from(String(value || ''), 'base64');
    return buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
}

function _serializeWebviewMessageForBridge(message) {
    const buffers = [];
    const addBuffer = (arrayBuffer) => {
        let index = buffers.indexOf(arrayBuffer);
        if (index < 0) {
            index = buffers.length;
            buffers.push(arrayBuffer);
        }
        return index;
    };
    const json = JSON.stringify(message, (_key, value) => {
        if (value instanceof ArrayBuffer) {
            const index = addBuffer(value);
            return {
                [WEBVIEW_ARRAY_BUFFER_REF]: true,
                index,
                dataBase64: _webviewArrayBufferToBase64(value),
                byteLength: value.byteLength,
            };
        }
        if (ArrayBuffer.isView(value)) {
            const type = _webviewTypedArrayType(value);
            if (type) {
                const index = addBuffer(value.buffer);
                return {
                    [WEBVIEW_ARRAY_BUFFER_REF]: true,
                    index,
                    dataBase64: _webviewArrayBufferToBase64(value.buffer),
                    byteLength: value.buffer.byteLength,
                    view: {
                        type,
                        byteLength: value.byteLength,
                        byteOffset: value.byteOffset,
                    },
                };
            }
        }
        return value;
    });
    return JSON.parse(json);
}

function _deserializeWebviewMessageFromBridge(message) {
    if (!message || typeof message !== 'object') return message;
    if (message[WEBVIEW_ARRAY_BUFFER_REF] && typeof message.dataBase64 === 'string') {
        const arrayBuffer = _webviewArrayBufferFromBase64(message.dataBase64);
        const view = message.view;
        if (view && WEBVIEW_TYPED_ARRAY_CTORS[view.type]) {
            const Ctor = WEBVIEW_TYPED_ARRAY_CTORS[view.type];
            return new Ctor(
                arrayBuffer,
                Number(view.byteOffset) || 0,
                Math.max(0, Number(view.byteLength) || 0) / Ctor.BYTES_PER_ELEMENT,
            );
        }
        return arrayBuffer;
    }
    if (Array.isArray(message)) {
        return message.map(item => _deserializeWebviewMessageFromBridge(item));
    }
    const copy = {};
    for (const [key, value] of Object.entries(message)) {
        copy[key] = _deserializeWebviewMessageFromBridge(value);
    }
    return copy;
}

class Webview {
    constructor(viewId, options, defaultLocalResourceRoots) {
        this._viewId = viewId;
        this._html = '';
        this._state = null;
        this._viewType = '';
        this._title = '';
        this._options = options && typeof options === 'object' ? options : {};
        this._defaultLocalResourceRoots = Array.isArray(defaultLocalResourceRoots)
            ? defaultLocalResourceRoots
            : [];
        this._disposed = false;
        this._asWebviewUriStats = {
            count: 0,
            lastSource: '',
            lastUri: '',
            lastInLocalResourceRoot: false,
        };
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
            state: this._state,
            title: this._title,
            viewType: this._viewType,
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
        send({
            type: 'webview_options',
            viewId: this._viewId,
            viewType: this._viewType,
            title: this._title,
            options: this._webviewOptionsPayload(),
            localResourceRoots: this._localResourceRootsPayload(),
        });
    }
    postMessage(message) {
        this._assertAlive();
        let serialized;
        try {
            serialized = _serializeWebviewMessageForBridge(message);
        } catch (err) {
            return Promise.reject(err);
        }
        send({ type: 'webview_post_message', viewId: this._viewId, message: serialized });
        return Promise.resolve(true);
    }
    asWebviewUri(localUri) {
        this._assertAlive();
        const uri = localUri instanceof Uri ? localUri : _workspaceUriFromInput(localUri);
        const webviewUri = _asWebviewResourceUri(uri);
        const roots = this._localResourceRootValues();
        this._asWebviewUriStats = {
            count: Number(this._asWebviewUriStats.count || 0) + 1,
            lastSource: uri.toString(),
            lastUri: webviewUri.toString(),
            lastInLocalResourceRoot: _webviewResourceWithinRoots(uri, roots),
        };
        send({
            type: 'webview_resource_uri',
            viewId: this._viewId,
            viewType: this._viewType,
            title: this._title,
            source: this._asWebviewUriStats.lastSource,
            uri: this._asWebviewUriStats.lastUri,
            inLocalResourceRoot: this._asWebviewUriStats.lastInLocalResourceRoot,
            count: this._asWebviewUriStats.count,
        });
        return webviewUri;
    }
    _setState(state) {
        this._state = state === undefined ? null : state;
    }
    _setPanelMetadata(viewType, title) {
        this._viewType = String(viewType || '');
        this._title = String(title || '');
    }
    _localResourceRootValues() {
        return (
            this._options
            && Array.isArray(this._options.localResourceRoots)
        )
            ? this._options.localResourceRoots
            : this._defaultLocalResourceRoots;
    }
    _localResourceRootsPayload() {
        const roots = this._localResourceRootValues();
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
        if ('enableForms' in options) payload.enableForms = !!options.enableForms;
        if ('enableCommandUris' in options) {
            payload.enableCommandUris = Array.isArray(options.enableCommandUris)
                ? options.enableCommandUris.map(item => String(item || '')).filter(Boolean)
                : !!options.enableCommandUris;
        }
        if (Array.isArray(options.portMapping)) {
            payload.portMapping = options.portMapping
                .map((item) => ({
                    webviewPort: Number(item && item.webviewPort),
                    extensionHostPort: Number(item && item.extensionHostPort),
                }))
                .filter((item) => Number.isFinite(item.webviewPort)
                    && Number.isFinite(item.extensionHostPort));
        }
        if ('retainContextWhenHidden' in options) {
            payload.retainContextWhenHidden = !!options.retainContextWhenHidden;
        }
        const roots = this._localResourceRootsPayload();
        if (roots !== undefined) payload.localResourceRoots = roots;
        payload.defaultLocalResourceRootCount = this._defaultLocalResourceRoots.length;
        payload.asWebviewUriCallCount = Number(this._asWebviewUriStats.count || 0);
        payload.lastAsWebviewUri = this._asWebviewUriStats.lastUri || '';
        payload.lastAsWebviewUriSource = this._asWebviewUriStats.lastSource || '';
        payload.lastAsWebviewUriInLocalResourceRoot = !!this._asWebviewUriStats.lastInLocalResourceRoot;
        return payload;
    }
}

class WebviewView {
    constructor(viewId, viewType, webviewOptions, defaultLocalResourceRoots) {
        this._viewId = viewId;
        this.viewType = viewType;
        this.webview = new Webview(
            viewId, webviewOptions || {}, defaultLocalResourceRoots || []);
        this.webview._setPanelMetadata(viewType, '');
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
    set title(v) {
        const next = String(v || '');
        if (this._title === next) return;
        this._title = next;
        this.webview._setPanelMetadata(this.viewType, next);
        this._emitMetadata();
    }
    get description() { return this._description; }
    set description(v) {
        const next = String(v || '');
        if (this._description === next) return;
        this._description = next;
        this._emitMetadata();
    }
    get badge() { return this._badge; }
    set badge(v) {
        this._badge = v;
        this._emitMetadata();
    }
    show(preserveFocus) {
        if (this._disposed) throw new Error('WebviewView has been disposed');
        this._updateVisibility(true, !!preserveFocus);
    }
    _metadataPayload() {
        return {
            type: 'webview_view_metadata',
            viewId: this._viewId,
            viewType: this.viewType,
            title: this._title,
            description: this._description,
            badge: _plainBridgeValue(this._badge),
            visible: this.visible,
            options: this.webview._webviewOptionsPayload(),
            retainContextWhenHidden: !!(
                this.webview._options
                && this.webview._options.retainContextWhenHidden),
        };
    }
    _emitMetadata() {
        if (this._disposed) return;
        send(this._metadataPayload());
    }
    _updateVisibility(visible, preserveFocus = false) {
        if (this._disposed) return false;
        const nextVisible = !!visible;
        if (this.visible === nextVisible) return false;
        this.visible = nextVisible;
        this._onDidChangeVisibility.fire({ visible: this.visible });
        send({
            type: 'webview_view_visibility',
            viewId: this._viewId,
            viewType: this.viewType,
            visible: this.visible,
            preserveFocus: !!preserveFocus,
        });
        this._emitMetadata();
        return true;
    }
    _updateVisibilityFromHost(visible) {
        return this._updateVisibility(visible, false);
    }
    dispose() {
        if (this._disposed) return;
        this._updateVisibility(false, false);
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
    view.webview._setPanelMetadata(viewType, currentTitle);
    function assertPanelAlive() {
        if (disposed) throw new Error('WebviewPanel has been disposed');
    }
    function updateViewState(nextState) {
        if (disposed) return false;
        const nextVisible = Object.prototype.hasOwnProperty.call(nextState, 'visible')
            ? !!nextState.visible
            : visible;
        const nextActive = Object.prototype.hasOwnProperty.call(nextState, 'active')
            ? !!nextState.active
            : active;
        const nextColumn = Object.prototype.hasOwnProperty.call(nextState, 'viewColumn')
            ? _webviewPanelColumnFromShowOptions(nextState.viewColumn, viewColumn)
            : viewColumn;
        if (visible === nextVisible && active === nextActive && viewColumn === nextColumn) {
            return false;
        }
        visible = nextVisible;
        active = nextActive;
        viewColumn = nextColumn;
        viewStateEmitter.fire({ webviewPanel: panel });
        return true;
    }
    const panel = {
        viewType,
        get title() {
            assertPanelAlive();
            return currentTitle;
        },
        set title(value) {
            assertPanelAlive();
            const nextTitle = String(value || '');
            if (currentTitle === nextTitle) return;
            currentTitle = nextTitle;
            view.title = nextTitle;
            view.webview._setPanelMetadata(viewType, nextTitle);
            send({
                type: 'webview_title',
                viewId,
                viewType,
                title: nextTitle,
            });
        },
        get iconPath() {
            assertPanelAlive();
            return iconPath;
        },
        set iconPath(value) {
            assertPanelAlive();
            if (iconPath === value) return;
            iconPath = value;
            send({
                type: 'webview_icon',
                viewId,
                viewType,
                iconPath: _serializeWebviewPanelIconPath(value),
            });
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
            const nextState = { visible: true, active: !preserveFocus };
            if (nextViewColumn !== undefined) nextState.viewColumn = nextViewColumn;
            updateViewState(nextState);
            send({
                type: 'webview_reveal',
                viewId,
                viewType,
                title: currentTitle,
                viewColumn,
                preserveFocus: !!preserveFocus,
                active,
                visible,
            });
        },
        _updateViewStateFromHost(nextState) {
            return updateViewState(nextState && typeof nextState === 'object' ? nextState : {});
        },
        dispose() {
            if (disposed) return;
            disposed = true;
            visible = false;
            active = false;
            view.dispose();
            _webviewViews.delete(viewId);
            _webviewPanels.delete(viewId);
            viewStateEmitter.dispose();
            send({ type: 'webview_dispose', viewId });
        },
    };
    _webviewPanels.set(viewId, panel);
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

function _customEditorSupportsMultipleEditors(reg) {
    return !!(
        reg
        && reg.options
        && reg.options.supportsMultipleEditorsPerDocument
    );
}

function _customEditorDocumentEntryKey(viewType, uriLike, viewId, supportsMultiple) {
    return _customEditorDocumentKey(
        viewType, uriLike, supportsMultiple ? viewId : '');
}

function _customEditorSingletonEntry(viewType, uriLike) {
    return _customEditorDocuments.get(
        _customEditorDocumentKey(viewType, uriLike)) || null;
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
    const view = entry.viewId ? _webviewViews.get(entry.viewId) : null;
    return {
        viewType: entry.viewType,
        viewId: entry.viewId,
        uri: entry.uri.toString(),
        dirty: !!entry.dirty,
        editable: !!entry.editable,
        textEditor: !!entry.textEditor,
        supportsMultipleEditorsPerDocument:
            !!entry.supportsMultipleEditorsPerDocument,
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
        webviewState: view && view.webview ? view.webview._state : null,
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
    constructor(name, languageId = '', isLog = false) {
        this.name = name;
        this.languageId = languageId || '';
        this._lines = [];
        this._disposed = false;
        // vscode.window.createOutputChannel(name, {log:true}) returns a
        // LogOutputChannel (trace/debug/info/warn/error + logLevel). This
        // was entirely missing - real gap, confirmed as the shared root
        // cause of "this.channel.error/.info is not a function" and
        // "r.trace is not a function" across ms-python.python/debugpy/
        // vscode-pylance/vscode-python-envs and ms-dotnettools.csharp
        // (2026-07-02 sweep) - all use a log channel as their internal
        // logger, called during activation before any real work happens.
        if (isLog) {
            this.logLevel = _envLogLevel;
            this.onDidChangeLogLevel = _onDidChangeLogLevelEmitter.event;
            const levelLine = (level, message, args) => {
                const rest = (args || []).map((a) => {
                    if (a instanceof Error) return a.stack || a.message || String(a);
                    if (typeof a === 'object' && a !== null) { try { return JSON.stringify(a); } catch { return String(a); } }
                    return String(a);
                });
                const text = [String(message ?? ''), ...rest].filter((s) => s !== '').join(' ');
                this.appendLine(`${new Date().toISOString()} [${level}] ${text}`);
            };
            this.trace = (message, ...args) => levelLine('trace', message, args);
            this.debug = (message, ...args) => levelLine('debug', message, args);
            this.info = (message, ...args) => levelLine('info', message, args);
            this.warn = (message, ...args) => levelLine('warn', message, args);
            this.error = (errorOrMessage, ...args) => {
                const message = errorOrMessage instanceof Error
                    ? (errorOrMessage.stack || errorOrMessage.message)
                    : errorOrMessage;
                levelLine('error', message, args);
            };
        }
    }
    get _content() { return this._lines.join(''); }
    append(text) {
        if (this._disposed) return;
        const value = String(text);
        this._lines.push(value);
        send({
            type: 'output',
            channelName: this.name,
            languageId: this.languageId,
            text: value,
            content: this._content,
        });
    }
    appendLine(text) { this.append(text + '\n'); }
    clear() {
        if (this._disposed) return;
        this._lines.length = 0;
        send({ type: 'output_clear', channelName: this.name });
    }
    show(preserveFocus) {
        if (this._disposed) return;
        send({
            type: 'output_show',
            channelName: this.name,
            languageId: this.languageId,
            preserveFocus: !!preserveFocus,
            content: this._content,
        });
    }
    hide() {
        if (this._disposed) return;
        send({ type: 'output_hide', channelName: this.name });
    }
    replace(value) { this.clear(); this.append(value); }
    dispose() {
        if (this._disposed) return;
        this._disposed = true;
        this._lines.length = 0;
        _outputChannels.delete(this.name);
        send({ type: 'output_dispose', channelName: this.name });
    }
}

// vscode.tests (the Testing API) was entirely missing - real gap, confirmed
// shared root cause of "Cannot read properties of undefined (reading
// 'createTestController')" for BOTH ms-python.python and
// ms-python.vscode-pylance (2026-07-02 sweep) - Python's pytest/unittest
// discovery integration builds its test tree through this API during
// activation. This app has no Test Explorer UI panel to visually run/
// display tests (a separate, much larger feature), so this is scoped like
// registerDebugAdapterTrackerFactory elsewhere in this file: a real,
// functioning registry/object graph matching VS Code's actual shape (so
// extensions that build a test tree and register run profiles don't crash)
// without a UI consumer wired up yet.
class TestTag {
    constructor(id) { this.id = String(id ?? ''); }
}
class TestMessage {
    constructor(message) {
        this.message = message;
        this.expectedOutput = undefined;
        this.actualOutput = undefined;
        this.location = undefined;
        this.contextValue = undefined;
    }
    static diff(message, expected, actual) {
        const m = new TestMessage(message);
        m.expectedOutput = expected;
        m.actualOutput = actual;
        return m;
    }
}
class TestRunRequest {
    constructor(include, exclude, profile, continuous) {
        this.include = include || undefined;
        this.exclude = exclude || undefined;
        this.profile = profile;
        this.continuous = !!continuous;
    }
}
class TestItemCollection {
    constructor() { this._map = new Map(); }
    get size() { return this._map.size; }
    add(item) { if (item && item.id !== undefined) this._map.set(String(item.id), item); }
    delete(id) { this._map.delete(String(id)); }
    get(id) { return this._map.get(String(id)); }
    forEach(callback, thisArg) {
        this._map.forEach((value) => callback.call(thisArg, value, this));
    }
    replace(items) {
        this._map.clear();
        for (const item of (items || [])) this.add(item);
    }
    [Symbol.iterator]() { return this._map.values(); }
}
function _createTestItem(controllerId, id, label, uri) {
    return {
        id: String(id ?? ''),
        label: String(label ?? ''),
        uri,
        busy: false,
        canResolveChildren: false,
        description: undefined,
        sortText: undefined,
        tags: [],
        range: undefined,
        error: undefined,
        parent: undefined,
        children: new TestItemCollection(),
    };
}
function _createTestRun(request, name, persist) {
    const tokenSource = new CancellationTokenSource();
    return {
        name: name || '',
        token: tokenSource.token,
        isPersisted: persist !== false,
        enqueued() {}, started() {}, skipped() {},
        failed() {}, errored() {}, passed() {},
        appendOutput() {}, appendMessage() {},
        end() { tokenSource.dispose(); },
    };
}
function _createTestController(id, label) {
    const items = new TestItemCollection();
    const profiles = new Set();
    const controller = {
        id: String(id ?? ''),
        label: String(label ?? ''),
        items,
        refreshHandler: undefined,
        resolveHandler: undefined,
        createTestItem(itemId, itemLabel, uri) {
            return _createTestItem(controller.id, itemId, itemLabel, uri);
        },
        createRunProfile(profileLabel, kind, runHandler, isDefault, tag, supportsContinuousRun) {
            const changeEmitter = new EventEmitter();
            const profile = {
                label: String(profileLabel ?? ''),
                kind: kind === undefined ? 1 : kind,
                isDefault: !!isDefault,
                tag,
                supportsContinuousRun: !!supportsContinuousRun,
                runHandler: typeof runHandler === 'function' ? runHandler : (() => {}),
                configureHandler: undefined,
                onDidChangeDefault: changeEmitter.event,
                dispose() { profiles.delete(profile); },
            };
            profiles.add(profile);
            return profile;
        },
        createTestRun(request, name, persist) {
            return _createTestRun(request, name, persist);
        },
        invalidateTestResults() {},
        dispose() { _testControllers.delete(controller.id); },
    };
    _testControllers.set(controller.id, controller);
    return controller;
}
const _testControllers = new Map(); // id -> TestController

class ProcessExecution {
    constructor(processValue, argsOrOptions, options) {
        if (typeof processValue !== 'string') {
            throw new Error('process must be a string');
        }
        this.process = processValue;
        if (Array.isArray(argsOrOptions)) {
            this.args = argsOrOptions.map(item => String(item));
            this.options = options;
        } else {
            this.args = [];
            this.options = argsOrOptions;
        }
    }
    computeId() {
        return ['process', this.process, ...(this.args || [])]
            .map(item => String(item).replace(/,/g, ',,'))
            .join(',') + ',';
    }
}

class ShellExecution {
    constructor(commandLineOrCommand, argsOrOptions, options) {
        if (Array.isArray(argsOrOptions)) {
            if (commandLineOrCommand === undefined || commandLineOrCommand === null) {
                throw new Error('command cannot be undefined or null');
            }
            this.command = commandLineOrCommand;
            this.args = argsOrOptions;
            this.options = options;
            this.commandLine = undefined;
        } else {
            if (typeof commandLineOrCommand !== 'string') {
                throw new Error('commandLine must be a string');
            }
            this.commandLine = String(commandLineOrCommand || '');
            this.command = undefined;
            this.args = [];
            this.options = argsOrOptions;
        }
    }
    computeId() {
        const values = ['shell'];
        if (this.commandLine !== undefined) values.push(this.commandLine);
        if (this.command !== undefined) values.push(_shellTokenText(this.command));
        for (const arg of this.args || []) values.push(_shellTokenText(arg));
        return values.map(item => String(item).replace(/,/g, ',,')).join(',') + ',';
    }
}

class CustomExecution {
    constructor(callback) {
        if (typeof callback !== 'function') {
            throw new Error('CustomExecution callback must be a function');
        }
        this.callback = callback;
    }
    computeId() {
        return `customExecution:${Date.now()}:${Math.random().toString(16).slice(2)}`;
    }
}

class TaskGroup {
    constructor(id, label) {
        if (typeof id !== 'string' || typeof label !== 'string') {
            throw new Error('TaskGroup id and label must be strings');
        }
        this._id = id;
        this.label = label;
        this.isDefault = undefined;
    }
    get id() { return this._id; }
    static from(value) {
        switch (value) {
            case 'clean': return TaskGroup.Clean;
            case 'build': return TaskGroup.Build;
            case 'rebuild': return TaskGroup.Rebuild;
            case 'test': return TaskGroup.Test;
            default: return undefined;
        }
    }
}
TaskGroup.Clean = new TaskGroup('clean', 'Clean');
TaskGroup.Build = new TaskGroup('build', 'Build');
TaskGroup.Rebuild = new TaskGroup('rebuild', 'Rebuild');
TaskGroup.Test = new TaskGroup('test', 'Test');

function _taskNormalizeProblemMatchers(value) {
    if (typeof value === 'string') return [value];
    if (Array.isArray(value)) {
        return value
            .filter(item => item !== undefined && item !== null)
            .map(item => {
                if (typeof item === 'string') return item;
                if (item && typeof item === 'object') {
                    return _plainBridgeValue(item);
                }
                return String(item);
            });
    }
    if (value && typeof value === 'object') return [_plainBridgeValue(value)];
    return [];
}

class Task {
    constructor(definition, scopeOrName, nameOrSource, sourceOrExecution,
            executionOrProblemMatchers, problemMatchers) {
        this.definition = definition && typeof definition === 'object'
            ? definition
            : {};
        if (typeof scopeOrName === 'string') {
            this.scope = undefined;
            this.name = scopeOrName;
            this.source = String(nameOrSource || '');
            this.execution = sourceOrExecution;
            this.problemMatchers = _taskNormalizeProblemMatchers(
                executionOrProblemMatchers);
            this.hasDefinedMatchers = executionOrProblemMatchers !== undefined;
        } else {
            this.scope = scopeOrName;
            this.name = String(nameOrSource || this.definition.type || 'task');
            this.source = String(sourceOrExecution || '');
            this.execution = executionOrProblemMatchers;
            this.problemMatchers = _taskNormalizeProblemMatchers(problemMatchers);
            this.hasDefinedMatchers = problemMatchers !== undefined;
        }
        this.isBackground = false;
        this.group = undefined;
        this.detail = undefined;
        this.presentationOptions = {};
        this.runOptions = {};
    }
}

class DebugAdapterExecutable {
    constructor(command, args = [], options = undefined) {
        this.command = String(command || '');
        this.args = Array.isArray(args) ? args.map(item => String(item)) : [];
        this.options = options;
    }
}

class DebugAdapterServer {
    constructor(port, host = undefined) {
        this.port = Number(port);
        this.host = host === undefined || host === null
            ? undefined
            : String(host);
    }
}

class DebugAdapterNamedPipeServer {
    constructor(pathValue) {
        this.path = String(pathValue || '');
    }
}

class Breakpoint {
    constructor(enabled = true, condition = undefined, hitCondition = undefined, logMessage = undefined) {
        this.id = `breakpoint-${_nextDebugBreakpointHandle++}`;
        this.enabled = enabled === undefined ? true : !!enabled;
        this.condition = condition === undefined || condition === null
            ? undefined : String(condition);
        this.hitCondition = hitCondition === undefined || hitCondition === null
            ? undefined : String(hitCondition);
        this.logMessage = logMessage === undefined || logMessage === null
            ? undefined : String(logMessage);
    }
}

class SourceBreakpoint extends Breakpoint {
    constructor(location, enabled = true, condition = undefined, hitCondition = undefined, logMessage = undefined) {
        super(enabled, condition, hitCondition, logMessage);
        this.location = location;
    }
}

class FunctionBreakpoint extends Breakpoint {
    constructor(functionName, enabled = true, condition = undefined, hitCondition = undefined, logMessage = undefined) {
        super(enabled, condition, hitCondition, logMessage);
        this.functionName = String(functionName || '');
    }
}

class DataBreakpoint extends Breakpoint {
    constructor(dataId, canPersist = false, label = undefined, accessTypes = undefined, accessType = undefined, enabled = true, condition = undefined, hitCondition = undefined) {
        super(enabled, condition, hitCondition, undefined);
        this.dataId = String(dataId || '');
        this.canPersist = !!canPersist;
        this.label = label === undefined || label === null ? undefined : String(label);
        this.accessTypes = Array.isArray(accessTypes)
            ? accessTypes.map(item => String(item))
            : undefined;
        this.accessType = accessType === undefined || accessType === null
            ? undefined : String(accessType);
    }
}

class DebugAdapterInlineImplementation {
    constructor(implementation) {
        this.implementation = implementation;
    }
}

// -------------------------------------------------------------------------
// Global registries
// -------------------------------------------------------------------------
const _extensions = new Map();           // extensionId -> { desc, module, context, deactivate }
const _knownExtensions = new Map();      // extensionId -> { extensionPath, manifest, extensionKind }
const _configurationDefaults = {};       // contributed default settings by dotted path
const _configurationLanguageDefaults = {}; // languageId -> defaults by dotted path
const _configurationUpdateTargets = {};  // dotted path -> last VS Code ConfigurationTarget
const _configurationTargetValues = {
    global: {},
    workspace: {},
    workspaceFolder: {},
};
const _extensionActivationRequests = new Map(); // requestId -> pending activation request
const _extensionActivationInFlight = new Map(); // extensionId -> pending activation request
const _activationEventInFlight = new Map(); // activationEvent -> Promise<number>
const _commands = new Map();             // commandId -> handler
// 'setContext' is a core VS Code built-in command (vscode.commands.
// executeCommand('setContext', key, value), used to set "when"-clause
// context keys) - it was entirely missing, so any extension calling it
// (very common - many extensions gate a command/view on their own state)
// hit "Command not found: setContext" via the Python round-trip. Real gap,
// confirmed shared root cause for ms-vscode.cmake-tools and
// ms-vscode.cpptools (2026-07-02 sweep), both call it during activation.
// Stored here (not wired to the frontend's own separate when-clause
// evaluation yet - a further step, not needed to unblock activation) so
// the command exists and functions rather than crashing.
const _contextKeys = new Map();
const _onDidChangeContextEmitter = new EventEmitter();
_commands.set('setContext', (key, value) => {
    _contextKeys.set(String(key ?? ''), value);
    _onDidChangeContextEmitter.fire({ key: String(key ?? ''), value });
});
const _pythonCommandRequests = new Map(); // requestId -> { resolve, reject, timer }
const _pythonLmRequests = new Map();      // requestId -> { resolve, reject, timer }
const _windowDialogRequests = new Map();  // requestId -> { resolve, timer, cleanup, kind }
const _windowMessageRequests = new Map();  // requestId -> { resolve, timer, cleanup, items }
const _envClipboardRequests = new Map();  // requestId -> { resolve, timer, cleanup, action }
const _webviewViewProviders = new Map(); // viewType -> { provider, options }
const _webviewViews = new Map();         // viewId -> WebviewView
const _webviewViewActivationResolving = new Set(); // viewType pending explicit onView resolve
const _webviewPanels = new Map();        // viewId -> WebviewPanel-like object
const _webviewPanelSerializers = new Map(); // viewType -> { serializer, extensionId, extensionPath }
const _customEditorProviders = new Map(); // viewType -> { provider, options, extensionId }
const _customEditorDocuments = new Map(); // viewType|uri|viewId -> resolved custom document state
const _customEditorViewKeys = new Map();  // viewId -> custom editor document key
const _treeDataProviders = new Map();    // viewId -> { provider, disposable? }
const _treeViews = new Map();            // viewId -> TreeView-like object
const _treeElementStores = new Map();    // viewId -> element handle store
const _outputChannels = new Map();       // name -> OutputChannel
const _taskExecutions = [];              // active TaskExecution-like objects
const _fileSystemProviders = new Map();  // scheme -> { provider, options, extensionId }
const _textDocumentContentProviders = new Map(); // scheme -> { provider, extensionId }
const _uriHandlers = new Map();          // extensionId -> { handler }
const _languageProviders = [];           // { kind, selector, provider, triggers?, disposable }
const _languageProviderRequests = new Map(); // requestId -> CancellationTokenSource
const _languageStatusItems = new Map();  // fullyQualifiedId -> language status state
const _fileDecorationProviders = [];     // { handle, extensionId, provider, disposable? }
const _fileDecorationRequests = new Map(); // requestId -> CancellationTokenSource
const _scmQuickDiffRequests = new Map(); // requestId -> CancellationTokenSource
const _scmHistoryRequests = new Map(); // requestId -> CancellationTokenSource
const _fileDecorationChangeMaxEventSize = 250;
const _runtimeLanguageConfigurations = new Map(); // languageId -> [{ handle, configuration }]
const _lmTools = new Map();              // name -> { handle, tool, extensionId, metadata }
const _lmChatProviders = new Map();      // vendor -> { handle, vendor, provider, extensionId }
const _mcpServerDefinitionProviders = new Map(); // handle -> { handle, id, provider, extensionId }
const _chatParticipants = new Map();     // id -> { handle, handler, extensionId }
const _chatContextProviders = new Map(); // handle -> { handle, kind, id, selector, provider, extensionId }
let _nextLanguageProviderHandle = 1;
let _nextLanguageStatusHandle = 1;
let _nextFileDecorationProviderHandle = 1;
let _nextLanguageConfigurationHandle = 1;
let _nextLmToolHandle = 1;
let _nextLmChatProviderHandle = 1;
let _nextMcpServerDefinitionProviderHandle = 1;
let _nextChatParticipantHandle = 1;
let _nextChatContextProviderHandle = 1;
let _nextPythonCommandRequestHandle = 1;
let _nextPythonLmRequestHandle = 1;
let _nextExtensionActivationRequestHandle = 1;
let _nextWindowDialogRequestHandle = 1;
let _nextWindowMessageRequestHandle = 1;
let _nextEnvClipboardRequestHandle = 1;
let _nextTaskExecutionHandle = 1;
let _nextDebugSessionHandle = 1;
let _nextDebugBreakpointHandle = 1;
let _activeDebugSession = null;
const _debugAdapterTrackerFactories = []; // { type, factory }
let _envClipboardFallbackText = '';
const _languageDocumentTextCache = new Map(); // uri -> { version, text }
const _workspaceTextDocuments = new Map(); // uri -> TextDocument-like object
const _onDidOpenTextDocumentEmitter = new EventEmitter();
const _onDidCloseTextDocumentEmitter = new EventEmitter();
const _onDidChangeTextDocumentEmitter = new EventEmitter();
const _onDidSaveTextDocumentEmitter = new EventEmitter();
const _onDidChangeActiveTextEditorEmitter = new EventEmitter();
const _onDidChangeVisibleTextEditorsEmitter = new EventEmitter();
const _onDidChangeTextEditorSelectionEmitter = new EventEmitter();
const _onDidChangeTextEditorOptionsEmitter = new EventEmitter();
const _onDidChangeTextEditorVisibleRangesEmitter = new EventEmitter();
const _onDidChangeTextEditorViewColumnEmitter = new EventEmitter();
const _onDidChangeTextEditorDiffInformationEmitter = new EventEmitter();
const _onDidChangeTabGroupsEmitter = new EventEmitter();
const _onDidChangeTabsEmitter = new EventEmitter();
let _activeTextEditor = undefined;
const _visibleTextEditors = new Map(); // uri -> TextEditor-like object
const _textEditorDecorationTypes = new Map(); // key -> { key, options }
let _nextTextEditorDecorationHandle = 1;
const _onDidChangeLmToolsEmitter = new EventEmitter();
const _onDidChangeLmChatModelsEmitter = new EventEmitter();
const _workspaceRoot = path.resolve(process.cwd());
const _workspaceName = path.basename(_workspaceRoot) || _workspaceRoot;
const _onDidChangeWorkspaceFoldersEmitter = new EventEmitter();
const _onDidGrantWorkspaceTrustEmitter = new EventEmitter();
let _workspaceTrusted = process.env.SAO_AI_EDITOR_WORKSPACE_TRUSTED === '0' ? false : true;
let _workspaceFolders = [{
    uri: Uri.file(_workspaceRoot),
    name: _workspaceName,
    index: 0,
}];
let _nextNotebookSerializerHandle = 1;
let _nextNotebookDocumentHandle = 1;
const _notebookSerializers = new Map(); // handle -> { viewType, serializer, options, extensionId }
const _notebookDocuments = new Map(); // uri string -> notebook document
let _nextNotebookControllerHandle = 1;
const _notebookControllers = new Map(); // handle -> controller
const _selectedNotebookControllers = new Map(); // notebook uri string -> controller handle
let _nextNotebookStatusBarProviderHandle = 1;
const _notebookStatusBarProviders = new Map(); // handle -> { notebookType, provider, extensionId, subscription }
const _onDidOpenNotebookDocumentEmitter = new EventEmitter();
const _onDidCloseNotebookDocumentEmitter = new EventEmitter();
const _onDidChangeNotebookDocumentEmitter = new EventEmitter();
const _onDidSaveNotebookDocumentEmitter = new EventEmitter();
const _terminals = [];
let _activeTerminal = undefined;
let _nextTerminalHandle = 1;
const _terminalProfileProviders = new Map(); // id -> { provider, extensionId }
const _terminalLinkProviders = new Set();
const _terminalLinkCache = new Map(); // terminal handle -> Map<link id, { provider, link }>
const _environmentVariableCollections = new Map(); // extension id -> shared collection
let _nextTerminalLinkHandle = 1;
let _nextTerminalShellExecutionHandle = 1;
const _onDidChangeActiveTerminalEmitter = new EventEmitter();
const _onDidOpenTerminalEmitter = new EventEmitter();
const _onDidCloseTerminalEmitter = new EventEmitter();
const _onDidChangeTerminalStateEmitter = new EventEmitter();
const _onDidChangeTerminalShellIntegrationEmitter = new EventEmitter();
const _onDidStartTerminalShellExecutionEmitter = new EventEmitter();
const _onDidEndTerminalShellExecutionEmitter = new EventEmitter();
const _workspaceDefaultSkipDirs = new Set(['.git', 'node_modules', '__pycache__', '.venv', 'venv']);
const _workspaceSymbolCache = new Map(); // handle -> { provider, symbol }
const _fileSystemWatchers = new Set();
let _nextWorkspaceSymbolHandle = 1;
const _completionItemCache = new Map(); // handle -> { provider, item, resolved }
let _nextCompletionItemHandle = 1;
const _resolvableLanguageItemCache = new Map(); // handle -> { provider, item, kind, resolved }
let _nextResolvableLanguageItemHandle = 1;
const _hierarchyItemCache = new Map(); // handle -> { provider, item, kind }
let _nextHierarchyItemHandle = 1;
const _diagnosticCollections = new Map(); // name -> DiagnosticCollection
const _onDidChangeDiagnosticsEmitter = new EventEmitter();
let _nextCustomEditorEditHandle = 1;
const _debugAdapterFactories = new Map();   // type -> factory
const _debugVisualizationTreeProviders = new Map(); // id -> provider
const _debugVisualizationProviders = new Map();     // id -> provider
const _debugConfigProviders = new Map();    // type -> provider
const _taskProviders = new Map();           // type -> provider
const _taskProviderStates = new Map();      // type -> provider metadata
const _debugAdapterFactoryStates = new Map(); // type -> factory metadata
let _nextTaskProviderHandle = 1;
let _nextDebugConfigProviderHandle = 1;
let _nextDebugAdapterFactoryHandle = 1;
let _nextDebugAdapterTrackerHandle = 1;
const _scmProviders = new Map();            // id -> SourceControl
const _authenticationProviders = new Map(); // id -> { label, provider, options, listener }
const _authenticationSessions = new Map();  // id -> AuthenticationSession[]
const _onDidChangeAuthenticationSessionsEmitter = new EventEmitter();
const _onDidChangeConfigurationEmitter = new EventEmitter();
// workspace.onDid/onWill{Create,Delete,Rename}Files were entirely missing -
// real gap, confirmed shared root cause of "workspace.onDidDeleteFiles is
// not a function" (ms-python.vscode-python-envs, 2026-07-02 sweep), which
// subscribes during activation. This app has no workspace-wide recursive
// file-system watcher (createFileSystemWatcher is per-glob, not global), so
// these honestly never fire yet - same "declared but not wired" pattern
// already used for window.onDidChangeWindowState - not overclaiming.
const _onWillCreateFilesEmitter = new EventEmitter();
const _onDidCreateFilesEmitter = new EventEmitter();
const _onWillDeleteFilesEmitter = new EventEmitter();
const _onDidDeleteFilesEmitter = new EventEmitter();
const _onWillRenameFilesEmitter = new EventEmitter();
const _onDidRenameFilesEmitter = new EventEmitter();
const _portAttributesProviders = new Map(); // handle -> { provider, portSelector }
let _nextPortAttributesProviderHandle = 1;
const _windowState = Object.freeze({ focused: true, active: true });
const _onDidChangeWindowStateEmitter = new EventEmitter();
const _envLogLevel = 3; // vscode.LogLevel.Info
const _onDidChangeTelemetryEnabledEmitter = new EventEmitter();
const _onDidChangeShellEmitter = new EventEmitter();
const _onDidChangeLogLevelEmitter = new EventEmitter();
let _nextUntitledDocument = 1;
let _nextProgressHandle = 1;
let _nextQuickInputHandle = 1;
let _activeQuickInput = null;
const _quickInputs = new Map();          // quickInput id -> QuickInputBase

const _languageProviderResolveMethods = {
    completion: 'resolveCompletionItem',
    documentLink: 'resolveDocumentLink',
    inlayHint: 'resolveInlayHint',
    codeLens: 'resolveCodeLens',
    codeActions: 'resolveCodeAction',
    documentPaste: 'resolveDocumentPasteEdit',
    documentDrop: 'resolveDocumentDropEdit',
    workspaceSymbol: 'resolveWorkspaceSymbol',
};

function _languageProviderResolveSupport(kind, provider) {
    const method = _languageProviderResolveMethods[String(kind || '')] || '';
    return {
        supported: !!(method && provider && typeof provider[method] === 'function'),
        method,
    };
}

function _workspaceDocumentIsOpened(document) {
    return !!document && document.__opened !== false;
}

function _normalizeViewColumn(value) {
    const raw = Number(value);
    if (!Number.isFinite(raw)) return 1;
    if (raw === -2) return 2;
    return raw > 0 ? raw : 1;
}

function _showTextDocumentOptions(options) {
    if (typeof options === 'number') return { viewColumn: _normalizeViewColumn(options) };
    if (!options || typeof options !== 'object') return { viewColumn: 1 };
    return {
        viewColumn: _normalizeViewColumn(options.viewColumn),
        preserveFocus: !!options.preserveFocus,
        preview: options.preview !== false,
        selection: options.selection ? _rangeFromPayload(options.selection) : undefined,
    };
}

function _selectionFromPayload(value) {
    if (value instanceof Selection) return value;
    if (value instanceof Range) return new Selection(value.start, value.end);
    const range = _rangeFromPayload(value);
    return new Selection(range.start, range.end);
}

function _textEditorSelections(value) {
    if (Array.isArray(value)) {
        const selections = value.map(_selectionFromPayload);
        return selections.length ? selections : [new Selection(new Position(0, 0), new Position(0, 0))];
    }
    return [_selectionFromPayload(value)];
}

function _setTextEditorSelections(editor, value, kind = 'api') {
    if (!editor) return;
    const selections = _textEditorSelections(value);
    editor._selections = selections;
    _onDidChangeTextEditorSelectionEmitter.fire({
        textEditor: editor,
        selections,
        kind,
    });
}

function _setTextEditorOptions(editor, value) {
    if (!editor) return;
    const next = value && typeof value === 'object'
        ? Object.assign({}, value)
        : {};
    editor._options = Object.assign({
        tabSize: 4,
        insertSpaces: true,
    }, next);
    _onDidChangeTextEditorOptionsEmitter.fire({
        textEditor: editor,
        options: editor._options,
    });
}

function _setTextEditorVisibleRanges(editor, value) {
    if (!editor) return;
    const ranges = Array.isArray(value)
        ? value.map(item => item instanceof Range ? item : _rangeFromPayload(item))
        : [value instanceof Range ? value : _rangeFromPayload(value)];
    editor._visibleRanges = ranges;
    _onDidChangeTextEditorVisibleRangesEmitter.fire({
        textEditor: editor,
        visibleRanges: ranges,
    });
}

function _setTextEditorViewColumn(editor, value) {
    if (!editor) return false;
    const next = _normalizeViewColumn(value);
    const previous = _normalizeViewColumn(editor.viewColumn);
    editor.viewColumn = next;
    if (previous !== next) {
        _onDidChangeTextEditorViewColumnEmitter.fire({
            textEditor: editor,
            viewColumn: next,
        });
        _fireEditorTabEvents('change', editor);
        return true;
    }
    return false;
}

function _setTextEditorDiffInformation(editor, value) {
    if (!editor) return;
    const next = value && typeof value === 'object'
        ? _plainBridgeValue(Object.assign({}, value))
        : {};
    editor._diffInformation = next;
    _onDidChangeTextEditorDiffInformationEmitter.fire({
        textEditor: editor,
        diffInformation: next,
    });
}

function _decorationTypeKey(decorationType) {
    if (!decorationType) return '';
    return String(decorationType.key || decorationType.id || decorationType);
}

function _decorationRangePayload(value) {
    if (value instanceof Range) return { range: value };
    if (value && typeof value === 'object' && value.range) {
        return {
            range: value.range instanceof Range ? value.range : _rangeFromPayload(value.range),
            hoverMessage: value.hoverMessage,
            renderOptions: value.renderOptions,
        };
    }
    return { range: _rangeFromPayload(value) };
}

function _normalizeDecorationRanges(value) {
    if (!Array.isArray(value)) return [];
    return value.map(_decorationRangePayload);
}

function _createTextEditorDecorationType(options) {
    const key = `sao-decoration-${_nextTextEditorDecorationHandle++}`;
    const entry = {
        key,
        options: _plainBridgeValue(options || {}),
    };
    _textEditorDecorationTypes.set(key, entry);
    send({
        type: 'text_editor_decoration_type_registered',
        key,
        options: entry.options,
    });
    return {
        key,
        dispose() {
            _textEditorDecorationTypes.delete(key);
            for (const editor of _visibleTextEditors.values()) {
                if (editor._decorations) editor._decorations.delete(key);
            }
            send({ type: 'text_editor_decoration_type_disposed', key });
        },
    };
}

function _documentTabLabel(document) {
    if (!document || !document.uri) return '';
    if (document.uri.scheme === 'untitled') {
        const base = path.basename(document.uri.path || '');
        return base || 'Untitled';
    }
    if (document.uri.scheme === 'file') return path.basename(document.uri.fsPath || '');
    return path.basename(document.uri.path || '') || document.uri.toString();
}

function _editorTabObject(editor, group) {
    const document = editor && editor.document;
    const tab = {
        get isActive() { return _activeTextEditor === editor; },
        get label() { return _documentTabLabel(document); },
        get input() { return new TabInputText(document.uri); },
        get isDirty() { return !!document.isDirty; },
        get isPinned() { return true; },
        get isPreview() { return false; },
        get group() { return group; },
        _editor: editor,
    };
    return tab;
}

function _editorTabGroupsSnapshot() {
    const byColumn = new Map();
    for (const editor of _visibleTextEditors.values()) {
        const column = _normalizeViewColumn(editor.viewColumn);
        if (!byColumn.has(column)) byColumn.set(column, []);
        byColumn.get(column).push(editor);
    }
    if (!byColumn.size) byColumn.set(1, []);
    const activeColumn = _activeTextEditor
        ? _normalizeViewColumn(_activeTextEditor.viewColumn)
        : Math.min(...byColumn.keys());
    return [...byColumn.entries()]
        .sort((a, b) => a[0] - b[0])
        .map(([viewColumn, editors]) => {
            const group = {
                _tabsCache: undefined,
                get isActive() { return viewColumn === activeColumn; },
                viewColumn,
                get activeTab() {
                    return this.tabs.find(tab => tab.isActive) || this.tabs[0];
                },
                get tabs() {
                    if (!this._tabsCache) {
                        this._tabsCache = Object.freeze(
                            editors.map(editor => _editorTabObject(editor, group)));
                    }
                    return this._tabsCache;
                },
            };
            return group;
        });
}

function _fireEditorTabEvents(kind, editor) {
    const groups = _editorTabGroupsSnapshot();
    let tab = editor
        ? groups.flatMap(group => group.tabs)
            .find(candidate => candidate._editor === editor)
        : undefined;
    if (!tab && editor && editor.document) {
        const fallbackGroup = {
            isActive: false,
            viewColumn: _normalizeViewColumn(editor.viewColumn),
            activeTab: undefined,
            tabs: Object.freeze([]),
        };
        tab = _editorTabObject(editor, fallbackGroup);
    }
    _onDidChangeTabGroupsEmitter.fire({
        opened: [],
        closed: [],
        changed: groups,
    });
    _onDidChangeTabsEmitter.fire({
        opened: kind === 'open' && tab ? [tab] : [],
        closed: kind === 'close' && tab ? [tab] : [],
        changed: kind === 'change' && tab ? [tab] : [],
    });
}

function _fireVisibleTextEditorsChanged(kind, editor) {
    _onDidChangeVisibleTextEditorsEmitter.fire(
        Array.from(_visibleTextEditors.values()));
    _fireEditorTabEvents(kind, editor);
}

function _closeTextEditor(editor) {
    if (!editor || !editor.document || !editor.document.uri) return false;
    const key = editor.document.uri.toString();
    if (_visibleTextEditors.get(key) !== editor) return false;
    _visibleTextEditors.delete(key);
    if (_activeTextEditor === editor) {
        _activeTextEditor = undefined;
        _onDidChangeActiveTextEditorEmitter.fire(undefined);
    }
    _fireVisibleTextEditorsChanged('close', editor);
    return true;
}

function _tabEditor(tab) {
    return tab && tab._editor;
}

function _tabGroupEditors(group) {
    if (!group || !Array.isArray(group.tabs)) return [];
    return group.tabs.map(_tabEditor).filter(Boolean);
}

async function _tabGroupsClose(tabOrGroup, preserveFocus) {
    const items = Array.isArray(tabOrGroup) ? tabOrGroup : [tabOrGroup];
    if (!items.length) return true;
    let ok = true;
    for (const item of items) {
        const editors = item && Array.isArray(item.tabs)
            ? _tabGroupEditors(item)
            : [_tabEditor(item)].filter(Boolean);
        if (!editors.length) {
            ok = false;
            continue;
        }
        for (const editor of editors) {
            ok = _workspaceCloseTextDocument(editor.document) !== undefined && ok;
        }
    }
    if (preserveFocus && !_activeTextEditor) {
        const first = _visibleTextEditors.values().next();
        if (!first.done) {
            _activeTextEditor = first.value;
            _onDidChangeActiveTextEditorEmitter.fire(_activeTextEditor);
            _fireEditorTabEvents('change', _activeTextEditor);
        }
    }
    return ok;
}

function _tabGroupsApiObject() {
    return {
        onDidChangeTabGroups: _onDidChangeTabGroupsEmitter.event,
        onDidChangeTabs: _onDidChangeTabsEmitter.event,
        get all() { return Object.freeze(_editorTabGroupsSnapshot()); },
        get activeTabGroup() {
            return _editorTabGroupsSnapshot().find(group => group.isActive)
                || _editorTabGroupsSnapshot()[0];
        },
        close: _tabGroupsClose,
    };
}

function _createTextEditor(document, options) {
    const showOptions = _showTextDocumentOptions(options);
    const initialSelection = showOptions.selection
        ? new Selection(showOptions.selection.start, showOptions.selection.end)
        : new Selection(new Position(0, 0), new Position(0, 0));
    const editor = {
        document,
        viewColumn: showOptions.viewColumn,
        _options: {
            tabSize: 4,
            insertSpaces: true,
        },
        _selections: [initialSelection],
        _visibleRanges: [],
        _diffInformation: {},
        _decorations: new Map(),
        get options() { return this._options; },
        set options(value) { _setTextEditorOptions(this, value); },
        get selections() { return this._selections; },
        set selections(value) { _setTextEditorSelections(this, value); },
        get selection() { return this.selections[0]; },
        set selection(value) {
            _setTextEditorSelections(this, [value]);
        },
        get visibleRanges() { return this._visibleRanges; },
        get diffInformation() { return this._diffInformation; },
        _setDiffInformation(value) { _setTextEditorDiffInformation(this, value); },
        edit(callback) {
            if (typeof callback !== 'function') return Promise.resolve(false);
            const edit = {
                _edits: [],
                replace(uri, range, text) { this._edits.push({ uri, range, text }); },
                insert(uri, pos, text) {
                    this._edits.push({ uri, range: new Range(pos, pos), text });
                },
                delete(uri, range) { this._edits.push({ uri, range, text: '' }); },
            };
            const builder = {
                replace: (range, text) => edit.replace(document.uri, range, text),
                insert: (position, text) => edit.insert(document.uri, position, text),
                delete: (range) => edit.delete(document.uri, range),
            };
            callback(builder);
            return _workspaceApplyEdit(edit);
        },
        insertSnippet(snippet, location) {
            const text = snippet && snippet.value !== undefined
                ? String(snippet.value)
                : String(snippet ?? '');
            const target = location instanceof Range
                ? location
                : new Range(location || this.selection.start, location || this.selection.start);
            const edit = { _edits: [{ uri: document.uri, range: target, text }] };
            return _workspaceApplyEdit(edit);
        },
        revealRange(range) {
            const normalized = range instanceof Range ? range : _rangeFromPayload(range);
            _setTextEditorVisibleRanges(this, [normalized]);
        },
        setDecorations(decorationType, rangesOrOptions) {
            const key = _decorationTypeKey(decorationType);
            if (!key || !_textEditorDecorationTypes.has(key)) return;
            const ranges = _normalizeDecorationRanges(rangesOrOptions);
            this._decorations.set(key, ranges);
            send({
                type: 'text_editor_decorations_changed',
                key,
                uri: document.uri.toString(),
                rangeCount: ranges.length,
                ranges: ranges.map(item => _plainBridgeValue(item)),
            });
        },
        show() { return _showTextDocumentEditor(document, showOptions); },
        hide() {
            _closeTextEditor(this);
        },
    };
    return editor;
}

function _showTextDocumentEditor(document, options) {
    if (!document || !document.uri) return Promise.resolve(undefined);
    const key = document.uri.toString();
    const showOptions = _showTextDocumentOptions(options);
    let editor = _visibleTextEditors.get(key);
    if (!editor || editor.document !== document) {
        editor = _createTextEditor(document, showOptions);
        _visibleTextEditors.set(key, editor);
        _fireVisibleTextEditorsChanged('open', editor);
    } else {
        const columnChanged = _setTextEditorViewColumn(editor, showOptions.viewColumn);
        if (showOptions.selection) {
            _setTextEditorSelections(editor, [showOptions.selection]);
        }
        if (!columnChanged && showOptions.selection) {
            _fireEditorTabEvents('change', editor);
        }
    }
    if (!showOptions.preserveFocus && _activeTextEditor !== editor) {
        _activeTextEditor = editor;
        _onDidChangeActiveTextEditorEmitter.fire(editor);
        _fireEditorTabEvents('change', editor);
    }
    return Promise.resolve(editor);
}

async function _windowShowTextDocument(documentOrUri, columnOrOptions, preserveFocus) {
    const document = documentOrUri && documentOrUri.uri
        ? documentOrUri
        : await _workspaceOpenTextDocument(documentOrUri);
    const options = typeof columnOrOptions === 'object'
        ? columnOrOptions
        : {
            viewColumn: columnOrOptions,
            preserveFocus: preserveFocus === true,
        };
    return _showTextDocumentEditor(document, options);
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

function _serializeWebviewPanelIconPath(iconPath) {
    if (!iconPath) return null;
    if (iconPath instanceof Uri) {
        const uri = _serializeTreeUri(iconPath);
        return { light: uri, dark: uri };
    }
    if (typeof iconPath === 'string') return { path: iconPath };
    if (typeof iconPath === 'object') {
        if (typeof iconPath.id === 'string') {
            const result = { id: iconPath.id, kind: 'theme' };
            const color = _serializeThemeColor(iconPath.color);
            if (color) result.color = { id: color };
            return result;
        }
        const result = {};
        if (iconPath.light) result.light = _serializeTreeUri(iconPath.light);
        if (iconPath.dark) result.dark = _serializeTreeUri(iconPath.dark);
        if (iconPath.path) result.path = _serializeTreeUri(iconPath.path);
        return Object.keys(result).length ? result : null;
    }
    return { path: String(iconPath) };
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

function _treeItemUndefinedProperties(item) {
    if (!item || typeof item !== 'object') return new Set();
    const keys = [
        'label', 'description', 'tooltip', 'resourceUri', 'iconPath',
        'command', 'contextValue', 'collapsibleState', 'checkboxState',
        'accessibilityInformation',
    ];
    return new Set(keys.filter(key => item[key] === undefined));
}

function _mergeResolvedTreeItem(base, resolved, allowed) {
    if (!base || typeof base !== 'object') return resolved || base;
    if (!resolved || typeof resolved !== 'object') return base;
    for (const key of allowed) {
        if (resolved[key] !== undefined) base[key] = resolved[key];
    }
    return base;
}

async function _resolveTreeItemForProvider(provider, viewId, element) {
    let item = typeof provider.getTreeItem === 'function'
        ? await provider.getTreeItem(element)
        : element;
    if (!provider || typeof provider.resolveTreeItem !== 'function') {
        return item;
    }
    if (!item || typeof item !== 'object') {
        return item;
    }
    const allowed = _treeItemUndefinedProperties(item);
    if (!allowed.size) return item;
    const token = {
        isCancellationRequested: false,
        onCancellationRequested: new EventEmitter().event,
    };
    const resolved = await provider.resolveTreeItem(item, element, token);
    return _mergeResolvedTreeItem(item, resolved, allowed);
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

function _lmChatSelectorMatches(model, selector) {
    const sel = selector && typeof selector === 'object' ? selector : {};
    if (sel.vendor && String(sel.vendor) !== String(model.vendor || '')) return false;
    if (sel.id && String(sel.id) !== String(model.id || '')) return false;
    if (sel.family && !String(model.family || '').includes(String(sel.family))) return false;
    if (sel.version && String(sel.version) !== String(model.version || '')) return false;
    return true;
}

function _languageModelResponseFromParts(parts) {
    const chunks = (Array.isArray(parts) ? parts : [])
        .map(_languageModelPartFromPayload);
    const stream = async function* () {
        for (const chunk of chunks) yield chunk;
    };
    const text = async function* () {
        for (const chunk of chunks) {
            const partText = _languageModelResponsePartText(chunk);
            if (partText) yield partText;
        }
    };
    return {
        text: text(),
        value: chunks.map(_languageModelResponsePartText).join(''),
        stream: stream(),
    };
}

function _languageModelChatFromProvider(entry, modelInfo) {
    const meta = modelInfo && typeof modelInfo === 'object' ? modelInfo : {};
    const id = String(meta.id || meta.identifier || `${entry.vendor}.model`);
    const model = Object.assign({}, meta, {
        id,
        vendor: String(meta.vendor || entry.vendor),
        family: String(meta.family || meta.vendor || entry.vendor),
        version: String(meta.version || '1'),
        maxInputTokens: Number(meta.maxInputTokens ?? meta.max_input_tokens ?? 0) || undefined,
        capabilities: _normalizeLmCapabilities(meta.capabilities),
    });
    const apiObject = {
        id: model.id,
        name: String(model.name || model.id),
        vendor: model.vendor,
        family: model.family,
        version: model.version,
        maxInputTokens: model.maxInputTokens,
        capabilities: model.capabilities,
        countTokens(text, token) {
            if (token?.isCancellationRequested) {
                return Promise.reject(new Error('Language model token count cancelled'));
            }
            if (typeof entry.provider.provideTokenCount !== 'function') {
                return Promise.resolve(String(text ?? '').length);
            }
            const raw = entry.provider.provideTokenCount(
                model,
                text,
                token || { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event },
            );
            return raw && typeof raw.then === 'function' ? raw : Promise.resolve(raw);
        },
        async sendRequest(messages, options, token) {
            if (token?.isCancellationRequested) {
                throw new Error('Language model request cancelled');
            }
            if (typeof entry.provider.provideLanguageModelChatResponse !== 'function') {
                throw new Error(`Language model provider "${entry.vendor}" has no response handler`);
            }
            const parts = [];
            const progress = {
                report(part) {
                    if (part !== undefined && part !== null) parts.push(part);
                },
            };
            const activeToken = token || {
                isCancellationRequested: false,
                onCancellationRequested: new EventEmitter().event,
            };
            const raw = entry.provider.provideLanguageModelChatResponse(
                model,
                messages || [],
                options || {},
                progress,
                activeToken,
            );
            if (raw && typeof raw.then === 'function') await raw;
            if (activeToken?.isCancellationRequested) {
                throw new Error('Language model request cancelled');
            }
            return _languageModelResponseFromParts(parts);
        },
    };
    return Object.freeze(apiObject);
}

async function _nodeLmChatModelsForSelector(selector) {
    const sel = selector && typeof selector === 'object' ? selector : {};
    if (sel.vendor) {
        await _activateKnownExtensionsForEvent(
            `onLanguageModelChatProvider:${String(sel.vendor)}`);
    }
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    const models = [];
    for (const entry of _lmChatProviders.values()) {
        if (sel.vendor && String(sel.vendor) !== entry.vendor) continue;
        if (typeof entry.provider.provideLanguageModelChatInformation !== 'function') continue;
        const raw = entry.provider.provideLanguageModelChatInformation(
            { silent: true },
            token,
        );
        const infos = raw && typeof raw.then === 'function' ? await raw : raw;
        for (const info of Array.isArray(infos) ? infos : []) {
            const model = _languageModelChatFromProvider(entry, info);
            if (_lmChatSelectorMatches(model, sel)) models.push(model);
        }
    }
    return models;
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

function _urisFromDialogValue(value) {
    const values = Array.isArray(value) ? value : [value];
    return values
        .filter(item => item !== undefined && item !== null)
        .map(item => {
            if (item instanceof Uri) return item;
            if (item && typeof item === 'object' && (item.scheme || item.uri || item.fsPath || item.path)) {
                return _workspaceUriFromInput(item);
            }
            return Uri.file(String(item));
        });
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
        const paths = value.uris !== undefined ? value.uris : value.uri !== undefined ? value.uri : value.paths !== undefined ? value.paths : value.path;
        const uris = _urisFromDialogValue(paths);
        pending.resolve(uris.length ? uris : undefined);
    } else {
        const paths = _urisFromDialogValue(value.uri || value.uris || value.path || value.paths);
        pending.resolve(paths[0]);
    }
}

function _handleWindowMessageResponse(msg) {
    const requestId = String(msg.requestId || '');
    const pending = _windowMessageRequests.get(requestId);
    if (!pending) return;
    pending.cleanup?.();
    if (!msg.ok) {
        log(`window message failed: ${msg.error || 'unknown error'}`);
        pending.resolve(undefined);
        return;
    }
    const value = msg.value && typeof msg.value === 'object' ? msg.value : {};
    if (value.cancelled) {
        pending.resolve(undefined);
        return;
    }
    const handle = Number.isInteger(Number(value.handle)) ? Number(value.handle) : null;
    const index = Number.isInteger(Number(value.index)) ? Number(value.index) : null;
    const title = value.title !== undefined ? String(value.title) : '';
    let selected = null;
    if (handle !== null) {
        const normalizedIndex = pending.items.findIndex(item => Number(item && item.handle) === handle);
        if (normalizedIndex >= 0 && normalizedIndex < pending.rawItems.length) {
            selected = pending.rawItems[normalizedIndex];
        }
    }
    if (!selected && index !== null && index >= 0 && index < pending.rawItems.length) {
        selected = pending.rawItems[index];
    }
    if (!selected && title) {
        selected = pending.rawItems.find(item => {
            if (typeof item === 'string') return item === title;
            return item && String(item.title || '') === title;
        }) || null;
    }
    pending.resolve(selected || undefined);
}

function _fileDecorationUriFromChangeItem(value) {
    if (value === undefined || value === null) return null;
    try {
        if (value instanceof Uri) return value;
        if (value && typeof value === 'object' && (value.scheme || value.uri || value.fsPath || value.path)) {
            return _workspaceUriFromInput(value);
        }
        if (typeof value === 'string' && value) return _workspaceUriFromInput(value);
    } catch {
        return null;
    }
    return null;
}

function _fileDecorationChangedPayload(value) {
    if (value === undefined || value === null) {
        return { all: true, uris: [], count: 0, capped: false };
    }
    const rawItems = Array.isArray(value) ? value : [value];
    const uriItems = rawItems
        .map(item => _fileDecorationUriFromChangeItem(item))
        .filter(Boolean);
    if (uriItems.length <= _fileDecorationChangeMaxEventSize) {
        return {
            all: false,
            uris: uriItems.map(uri => _serializeLanguageUri(uri)),
            count: uriItems.length,
            capped: false,
        };
    }
    const sorted = uriItems
        .map(uri => ({
            uri,
            rank: String(uri.path || '').split('/').filter(Boolean).length,
        }))
        .sort((a, b) => a.rank - b.rank || String(a.uri.path || '').localeCompare(String(b.uri.path || '')));
    const picked = [];
    let lastDir = null;
    for (const item of sorted) {
        const dir = path.posix.dirname(String(item.uri.path || '').replace(/\\/g, '/'));
        if (dir === lastDir) continue;
        lastDir = dir;
        picked.push(item.uri);
        if (picked.length >= _fileDecorationChangeMaxEventSize) break;
    }
    return {
        all: false,
        uris: picked.map(uri => _serializeLanguageUri(uri)),
        count: uriItems.length,
        capped: true,
    };
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

function _requestWindowMessage(level, message, options, rawItems, normalizedItems) {
    const requestId = `wnmsg-${_nextWindowMessageRequestHandle++}`;
    return new Promise(resolve => {
        let timer;
        const cleanup = () => {
            _windowMessageRequests.delete(requestId);
            if (timer) clearTimeout(timer);
        };
        const finishUndefined = () => {
            const pending = _windowMessageRequests.get(requestId);
            if (!pending) return;
            cleanup();
            resolve(undefined);
        };
        timer = setTimeout(finishUndefined, 30000);
        _windowMessageRequests.set(requestId, {
            resolve,
            timer,
            cleanup,
            rawItems: rawItems.filter(_messageArgIsItem),
            items: normalizedItems,
        });
        send({
            type: 'window_message_request',
            requestId,
            level,
            message: String(message),
            options: {
                modal: !!options.modal,
                detail: options.detail ? String(options.detail) : '',
            },
            items: normalizedItems,
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

function _telemetryDataObject(data) {
    return data && typeof data === 'object' && !Array.isArray(data)
        ? data
        : {};
}

function _telemetryErrorObject(value) {
    if (value instanceof Error) return value;
    const message = value === undefined || value === null ? 'error' : String(value);
    return new Error(message);
}

function _createTelemetryLogger(sender) {
    let disposed = false;
    const safeSender = sender && typeof sender === 'object' ? sender : {};
    // onDidChangeEnableStates/isUsageEnabled/isErrorsEnabled were entirely
    // missing - real gap (this IS a documented TelemetryLogger member, not
    // a misunderstanding), confirmed shared root cause of
    // "this.telemetryLogger.onDidChangeEnableStates is not a function"
    // across ms-dotnettools.csharp, ms-dotnettools.vscode-dotnet-runtime,
    // and ms-vscode.powershell (2026-07-02 sweep) - all three subscribe to
    // it right after creating their telemetry logger during activation.
    // Telemetry is never actually enabled in this host, so both flags are
    // false and the event correctly never needs to fire (no real state
    // change source exists) - honest static values, not overclaiming.
    const changeEmitter = new EventEmitter();
    return {
        isUsageEnabled: false,
        isErrorsEnabled: false,
        onDidChangeEnableStates: changeEmitter.event,
        logUsage(eventName, data) {
            if (disposed || typeof safeSender.sendEventData !== 'function') return;
            try {
                safeSender.sendEventData(String(eventName || ''), _telemetryDataObject(data));
            } catch (err) {
                log(`telemetry logUsage failed: ${err?.message || err}`);
            }
        },
        logError(errorOrEventName, data) {
            if (disposed || typeof safeSender.sendErrorData !== 'function') return;
            try {
                safeSender.sendErrorData(
                    _telemetryErrorObject(errorOrEventName),
                    _telemetryDataObject(data),
                );
            } catch (err) {
                log(`telemetry logError failed: ${err?.message || err}`);
            }
        },
        dispose() {
            disposed = true;
        },
    };
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
    if (value instanceof CodeActionKind) return { value: value.value };
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

function _mcpServerDefinitionKind(definition) {
    if (!definition || typeof definition !== 'object') return '';
    if (definition instanceof McpHttpServerDefinition) return 'http';
    if (definition instanceof McpStdioServerDefinition) return 'stdio';
    if (definition.uri !== undefined) return 'http';
    if (definition.command !== undefined) return 'stdio';
    return '';
}

function _serializeMcpServerDefinition(definition) {
    if (!definition || typeof definition !== 'object') return null;
    const kind = _mcpServerDefinitionKind(definition);
    const result = {
        type: kind || 'stdio',
        label: definition.label === undefined || definition.label === null
            ? ''
            : String(definition.label),
    };
    if (definition.version !== undefined) result.version = String(definition.version);
    if (kind === 'http') {
        result.uri = _serializeLanguageUri(definition.uri || '');
        result.headers = definition.headers && typeof definition.headers === 'object'
            ? _serializeLanguageValue(definition.headers)
            : {};
    } else {
        result.command = definition.command === undefined || definition.command === null
            ? ''
            : String(definition.command);
        result.args = Array.isArray(definition.args)
            ? definition.args.map(item => String(item))
            : [];
        result.env = definition.env && typeof definition.env === 'object'
            ? _serializeLanguageValue(definition.env)
            : {};
        if (definition.cwd !== undefined && definition.cwd !== null) {
            result.cwd = _serializeLanguageUri(definition.cwd);
        }
    }
    return result;
}

function _mcpServerDefinitionFromPayload(payload) {
    const data = payload && typeof payload === 'object' ? payload : {};
    const kind = String(data.type || (data.uri ? 'http' : 'stdio')).toLowerCase();
    if (kind === 'http') {
        return new McpHttpServerDefinition(
            data.label || '',
            _uriFromPayload(data.uri || ''),
            data.headers && typeof data.headers === 'object' ? data.headers : {},
            data.version,
        );
    }
    const definition = new McpStdioServerDefinition(
        data.label || '',
        data.command || '',
        Array.isArray(data.args) ? data.args : [],
        data.env && typeof data.env === 'object' ? data.env : {},
        data.version,
    );
    if (data.cwd !== undefined && data.cwd !== null && data.cwd !== '') {
        definition.cwd = _uriFromPayload(data.cwd);
    }
    return definition;
}

function _diagnosticsForUri(uri) {
    const key = uri && typeof uri.toString === 'function' ? uri.toString() : String(uri || '');
    if (!key) return [];
    const values = [];
    for (const collection of _diagnosticCollections.values()) {
        const diagnostics = collection && typeof collection.get === 'function'
            ? collection.get(key)
            : null;
        if (Array.isArray(diagnostics)) values.push(...diagnostics);
        else if (diagnostics) values.push(diagnostics);
    }
    return values;
}

function _codeActionDiagnostics(document, msg) {
    const incoming = Array.isArray(msg.diagnostics) ? msg.diagnostics : [];
    const local = _diagnosticsForUri(document && document.uri);
    if (!incoming.length) return local;
    if (!local.length) return incoming;
    return [...incoming, ...local];
}

function _codeActionMatchesKind(action, only) {
    const onlyValue = _codeActionKindFromPayload(only).value;
    if (!onlyValue) return true;
    const actionValue = _codeActionKindFromPayload(action?.kind).value;
    if (!actionValue) return false;
    return _codeActionKindFromPayload(only).contains(actionValue);
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
    const responseParts = [];
    const contentReferences = [];
    const fileTrees = [];
    const textEdits = [];
    const append = (value) => {
        const text = _chatPartToText(value);
        parts.push(text);
        return text;
    };
    const pushResponsePart = (part) => {
        const normalized = _serializeLanguageValue(part);
        if (normalized && typeof normalized === 'object') {
            if (!normalized.kind && normalized.type) normalized.kind = String(normalized.type);
            responseParts.push(normalized);
            return normalized;
        }
        const fallback = { kind: 'text', value: normalized === undefined || normalized === null ? '' : String(normalized) };
        responseParts.push(fallback);
        return fallback;
    };
    const recordReference = (value, iconPath, options) => {
        const part = {
            kind: 'reference',
            value: _serializeLanguageValue(value),
            uri: _serializeLanguageUri(value),
        };
        if (iconPath !== undefined && iconPath !== null) part.iconPath = _serializeLanguageValue(iconPath);
        if (options !== undefined && options !== null) {
            part.options = _serializeLanguageValue(options);
            if (options && typeof options === 'object' && options.status !== undefined) {
                part.status = _serializeLanguageValue(options.status);
            }
        }
        const normalized = pushResponsePart(part);
        contentReferences.push(normalized);
        return normalized;
    };
    const recordFileTree = (value, baseUri) => {
        const part = {
            kind: 'fileTree',
            value: _serializeLanguageValue(value),
        };
        if (baseUri !== undefined && baseUri !== null) part.baseUri = _serializeLanguageValue(baseUri);
        const normalized = pushResponsePart(part);
        fileTrees.push(normalized);
        return normalized;
    };
    const recordTextEdit = (target, edits) => {
        const part = {
            kind: 'textEdit',
            target: _serializeLanguageValue(target),
            uri: _serializeLanguageUri(target),
            edits: _serializeLanguageValue(edits || []),
        };
        const normalized = pushResponsePart(part);
        textEdits.push(normalized);
        return normalized;
    };
    const push = (part) => {
        if (part && typeof part === 'object' && !(part instanceof Uri)) {
            const kind = String(part.kind || part.type || part.constructor?.name || '');
            if (kind) {
                const normalized = pushResponsePart(Object.assign({ kind }, _serializeLanguageValue(part)));
                const lower = kind.toLowerCase();
                if (lower.includes('reference')) contentReferences.push(normalized);
                else if (lower.includes('filetree')) fileTrees.push(normalized);
                else if (lower.includes('textedit')) textEdits.push(normalized);
                const text = part.text ?? part.markdown ?? part.content ?? part.value;
                if (text !== undefined && text !== null && lower.includes('markdown')) append(text);
                return normalized;
            }
        }
        append(part);
        return part;
    };
    const stream = {
        markdown: append,
        text: append,
        progress() {},
        warning: append,
        info: append,
        anchor(value, title) {
            append(title || value);
            return pushResponsePart({
                kind: 'anchor',
                value: _serializeLanguageValue(value),
                uri: _serializeLanguageUri(value),
                title: title === undefined || title === null ? '' : String(title),
            });
        },
        button() {},
        reference: recordReference,
        reference2: recordReference,
        filetree: recordFileTree,
        codeblockUri(value) { append(value); },
        codeCitation(value) { append(value); },
        textEdit: recordTextEdit,
        confirmation() {},
        notebookEdit() {},
        workspaceEdit() {},
        thinkingProgress: append,
        beginToolInvocation() {},
        updateToolInvocation() {},
        push,
    };
    Object.defineProperties(stream, {
        _responseParts: { value: responseParts },
        _contentReferences: { value: contentReferences },
        _fileTrees: { value: fileTrees },
        _textEdits: { value: textEdits },
    });
    return stream;
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

function _codeActionKindFromPayload(value) {
    if (value instanceof CodeActionKind) return value;
    if (value && typeof value === 'object' && value.value !== undefined) {
        return new CodeActionKind(value.value);
    }
    return new CodeActionKind(value === undefined || value === null ? '' : String(value));
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

function _knownLanguageIds() {
    const languages = new Set([
        'plaintext', 'python', 'javascript', 'typescript', 'json', 'html',
        'css', 'markdown', 'yaml', 'xml', 'sql', 'shell', 'lua', 'c', 'cpp',
        'csharp', 'java', 'go', 'rust', 'toml',
    ]);
    for (const entry of _knownExtensions.values()) {
        const contributed = entry?.manifest?.contributes?.languages;
        for (const language of Array.isArray(contributed) ? contributed : []) {
            const id = String(language?.id || '').trim();
            if (id) languages.add(id);
        }
    }
    for (const id of _runtimeLanguageConfigurations.keys()) {
        if (id) languages.add(id);
    }
    return Array.from(languages).sort((a, b) => a.localeCompare(b));
}

function _setLanguageConfiguration(language, configuration) {
    const languageId = String(language || '').trim();
    if (!languageId) return new Disposable();
    const entry = {
        handle: _nextLanguageConfigurationHandle++,
        configuration: _serializeLanguageValue(configuration || {}),
    };
    const list = _runtimeLanguageConfigurations.get(languageId) || [];
    list.push(entry);
    _runtimeLanguageConfigurations.set(languageId, list);
    return new Disposable(() => {
        const current = _runtimeLanguageConfigurations.get(languageId);
        if (!current) return;
        const filtered = current.filter(item => item.handle !== entry.handle);
        if (filtered.length) {
            _runtimeLanguageConfigurations.set(languageId, filtered);
        } else {
            _runtimeLanguageConfigurations.delete(languageId);
        }
    });
}

function _setTextDocumentLanguage(document, languageId) {
    const target = document && typeof document === 'object' ? document : undefined;
    const language = String(languageId || 'plaintext');
    if (!target) return Promise.resolve(target);
    _onDidCloseTextDocumentEmitter.fire(target);
    target.languageId = language;
    if (target.uri) {
        const cached = _workspaceTextDocuments.get(target.uri.toString());
        if (cached) cached.languageId = language;
    }
    _onDidOpenTextDocumentEmitter.fire(target);
    return Promise.resolve(target);
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
        if (value.fsPath && (!value.scheme || value.scheme === 'file')) {
            return Uri.file(String(value.fsPath));
        }
        if (value.scheme) return _uriFromPayload(value);
        if (value.uri || value.path || value.fsPath) {
            return _workspaceUriFromInput(value.uri || value.fsPath || value.path);
        }
    }
    return new Uri('untitled', '', '/Untitled-1', '', '');
}

function _workspaceFolderName(uri, explicitName) {
    if (explicitName !== undefined && explicitName !== null && String(explicitName).trim()) {
        return String(explicitName);
    }
    return path.basename(uri.fsPath || uri.path || '') || uri.toString();
}

function _normalizeWorkspaceFolder(value, index) {
    const raw = value && typeof value === 'object' && value.uri ? value.uri : value;
    const uri = _workspaceUriFromInput(raw);
    return {
        uri,
        name: _workspaceFolderName(uri, value && value.name),
        index,
    };
}

function _workspaceFolderKey(folder) {
    const uri = folder && folder.uri instanceof Uri ? folder.uri : _workspaceUriFromInput(folder);
    const text = uri.toString();
    return process.platform === 'win32' ? text.toLowerCase() : text;
}

function _workspaceFoldersSnapshot() {
    return _workspaceFolders.map((folder, index) => ({
        uri: folder.uri,
        name: folder.name,
        index,
    }));
}

function _workspaceFolder() {
    return _workspaceFoldersSnapshot()[0];
}

function _workspaceFolderContainsUri(folder, uri) {
    if (!folder || !uri || folder.uri.scheme !== uri.scheme) return false;
    if (folder.uri.authority !== uri.authority) return false;
    if (uri.scheme === 'file') {
        const folderPath = path.resolve(folder.uri.fsPath);
        const targetPath = path.resolve(uri.fsPath);
        const rel = path.relative(folderPath, targetPath);
        return !rel || (!rel.startsWith('..') && !path.isAbsolute(rel));
    }
    const folderPrefix = folder.uri.toString().replace(/[\\/]?$/, '/');
    const target = uri.toString();
    return target === folder.uri.toString() || target.startsWith(folderPrefix);
}

function _workspaceGetWorkspaceFolder(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    let best;
    let bestLength = -1;
    for (const folder of _workspaceFoldersSnapshot()) {
        if (!_workspaceFolderContainsUri(folder, uri)) continue;
        const length = folder.uri.scheme === 'file'
            ? path.resolve(folder.uri.fsPath).length
            : folder.uri.toString().length;
        if (length > bestLength) {
            best = folder;
            bestLength = length;
        }
    }
    return best;
}

function _workspaceUpdateWorkspaceFolders(start, deleteCount, ...workspaceFoldersToAdd) {
    const current = _workspaceFoldersSnapshot();
    const startIndex = Number(start);
    if (!Number.isInteger(startIndex) || startIndex < 0 || startIndex > current.length) {
        return false;
    }
    const rawDeleteCount = deleteCount === undefined || deleteCount === null
        ? 0
        : Number(deleteCount);
    if (!Number.isInteger(rawDeleteCount) || rawDeleteCount < 0) {
        return false;
    }
    const deleteTotal = Math.min(rawDeleteCount, current.length - startIndex);
    const additions = [];
    for (let i = 0; i < workspaceFoldersToAdd.length; i += 1) {
        const item = workspaceFoldersToAdd[i];
        if (!item || typeof item !== 'object' || !item.uri) return false;
        additions.push(_normalizeWorkspaceFolder(item, startIndex + i));
    }
    const next = current.slice();
    next.splice(startIndex, deleteTotal, ...additions);
    const keys = new Set();
    for (const folder of next) {
        const key = _workspaceFolderKey(folder);
        if (keys.has(key)) return false;
        keys.add(key);
    }
    const removed = current.slice(startIndex, startIndex + deleteTotal);
    _workspaceFolders = next.map((folder, index) => ({
        uri: folder.uri,
        name: folder.name,
        index,
    }));
    const added = _workspaceFolders.slice(
        startIndex, startIndex + additions.length).map((folder, index) => ({
            uri: folder.uri,
            name: folder.name,
            index: startIndex + index,
        }));
    _onDidChangeWorkspaceFoldersEmitter.fire({ added, removed });
    return true;
}

function _workspaceRelativePath(value, includeWorkspaceFolder) {
    const rawPath = _pathFromUriLike(value);
    const absPath = path.resolve(rawPath || _workspaceRoot);
    const folder = _workspaceGetWorkspaceFolder(Uri.file(absPath));
    const basePath = folder ? folder.uri.fsPath : _workspaceRoot;
    let rel = path.relative(basePath, absPath).replace(/\\/g, '/');
    if (!rel || rel.startsWith('..')) rel = absPath.replace(/\\/g, '/');
    const includeFolder = includeWorkspaceFolder === undefined
        ? _workspaceFolders.length > 1
        : !!includeWorkspaceFolder;
    return includeFolder && folder ? `${folder.name}/${rel}` : rel;
}

function _terminalOptionsFromArgs(nameOrOptions, shellPath, shellArgs) {
    if (nameOrOptions && typeof nameOrOptions === 'object') {
        return _terminalNormalizeOptions(nameOrOptions);
    }
    const options = {};
    if (nameOrOptions !== undefined && nameOrOptions !== null) {
        options.name = String(nameOrOptions);
    }
    if (shellPath !== undefined && shellPath !== null) {
        options.shellPath = String(shellPath);
    }
    if (shellArgs !== undefined && shellArgs !== null) {
        options.shellArgs = shellArgs;
    }
    return _terminalNormalizeOptions(options);
}

function _terminalNormalizeShellArgs(value) {
    if (value === undefined || value === null) return undefined;
    return Array.isArray(value) ? value.map(item => String(item)) : String(value);
}

function _terminalNormalizeEnv(env) {
    if (!env || typeof env !== 'object') return undefined;
    const result = {};
    for (const [key, value] of Object.entries(env)) {
        result[String(key)] = value === null || value === undefined ? null : String(value);
    }
    return result;
}

function _terminalEnvironmentKey(env, variable) {
    const name = String(variable);
    if (process.platform !== 'win32') return name;
    const lower = name.toLowerCase();
    return Object.keys(env).find(key => String(key).toLowerCase() === lower) || name;
}

function _terminalCwdPath(options) {
    const cwd = options && options.cwd;
    if (!cwd) return _workspaceRoot;
    if (cwd instanceof Uri) return cwd.fsPath || _workspaceRoot;
    return path.resolve(String(cwd || _workspaceRoot));
}

function _environmentMutatorMatchesTerminalScope(mutator, options) {
    const folder = mutator && mutator.scope && mutator.scope.workspaceFolder;
    if (!folder || !folder.uri) return true;
    const folderPath = path.resolve(folder.uri.fsPath || folder.uri.path || '');
    if (!folderPath) return false;
    const rel = path.relative(folderPath, _terminalCwdPath(options));
    return !rel || (!rel.startsWith('..') && !path.isAbsolute(rel));
}

function _terminalApplyEnvironmentCollections(env, options) {
    if (options && options.strictEnv === true) return env;
    for (const shared of _environmentVariableCollections.values()) {
        for (const mutator of shared.map.values()) {
            if (!mutator || mutator.options?.applyAtProcessCreation === false) {
                continue;
            }
            if (!_environmentMutatorMatchesTerminalScope(mutator, options)) {
                continue;
            }
            const key = _terminalEnvironmentKey(env, mutator.variable);
            const current = env[key] === undefined || env[key] === null
                ? ''
                : String(env[key]);
            const value = String(mutator.value ?? '');
            if (mutator.type === EnvironmentVariableMutatorType.Replace) {
                env[key] = value;
            } else if (mutator.type === EnvironmentVariableMutatorType.Append) {
                env[key] = current + value;
            } else if (mutator.type === EnvironmentVariableMutatorType.Prepend) {
                env[key] = value + current;
            }
        }
    }
    return env;
}

function _terminalResolveEnv(options) {
    const strict = options && options.strictEnv === true;
    const result = strict ? {} : Object.assign({}, process.env);
    const env = _terminalNormalizeEnv(options && options.env) || {};
    for (const [key, value] of Object.entries(env)) {
        if (value === null || value === undefined) delete result[key];
        else result[key] = String(value);
    }
    if (options && typeof options.shellIntegrationNonce === 'string'
            && options.shellIntegrationNonce) {
        result.VSCODE_NONCE = options.shellIntegrationNonce;
    }
    return _terminalApplyEnvironmentCollections(result, options);
}

function _terminalNormalizeOptions(options) {
    const raw = Object.assign({}, options || {});
    if (raw.name !== undefined && raw.name !== null) raw.name = String(raw.name);
    if (raw.shellPath !== undefined && raw.shellPath !== null) raw.shellPath = String(raw.shellPath);
    if (raw.shellArgs !== undefined && raw.shellArgs !== null) {
        raw.shellArgs = _terminalNormalizeShellArgs(raw.shellArgs);
    }
    if (raw.env !== undefined && raw.env !== null) raw.env = _terminalNormalizeEnv(raw.env);
    if (raw.cwd !== undefined && raw.cwd !== null && !(raw.cwd instanceof Uri)) {
        raw.cwd = String(raw.cwd);
    }
    if (raw.shellIntegrationNonce !== undefined
            && raw.shellIntegrationNonce !== null) {
        raw.shellIntegrationNonce = String(raw.shellIntegrationNonce);
    }
    raw.strictEnv = raw.strictEnv === true;
    raw.hideFromUser = raw.hideFromUser === true;
    return raw;
}

function _terminalMergeOptions(baseOptions, overrideOptions) {
    const base = _terminalNormalizeOptions(baseOptions || {});
    const rawOverride = overrideOptions && typeof overrideOptions === 'object' ? overrideOptions : {};
    const override = _terminalNormalizeOptions(rawOverride);
    const merged = Object.assign({}, base, override);
    if (!Object.prototype.hasOwnProperty.call(rawOverride, 'strictEnv')) {
        merged.strictEnv = base.strictEnv === true;
    }
    if (!Object.prototype.hasOwnProperty.call(rawOverride, 'hideFromUser')) {
        merged.hideFromUser = base.hideFromUser === true;
    }
    if (base.env || override.env) {
        merged.env = Object.assign({}, base.env || {}, override.env || {});
    }
    return _terminalNormalizeOptions(merged);
}

function _terminalOptionValue(value) {
    if (value === undefined || value === null) return undefined;
    if (value instanceof Uri) return value.toString();
    if (value instanceof ThemeIcon) return {
        id: value.id,
        color: value.color instanceof ThemeColor
            ? { id: value.color.id }
            : _terminalOptionValue(value.color),
    };
    if (value instanceof ThemeColor) return { id: value.id };
    if (Array.isArray(value)) return value.map(_terminalOptionValue);
    if (typeof value === 'object') {
        const result = {};
        for (const [key, item] of Object.entries(value)) {
            if (typeof item !== 'function') result[key] = _terminalOptionValue(item);
        }
        return result;
    }
    return value;
}

function _plainBridgeValue(value) {
    if (value === undefined || value === null) return value;
    if (value instanceof Uri) return value.toString();
    if (value instanceof TaskGroup) {
        return {
            id: value.id,
            label: value.label,
            isDefault: value.isDefault,
        };
    }
    if (value instanceof ProcessExecution) {
        return {
            type: 'process',
            command: value.process,
            args: value.args || [],
            options: _plainBridgeValue(value.options || {}),
        };
    }
    if (value instanceof ShellExecution) {
        return {
            type: 'shell',
            command: value.command,
            commandLine: value.commandLine,
            args: value.args || [],
            options: _plainBridgeValue(value.options || {}),
        };
    }
    if (value instanceof CustomExecution) {
        return { type: 'customExecution' };
    }
    if (value instanceof ThemeIcon || value instanceof ThemeColor) {
        return _terminalOptionValue(value);
    }
    if (Array.isArray(value)) return value.map(_plainBridgeValue);
    if (typeof value === 'object') {
        const result = {};
        for (const [key, item] of Object.entries(value)) {
            if (typeof item !== 'function') result[key] = _plainBridgeValue(item);
        }
        return result;
    }
    return value;
}

function _taskType(task) {
    if (!task || typeof task !== 'object') return '';
    const definition = task.definition && typeof task.definition === 'object'
        ? task.definition
        : {};
    return String(task.type || definition.type || '');
}

function _taskMatchesFilter(task, filter) {
    if (!filter || typeof filter !== 'object') return true;
    const filterType = String(filter.type || '');
    if (filterType && _taskType(task) !== filterType) return false;
    const filterSource = filter.source === undefined || filter.source === null
        ? ''
        : String(filter.source);
    if (filterSource && String(task?.source || '') !== filterSource) return false;
    return true;
}

function _serializeTask(task) {
    if (!task || typeof task !== 'object') return { name: String(task || 'task') };
    const executionSpec = _taskExecutionSpec(task);
    return {
        name: String(task.name || task.label || _taskType(task) || 'task'),
        source: task.source === undefined ? undefined : String(task.source),
        type: _taskType(task) || undefined,
        definition: _plainBridgeValue(task.definition || {}),
        scope: _plainBridgeValue(task.scope),
        execution: _plainBridgeValue(task.execution),
        commandLine: executionSpec.commandLine || undefined,
        problemMatchers: _plainBridgeValue(task.problemMatchers || []),
        hasDefinedMatchers: task.hasDefinedMatchers === true,
        isBackground: task.isBackground === true,
        group: _plainBridgeValue(task.group),
        detail: task.detail === undefined ? undefined : String(task.detail),
        presentationOptions: _plainBridgeValue(task.presentationOptions || {}),
        runOptions: _plainBridgeValue(task.runOptions || {}),
    };
}

function _shellTokenText(value) {
    if (value && typeof value === 'object' && Object.prototype.hasOwnProperty.call(value, 'value')) {
        return String(value.value);
    }
    return String(value ?? '');
}

function _quoteCommandToken(value) {
    const text = _shellTokenText(value);
    if (!text) return '""';
    if (!/[\s"'`$&|<>]/.test(text)) return text;
    return JSON.stringify(text);
}

function _taskExecutionSpec(task) {
    if (!task || typeof task !== 'object') return {};
    const execution = task.execution;
    let commandLine = '';
    let cwd;
    let env;
    let kind = '';
    if (execution instanceof ShellExecution) {
        kind = 'shell';
        if (execution.commandLine) {
            commandLine = String(execution.commandLine);
        } else if (execution.command) {
            commandLine = [
                execution.command,
                ...(execution.args || []),
            ].map(_quoteCommandToken).join(' ');
        }
        cwd = execution.options && execution.options.cwd;
        env = execution.options && execution.options.env;
    } else if (execution instanceof ProcessExecution) {
        kind = 'process';
        commandLine = [
            execution.process,
            ...(execution.args || []),
        ].map(_quoteCommandToken).join(' ');
        cwd = execution.options && execution.options.cwd;
        env = execution.options && execution.options.env;
    } else if (execution instanceof CustomExecution) {
        kind = 'customExecution';
    } else if (execution && typeof execution === 'object') {
        kind = String(
            execution.type
            || (execution.commandLine || execution.command ? 'shell' : '')
            || (execution.process ? 'process' : ''));
        if (execution.commandLine) {
            commandLine = String(execution.commandLine);
        } else {
            const command = execution.command || execution.process;
            if (command) {
                commandLine = [
                    command,
                    ...(
                        Array.isArray(execution.args)
                            ? execution.args
                            : []
                    ),
                ].map(_quoteCommandToken).join(' ');
            }
        }
        if (!commandLine && execution.process) {
            commandLine = [
                execution.process,
                ...(
                    Array.isArray(execution.args)
                        ? execution.args
                        : []
                ),
            ].map(_quoteCommandToken).join(' ');
        }
        cwd = execution.cwd || (execution.options && execution.options.cwd);
        env = execution.env || (execution.options && execution.options.env);
    } else if (typeof task.command === 'string') {
        kind = task.shell ? 'shell' : 'process';
        commandLine = [
            task.command,
            ...(Array.isArray(task.args) ? task.args : []),
        ].map(_quoteCommandToken).join(' ');
        cwd = task.cwd || (task.options && task.options.cwd);
        env = task.env || (task.options && task.options.env);
    }
    return {
        commandLine,
        cwd: _terminalOptionValue(cwd),
        env: _terminalOptionValue(env),
        kind,
    };
}

function _taskBridgePresentationName(value) {
    const num = Number(value);
    if (num === 1) return 'always';
    if (num === 2) return 'silent';
    if (num === 3) return 'never';
    return '';
}

function _taskBridgePanelName(value) {
    const num = Number(value);
    if (num === 1) return 'shared';
    if (num === 2) return 'dedicated';
    if (num === 3) return 'new';
    return '';
}

function _taskBridgeGroupId(group) {
    if (!group) return '';
    if (group instanceof TaskGroup) return String(group.id || '');
    if (typeof group === 'string') return group;
    if (typeof group === 'object') return String(group.id || group._id || '');
    return '';
}

function _taskBridgeGroupLabel(group) {
    if (!group) return '';
    if (group instanceof TaskGroup) return String(group.label || group.id || '');
    if (typeof group === 'string') return group;
    if (typeof group === 'object') {
        return String(group.label || group.id || group._id || '');
    }
    return '';
}

function _taskBridgeMetadata(task, executionSpec, extra = {}) {
    const serialized = _serializeTask(task);
    const presentation = task && typeof task.presentationOptions === 'object'
        ? _plainBridgeValue(task.presentationOptions || {})
        : {};
    const runOptions = task && typeof task.runOptions === 'object'
        ? _plainBridgeValue(task.runOptions || {})
        : {};
    const group = task ? task.group : undefined;
    const panel = _taskBridgePanelName(presentation.panel) || 'shared';
    const reveal = _taskBridgePresentationName(presentation.reveal) || 'always';
    const terminalBase = serialized.name || _taskType(task) || 'Extension Task';
    return {
        cwd: executionSpec.cwd,
        env: executionSpec.env,
        kind: executionSpec.kind,
        taskType: serialized.type || _taskType(task) || '',
        taskName: terminalBase,
        isBackground: !!(task && task.isBackground === true),
        group: _plainBridgeValue(group),
        groupId: _taskBridgeGroupId(group),
        groupLabel: _taskBridgeGroupLabel(group),
        presentationOptions: presentation,
        runOptions,
        reveal,
        panel,
        clear: presentation.clear === true,
        close: presentation.close === true,
        focus: presentation.focus === true,
        echo: presentation.echo === true,
        showReuseMessage: presentation.showReuseMessage !== false,
        problemMatchers: serialized.problemMatchers || [],
        problemMatcherCount: Array.isArray(serialized.problemMatchers)
            ? serialized.problemMatchers.length
            : 0,
        terminalReuseKey: panel === 'shared'
            ? 'shared'
            : (panel === 'dedicated'
                ? `dedicated:${serialized.type || ''}:${terminalBase}`
                : ''),
        ...extra,
    };
}

async function _callTaskProvider(provider, methodName, ...args) {
    if (!provider || typeof provider !== 'object') return undefined;
    const method = provider[methodName];
    if (typeof method !== 'function') return undefined;
    return await Promise.resolve(method.apply(provider, args));
}

async function _resolveTask(task) {
    const provider = _taskProviders.get(_taskType(task));
    const resolved = await _callTaskProvider(provider, 'resolveTask', task);
    return resolved || task;
}

function _debugSessionPayload(session) {
    if (!session) return undefined;
    return {
        id: session.id,
        type: session.type,
        name: session.name,
        parentSession: session.parentSession
            ? { id: session.parentSession.id, name: session.parentSession.name }
            : undefined,
        configuration: _plainBridgeValue(session.configuration || {}),
        adapterDescriptor: _debugAdapterDescriptorPayload(
            session.adapterDescriptor),
    };
}

function _debugTrackerEntriesForType(type) {
    const wanted = String(type || '');
    return _debugAdapterTrackerFactories.filter(entry => {
        return entry && (entry.type === '*' || entry.type === wanted);
    });
}

function _debugCallTrackers(session, method, ...args) {
    const trackers = Array.isArray(session?._debugTrackers)
        ? session._debugTrackers
        : [];
    for (const tracker of trackers) {
        try {
            if (tracker && typeof tracker[method] === 'function') {
                tracker[method](...args);
            }
        } catch (err) {
            if (method !== 'onError' && tracker
                    && typeof tracker.onError === 'function') {
                try { tracker.onError(err); } catch {}
            }
        }
    }
}

function _debugRuntimeData(session) {
    if (!session) return {};
    if (!session._debugRuntimeData) {
        session._debugRuntimeData = {
            state: 'started',
            consoleOutputCount: 0,
            lastConsoleOutput: '',
            lastConsoleCategory: '',
            stoppedReason: '',
            threadId: undefined,
            lastEvent: '',
        };
    }
    return session._debugRuntimeData;
}

function _debugActiveStackItemPayload(item) {
    if (!item || typeof item !== 'object') return undefined;
    const payload = { ...item };
    if (payload.session && typeof payload.session === 'object') {
        payload.session = _debugSessionPayload(payload.session);
    }
    return _plainBridgeValue(payload);
}

function _debugSendSessionUpdate(session, patch = {}) {
    if (!session) return;
    const runtime = Object.assign(_debugRuntimeData(session), patch || {});
    send({
        type: 'debug_session_update',
        state: runtime.state || 'started',
        session: _debugSessionPayload(session),
        consoleOutputCount: Number(runtime.consoleOutputCount || 0),
        lastConsoleOutput: String(runtime.lastConsoleOutput || ''),
        lastConsoleCategory: String(runtime.lastConsoleCategory || ''),
        stoppedReason: String(runtime.stoppedReason || ''),
        threadId: runtime.threadId,
        lastEvent: String(runtime.lastEvent || ''),
        message: String(runtime.message || ''),
        activeStackItem: _debugActiveStackItemPayload(session.activeStackItem || runtime.activeStackItem || undefined),
    });
}

function _debugProviderToken() {
    return {
        isCancellationRequested: false,
        onCancellationRequested: () => new Disposable(),
    };
}

function _debugContributionForType(type) {
    const wanted = String(type || '');
    if (!wanted) return null;
    for (const known of _knownExtensions.values()) {
        const debuggers = known?.manifest?.contributes?.debuggers;
        for (const dbg of Array.isArray(debuggers) ? debuggers : []) {
            if (dbg && String(dbg.type || '') === wanted) {
                return { extension: known, contribution: dbg };
            }
        }
    }
    return null;
}

function _debugExecutableFromPackage(type) {
    const found = _debugContributionForType(type);
    if (!found) return undefined;
    const { extension, contribution } = found;
    const extensionPath = String(extension.extensionPath || '');
    const executable = contribution.adapterExecutableCommand
        || contribution.executable
        || contribution.command;
    if (executable) {
        return new DebugAdapterExecutable(
            String(executable),
            Array.isArray(contribution.args)
                ? contribution.args.map(item => String(item))
                : [],
            { cwd: extensionPath || undefined },
        );
    }
    const program = contribution.program || contribution.adapter || contribution.main;
    if (!program) return undefined;
    const programPath = path.isAbsolute(String(program))
        ? String(program)
        : path.join(extensionPath, String(program));
    const runtime = contribution.runtime || contribution.runtimeExecutable;
    const runtimeArgs = Array.isArray(contribution.runtimeArgs)
        ? contribution.runtimeArgs.map(item => String(item))
        : [];
    const args = Array.isArray(contribution.args)
        ? contribution.args.map(item => String(item))
        : [];
    if (runtime) {
        return new DebugAdapterExecutable(
            String(runtime),
            [...runtimeArgs, programPath, ...args],
            { cwd: extensionPath || undefined },
        );
    }
    return new DebugAdapterExecutable(
        programPath,
        args,
        { cwd: extensionPath || undefined },
    );
}

function _debugAdapterDescriptorPayload(descriptor) {
    if (!descriptor) return undefined;
    if (descriptor instanceof DebugAdapterExecutable) {
        return {
            type: 'executable',
            command: descriptor.command,
            args: descriptor.args || [],
            options: _plainBridgeValue(descriptor.options || {}),
        };
    }
    if (descriptor instanceof DebugAdapterServer) {
        return {
            type: 'server',
            port: descriptor.port,
            host: descriptor.host,
        };
    }
    if (descriptor instanceof DebugAdapterNamedPipeServer) {
        return {
            type: 'pipeServer',
            path: descriptor.path,
        };
    }
    if (descriptor instanceof DebugAdapterInlineImplementation) {
        return {
            type: 'implementation',
            hasImplementation: !!descriptor.implementation,
        };
    }
    return _plainBridgeValue(descriptor);
}

function _debugDapFrame(message) {
    const payload = JSON.stringify(message || {});
    return `Content-Length: ${Buffer.byteLength(payload, 'utf8')}\r\n\r\n${payload}`;
}

function _debugParseDapFrames(transport, chunk) {
    transport.buffer += chunk.toString('utf8');
    for (;;) {
        const headerEnd = transport.buffer.indexOf('\r\n\r\n');
        if (headerEnd < 0) return;
        const header = transport.buffer.slice(0, headerEnd);
        const match = /Content-Length:\s*(\d+)/i.exec(header);
        if (!match) {
            transport.buffer = transport.buffer.slice(headerEnd + 4);
            continue;
        }
        const length = Number(match[1]);
        const bodyStart = headerEnd + 4;
        if (transport.buffer.length < bodyStart + length) return;
        const raw = transport.buffer.slice(bodyStart, bodyStart + length);
        transport.buffer = transport.buffer.slice(bodyStart + length);
        try {
            _debugHandleDapMessage(transport, JSON.parse(raw));
        } catch (err) {
            _debugCallTrackers(transport.session, 'onError', err);
        }
    }
}

function _debugHandleDapMessage(transport, message) {
    if (!message || typeof message !== 'object') return;
    if (message.type === 'response' && message.request_seq !== undefined) {
        const pending = transport.pending.get(Number(message.request_seq));
        if (pending) {
            clearTimeout(pending.timer);
            transport.pending.delete(Number(message.request_seq));
            const body = message.body === undefined ? {} : message.body;
            if (pending.command === 'initialize' && body && typeof body === 'object') {
                transport.capabilities = body;
            }
            if (message.success === false) {
                pending.reject(new Error(message.message || 'Debug adapter request failed'));
            } else {
                pending.resolve(body);
            }
        }
    }
    if (message.type === 'event') {
        _debugCallTrackers(transport.session, 'onDidSendMessage', message);
        const eventName = String(message.event || '');
        const body = message.body && typeof message.body === 'object'
            ? message.body
            : {};
        if (eventName === 'output') {
            const text = String(body.output ?? '');
            const runtime = _debugRuntimeData(transport.session);
            runtime.consoleOutputCount = Number(runtime.consoleOutputCount || 0) + (text ? 1 : 0);
            runtime.lastConsoleOutput = text.trim();
            runtime.lastConsoleCategory = String(body.category || 'console');
            runtime.lastEvent = 'output';
            send({
                type: 'debug_console',
                session: _debugSessionPayload(transport.session),
                text,
                newline: false,
                category: runtime.lastConsoleCategory,
                source: _plainBridgeValue(body.source || {}),
                line: body.line,
                column: body.column,
            });
            _debugSendSessionUpdate(transport.session, runtime);
        } else if (eventName === 'stopped') {
            _debugSendSessionUpdate(transport.session, {
                state: 'stopped',
                stoppedReason: String(body.reason || ''),
                threadId: body.threadId,
                lastEvent: eventName,
                message: String(body.description || body.text || body.reason || 'Paused'),
            });
        } else if (eventName === 'continued') {
            _debugSendSessionUpdate(transport.session, {
                state: 'running',
                stoppedReason: '',
                threadId: body.threadId,
                lastEvent: eventName,
                message: 'Continued',
            });
        } else if (eventName === 'terminated' || eventName === 'exited') {
            _debugSendSessionUpdate(transport.session, {
                state: eventName,
                stoppedReason: '',
                threadId: body.threadId,
                lastEvent: eventName,
                message: eventName === 'exited' && body.exitCode !== undefined
                    ? `Exited ${body.exitCode}`
                    : eventName,
            });
        }
        transport.customEventEmitter.fire({
            session: transport.session,
            event: eventName,
            body: _plainBridgeValue(body),
        });
        _debugHandleDapEventState(transport, message);
    }
}

function _debugDapSourcePayload(source) {
    const payload = source && typeof source === 'object' ? source : {};
    return {
        name: payload.name,
        path: payload.path,
    };
}

function _debugDapStackFramePayload(frame) {
    const payload = frame && typeof frame === 'object' ? frame : {};
    return {
        id: payload.id,
        name: String(payload.name || ''),
        source: _debugDapSourcePayload(payload.source || {}),
        line: payload.line,
        column: payload.column,
    };
}

function _debugDapVariablePayload(variable) {
    const payload = variable && typeof variable === 'object' ? variable : {};
    return {
        name: String(payload.name || ''),
        value: payload.value === undefined || payload.value === null
            ? ''
            : String(payload.value),
        type: payload.type === undefined || payload.type === null
            ? undefined
            : String(payload.type),
        variablesReference: Number(payload.variablesReference || 0),
        evaluateName: payload.evaluateName,
    };
}

function _debugDapScopePayload(scope, variables) {
    const payload = scope && typeof scope === 'object' ? scope : {};
    return {
        name: String(payload.name || ''),
        variablesReference: Number(payload.variablesReference || 0),
        expensive: !!payload.expensive,
        variables: Array.isArray(variables)
            ? variables.map(_debugDapVariablePayload)
            : [],
    };
}

function _debugDapThreadPayload(thread) {
    const payload = thread && typeof thread === 'object' ? thread : {};
    return {
        id: payload.id,
        name: String(payload.name || `Thread ${payload.id || ''}`).trim(),
    };
}

function _debugSetActiveStackItem(transport, item) {
    transport.activeStackItem = item || undefined;
    transport.session.activeStackItem = item || undefined;
    try { transport.onActiveStackItem?.(item || undefined); } catch {}
    _debugSendSessionUpdate(transport.session, { activeStackItem: item || undefined });
}

function _debugRefreshActiveStackItem(transport, stoppedBody) {
    if (!transport || transport.error) return;
    const body = stoppedBody && typeof stoppedBody === 'object' ? stoppedBody : {};
    const run = async () => {
        const threadsResponse = await transport.sendRequest('threads', {}, 1200);
        const threads = Array.isArray(threadsResponse?.threads)
            ? threadsResponse.threads
            : [];
        const selectedThread = threads.find(item => item?.id === body.threadId)
            || threads[0]
            || (body.threadId !== undefined
                ? { id: body.threadId, name: `Thread ${body.threadId}` }
                : undefined);
        if (!selectedThread) return;
        const stackResponse = await transport.sendRequest('stackTrace', {
            threadId: selectedThread.id,
            startFrame: 0,
            levels: 1,
        }, 1200);
        const frames = Array.isArray(stackResponse?.stackFrames)
            ? stackResponse.stackFrames
            : [];
        const threadPayload = _debugDapThreadPayload(selectedThread);
        const framePayload = frames.length ? _debugDapStackFramePayload(frames[0]) : undefined;
        const scopePayloads = [];
        if (framePayload && framePayload.id !== undefined) {
            const scopesResponse = await transport.sendRequest('scopes', {
                frameId: framePayload.id,
            }, 1200);
            const scopes = Array.isArray(scopesResponse?.scopes)
                ? scopesResponse.scopes
                : [];
            for (const scope of scopes) {
                const reference = Number(scope?.variablesReference || 0);
                let variables = [];
                if (reference > 0) {
                    const variablesResponse = await transport.sendRequest('variables', {
                        variablesReference: reference,
                    }, 1200);
                    variables = Array.isArray(variablesResponse?.variables)
                        ? variablesResponse.variables
                        : [];
                }
                scopePayloads.push(_debugDapScopePayload(scope, variables));
            }
        }
        _debugSetActiveStackItem(transport, {
            session: transport.session,
            thread: threadPayload,
            frame: framePayload,
            scopes: scopePayloads,
            reason: body.reason,
        });
    };
    run().catch(err => _debugCallTrackers(transport.session, 'onError', err));
}

function _debugHandleDapEventState(transport, message) {
    const eventName = String(message.event || '');
    if (eventName === 'stopped') {
        _debugRefreshActiveStackItem(transport, message.body || {});
    } else if (eventName === 'continued' || eventName === 'terminated' || eventName === 'exited') {
        _debugSetActiveStackItem(transport, undefined);
    }
}

function _debugRejectPendingTransportRequests(transport, error) {
    for (const pending of transport.pending.values()) {
        clearTimeout(pending.timer);
        pending.reject(error);
    }
    transport.pending.clear();
}

function _debugCreateStreamTransport(session, config, customEventEmitter, readStream, writeStream, disposer, beforeLaunch) {
    const transport = {
        session,
        readStream,
        writeStream,
        customEventEmitter,
        buffer: '',
        seq: 1,
        pending: new Map(),
        error: null,
        sendRequest(command, args, timeoutMs = 1200) {
            if (transport.error) return Promise.reject(transport.error);
            const seq = transport.seq++;
            const request = {
                seq,
                type: 'request',
                command: String(command || ''),
                arguments: _plainBridgeValue(args || {}),
            };
            _debugCallTrackers(session, 'onWillReceiveMessage', request);
            return new Promise((resolve, reject) => {
                const timer = setTimeout(() => {
                    transport.pending.delete(seq);
                    reject(new Error(`Debug adapter request timed out: ${request.command}`));
                }, timeoutMs);
                transport.pending.set(seq, {
                    resolve,
                    reject,
                    timer,
                    command: request.command,
                });
                try {
                    writeStream.write(_debugDapFrame(request));
                } catch (err) {
                    clearTimeout(timer);
                    transport.pending.delete(seq);
                    reject(err);
                }
            });
        },
        dispose() {
            _debugRejectPendingTransportRequests(
                transport, new Error('Debug session stopped'));
            try { writeStream.end(); } catch {}
            try { disposer?.(); } catch {}
        },
    };
    readStream.on('data', chunk => _debugParseDapFrames(transport, chunk));
    readStream.on('error', err => {
        transport.error = err;
        _debugRejectPendingTransportRequests(transport, err);
        _debugCallTrackers(session, 'onError', err);
    });
    readStream.on('close', () => {
        _debugRejectPendingTransportRequests(
            transport, new Error('Debug adapter transport closed'));
    });
    transport.ready = (async () => {
        await transport.sendRequest('initialize', {
            adapterID: session.type,
            pathFormat: 'path',
            linesStartAt1: true,
            columnsStartAt1: true,
        });
        if (typeof beforeLaunch === 'function') {
            await beforeLaunch(transport);
        }
        if (transport.capabilities?.supportsConfigurationDoneRequest === true) {
            await transport.sendRequest('configurationDone', {});
            transport.configurationDoneSent = true;
        }
        await transport.sendRequest(String(config.request || 'launch'), config || {});
        return true;
    })().catch(err => {
        transport.error = err;
        _debugCallTrackers(session, 'onError', err);
        return false;
    });
    return transport;
}

function _debugCreateExecutableAdapterTransport(session, descriptor, config, customEventEmitter, beforeLaunch) {
    if (!(descriptor instanceof DebugAdapterExecutable) || !descriptor.command) return null;
    const options = descriptor.options && typeof descriptor.options === 'object'
        ? descriptor.options
        : {};
    const cwd = options.cwd ? String(options.cwd) : _workspaceRoot;
    const env = Object.assign({}, process.env, options.env || {});
    let proc;
    try {
        proc = childProcess.spawn(
            descriptor.command,
            Array.isArray(descriptor.args) ? descriptor.args : [],
            { cwd, env, stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true },
        );
    } catch (err) {
        _debugCallTrackers(session, 'onError', err);
        return { session, error: err, pending: new Map(), dispose() {} };
    }
    const transport = _debugCreateStreamTransport(
        session,
        config,
        customEventEmitter,
        proc.stdout,
        proc.stdin,
        () => {
            try { proc.stdin.end(); } catch {}
            try { if (!proc.killed) proc.kill(); } catch {}
        },
        beforeLaunch);
    transport.process = proc;
    proc.stderr.on('data', chunk => {
        const text = chunk.toString('utf8');
        if (text) send({ type: 'output', channel: 'Debug Adapter', text });
    });
    proc.on('error', err => {
        transport.error = err;
        _debugRejectPendingTransportRequests(transport, err);
        _debugCallTrackers(session, 'onError', err);
    });
    proc.on('exit', (code, signal) => {
        _debugRejectPendingTransportRequests(
            transport, new Error(`Debug adapter exited: ${code}:${signal}`));
        _debugCallTrackers(session, 'onExit', code, signal);
    });
    return transport;
}

function _debugCreateServerAdapterTransport(session, descriptor, config, customEventEmitter, beforeLaunch) {
    if (!(descriptor instanceof DebugAdapterServer)) return null;
    const port = Number(descriptor.port);
    if (!Number.isFinite(port) || port <= 0) return null;
    const host = descriptor.host || '127.0.0.1';
    const socket = net.createConnection({ port, host });
    return _debugCreateStreamTransport(
        session, config, customEventEmitter, socket, socket,
        () => { try { socket.destroy(); } catch {} },
        beforeLaunch);
}

function _debugCreateNamedPipeAdapterTransport(session, descriptor, config, customEventEmitter, beforeLaunch) {
    if (!(descriptor instanceof DebugAdapterNamedPipeServer)) return null;
    const pipePath = String(descriptor.path || '').trim();
    if (!pipePath) return null;
    const socket = net.createConnection({ path: pipePath });
    return _debugCreateStreamTransport(
        session, config, customEventEmitter, socket, socket,
        () => { try { socket.destroy(); } catch {} },
        beforeLaunch);
}

function _debugCreateAdapterTransport(session, descriptor, config, customEventEmitter, beforeLaunch) {
    return _debugCreateExecutableAdapterTransport(
        session, descriptor, config, customEventEmitter, beforeLaunch)
        || _debugCreateServerAdapterTransport(
            session, descriptor, config, customEventEmitter, beforeLaunch)
        || _debugCreateNamedPipeAdapterTransport(
            session, descriptor, config, customEventEmitter, beforeLaunch);
}

function _notebookSerializerByHandleOrViewType(handle, viewType) {
    const numericHandle = Number(handle);
    if (Number.isFinite(numericHandle) && _notebookSerializers.has(numericHandle)) {
        return { handle: numericHandle, entry: _notebookSerializers.get(numericHandle) };
    }
    const wanted = String(viewType || '');
    if (wanted) {
        for (const [entryHandle, entry] of _notebookSerializers.entries()) {
            if (entry.viewType === wanted) return { handle: entryHandle, entry };
        }
    }
    return null;
}

function _notebookSerializerByViewType(viewType) {
    const wanted = String(viewType || '');
    if (!wanted) return null;
    for (const [handle, entry] of _notebookSerializers.entries()) {
        if (entry.viewType === wanted) return { handle, entry };
    }
    return null;
}

function _notebookFilenamePatternMatches(pattern, uri) {
    const normalizedPattern = String(pattern || '').replace(/\\/g, '/').toLowerCase();
    if (!normalizedPattern) return false;
    const resource = String(uri?.fsPath || uri?.path || '').replace(/\\/g, '/').toLowerCase();
    const basename = path.posix.basename(resource);
    if (normalizedPattern === '*' || normalizedPattern === basename || normalizedPattern === resource) {
        return true;
    }
    if (normalizedPattern.startsWith('*.')) {
        return basename.endsWith(normalizedPattern.slice(1));
    }
    if (normalizedPattern.startsWith('**/*.')) {
        return basename.endsWith(normalizedPattern.slice(4));
    }
    const escaped = normalizedPattern
        .replace(/[.+^${}()|[\]\\]/g, '\\$&')
        .replace(/\*\*/g, '.*')
        .replace(/\*/g, '[^/]*');
    return new RegExp(`^${escaped}$`).test(resource)
        || new RegExp(`^${escaped}$`).test(basename);
}

function _notebookContributionViewTypesForUri(uri) {
    const matches = [];
    for (const known of _knownExtensions.values()) {
        const notebooks = known?.manifest?.contributes?.notebooks;
        for (const contribution of Array.isArray(notebooks) ? notebooks : []) {
            const viewType = String(
                contribution?.type || contribution?.viewType || contribution?.id || '',
            ).trim();
            if (!viewType) continue;
            const selectors = Array.isArray(contribution?.selector)
                ? contribution.selector
                : [];
            if (!selectors.length) continue;
            const matched = selectors.some(selector => {
                if (typeof selector === 'string') {
                    return _notebookFilenamePatternMatches(selector, uri);
                }
                return _notebookFilenamePatternMatches(selector?.filenamePattern, uri);
            });
            if (matched) matches.push(viewType);
        }
    }
    return matches;
}

async function _notebookSerializerForUri(uri) {
    const viewTypes = _notebookContributionViewTypesForUri(uri);
    for (const viewType of viewTypes) {
        await _activateKnownExtensionsForEvent(`onNotebook:${viewType}`);
        const serializer = _notebookSerializerByViewType(viewType);
        if (serializer) return serializer;
    }
    for (const [handle, entry] of _notebookSerializers.entries()) {
        return { handle, entry };
    }
    return null;
}

function _registerNotebookSerializer(extDesc, viewType, serializer, options) {
    const normalized = String(viewType || '').trim();
    if (!normalized) throw new Error('viewType cannot be empty or just whitespace');
    if (!serializer || typeof serializer.deserializeNotebook !== 'function'
            || typeof serializer.serializeNotebook !== 'function') {
        throw new Error('NotebookSerializer must implement serializeNotebook and deserializeNotebook');
    }
    const handle = _nextNotebookSerializerHandle++;
    const entry = {
        handle,
        viewType: normalized,
        serializer,
        options: options && typeof options === 'object' ? options : {},
        extensionId: extDesc.extensionId || '',
    };
    _notebookSerializers.set(handle, entry);
    send({
        type: 'notebook_serializer_registered',
        handle,
        viewType: entry.viewType,
        extensionId: entry.extensionId,
        options: _plainBridgeValue(entry.options),
    });
    return new Disposable(() => {
        if (_notebookSerializers.get(handle) !== entry) return;
        _notebookSerializers.delete(handle);
        send({
            type: 'notebook_serializer_disposed',
            handle,
            viewType: entry.viewType,
            extensionId: entry.extensionId,
        });
    });
}

function _registerNotebookCellStatusBarItemProvider(extDesc, notebookType, provider) {
    const normalized = String(notebookType || '').trim();
    if (!normalized) throw new Error('notebookType cannot be empty or just whitespace');
    if (!provider || typeof provider.provideCellStatusBarItems !== 'function') {
        throw new Error('NotebookCellStatusBarItemProvider must implement provideCellStatusBarItems');
    }
    const handle = _nextNotebookStatusBarProviderHandle++;
    const entry = {
        handle,
        notebookType: normalized,
        provider,
        extensionId: extDesc.extensionId || '',
        subscription: null,
    };
    if (typeof provider.onDidChangeCellStatusBarItems === 'function') {
        entry.subscription = provider.onDidChangeCellStatusBarItems(() => {
            send({
                type: 'notebook_cell_status_bar_changed',
                handle,
                notebookType: normalized,
                extensionId: entry.extensionId,
            });
        });
    }
    _notebookStatusBarProviders.set(handle, entry);
    send({
        type: 'notebook_cell_status_bar_provider_registered',
        handle,
        notebookType: normalized,
        extensionId: entry.extensionId,
        hasChangeEvent: !!entry.subscription,
    });
    return new Disposable(() => {
        if (_notebookStatusBarProviders.get(handle) !== entry) return;
        _notebookStatusBarProviders.delete(handle);
        entry.subscription?.dispose?.();
        send({
            type: 'notebook_cell_status_bar_provider_disposed',
            handle,
            notebookType: normalized,
            extensionId: entry.extensionId,
        });
    });
}

function _notebookStatusBarCommandPayload(command) {
    if (!command) return undefined;
    if (typeof command === 'string') {
        return { command, title: '', arguments: [] };
    }
    if (typeof command === 'object') {
        const commandId = String(command.command || command.id || '');
        if (!commandId) return undefined;
        return {
            command: commandId,
            title: String(command.title || commandId),
            arguments: Array.isArray(command.arguments)
                ? command.arguments.map(_plainBridgeValue)
                : [],
        };
    }
    return undefined;
}

function _notebookStatusBarItemPayload(item) {
    if (!item || typeof item !== 'object') return null;
    const command = _notebookStatusBarCommandPayload(item.command);
    const payload = {
        text: String(item.text ?? ''),
        alignment: Number(item.alignment) === NotebookCellStatusBarAlignment.Right ? 2 : 1,
        priority: item.priority === undefined || item.priority === null
            ? undefined
            : Number(item.priority),
        tooltip: item.tooltip === undefined || item.tooltip === null
            ? ''
            : String(item.tooltip),
        command: command || null,
        accessibilityInformation: _plainBridgeValue(item.accessibilityInformation || null),
    };
    if (!Number.isFinite(payload.priority)) delete payload.priority;
    return payload;
}

function _notebookDocumentFromStatusBarRequest(msg) {
    const uri = _workspaceUriFromInput(msg.uri || msg.resourceUri || msg.path || '');
    const existing = _notebookDocuments.get(uri.toString());
    if (existing) return existing;
    const viewType = String(
        msg.viewType
        || msg.notebookType
        || msg.notebook?.view_type
        || msg.notebook?.viewType
        || 'interactive');
    return _createNotebookDocument(
        viewType,
        uri,
        msg.notebook && typeof msg.notebook === 'object' ? msg.notebook : { cells: [] },
        { transientPreview: true });
}

async function _handleNotebookCellStatusBarRequest(msg) {
    const requestId = String(msg.requestId || '');
    try {
        const notebook = _notebookDocumentFromStatusBarRequest(msg);
        const index = Number(msg.index ?? msg.cellIndex ?? 0);
        if (!Number.isFinite(index) || index < 0 || index >= notebook.cellCount) {
            throw new Error('Notebook cell index is out of range');
        }
        const targetHandle = Number(msg.handle || 0);
        const providers = Array.from(_notebookStatusBarProviders.values())
            .filter(entry => (!targetHandle || entry.handle === targetHandle)
                && entry.notebookType === notebook.notebookType);
        const tokenSource = new CancellationTokenSource();
        const cell = notebook.cellAt(index);
        const items = [];
        for (const entry of providers) {
            const raw = await entry.provider.provideCellStatusBarItems(
                cell,
                tokenSource.token);
            const values = raw === undefined || raw === null
                ? []
                : (Array.isArray(raw) ? raw : [raw]);
            for (const item of values) {
                const payload = _notebookStatusBarItemPayload(item);
                if (payload) {
                    payload.providerHandle = entry.handle;
                    payload.extensionId = entry.extensionId;
                    items.push(payload);
                }
            }
        }
        send({
            type: 'notebook_cell_status_bar_response',
            requestId,
            ok: true,
            uri: notebook.uri.toString(),
            notebookType: notebook.notebookType,
            index,
            value: items,
            providerCount: providers.length,
        });
    } catch (err) {
        send({
            type: 'notebook_cell_status_bar_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
            value: [],
        });
    }
}

function _createNotebookController(extDesc, id, notebookType, label, handler) {
    const normalizedId = String(id || '').trim();
    const normalizedType = String(notebookType || '').trim();
    if (!normalizedId || !normalizedType) {
        throw new Error('NotebookController id and notebookType are required');
    }
    const handle = _nextNotebookControllerHandle++;
    const selectedEmitter = new EventEmitter();
    const affinities = new Map();
    const controller = {
        id: normalizedId,
        notebookType: normalizedType,
        label: label === undefined || label === null ? normalizedId : String(label),
        description: undefined,
        detail: undefined,
        supportedLanguages: [],
        supportsExecutionOrder: true,
        executeHandler: typeof handler === 'function' ? handler : undefined,
        extensionId: extDesc.extensionId || '',
        _handle: handle,
        _selectedEmitter: selectedEmitter,
        _affinities: affinities,
        get onDidChangeSelectedNotebooks() {
            return selectedEmitter.event;
        },
        createNotebookCellExecution(cell) {
            return _createNotebookCellExecution(controller, cell);
        },
        updateNotebookAffinity(notebook, affinity) {
            if (!notebook || !notebook.uri) {
                throw new Error('Notebook affinity requires a notebook document');
            }
            const normalizedAffinity = Number(affinity) === NotebookControllerAffinity.Preferred
                ? NotebookControllerAffinity.Preferred
                : NotebookControllerAffinity.Default;
            const uri = notebook.uri.toString();
            affinities.set(uri, normalizedAffinity);
            send({
                type: 'notebook_controller_affinity_changed',
                handle,
                id: normalizedId,
                notebookType: normalizedType,
                uri,
                affinity: normalizedAffinity,
                extensionId: controller.extensionId,
            });
        },
        dispose() {
            for (const [uri, selectedHandle] of [..._selectedNotebookControllers.entries()]) {
                if (selectedHandle === handle) _selectedNotebookControllers.delete(uri);
            }
            _notebookControllers.delete(handle);
            selectedEmitter.dispose();
            send({
                type: 'notebook_controller_disposed',
                handle,
                id: normalizedId,
                notebookType: normalizedType,
                extensionId: controller.extensionId,
            });
        },
    };
    _notebookControllers.set(handle, controller);
    send({
        type: 'notebook_controller_registered',
        handle,
        id: normalizedId,
        notebookType: normalizedType,
        label: controller.label,
        description: controller.description || '',
        detail: controller.detail || '',
        supportedLanguages: Array.isArray(controller.supportedLanguages)
            ? controller.supportedLanguages.slice()
            : [],
        supportsExecutionOrder: controller.supportsExecutionOrder !== false,
        extensionId: controller.extensionId,
    });
    return controller;
}

function _notebookControllerPayload(controller) {
    if (!controller) return null;
    return {
        handle: controller._handle,
        id: controller.id,
        notebookType: controller.notebookType,
        label: String(controller.label || controller.id || ''),
        description: controller.description === undefined || controller.description === null
            ? ''
            : String(controller.description),
        detail: controller.detail === undefined || controller.detail === null
            ? ''
            : String(controller.detail),
        supportedLanguages: Array.isArray(controller.supportedLanguages)
            ? controller.supportedLanguages.map(value => String(value))
            : [],
        supportsExecutionOrder: controller.supportsExecutionOrder !== false,
        extensionId: controller.extensionId || '',
    };
}

function _notebookControllerByHandleOrId(handle, id, notebookType) {
    const numericHandle = Number(handle);
    if (Number.isFinite(numericHandle) && _notebookControllers.has(numericHandle)) {
        return _notebookControllers.get(numericHandle);
    }
    const wantedId = String(id || '').trim();
    const wantedType = String(notebookType || '').trim();
    for (const controller of _notebookControllers.values()) {
        if (wantedId && controller.id !== wantedId) continue;
        if (wantedType && controller.notebookType !== wantedType) continue;
        return controller;
    }
    return null;
}

function _notebookDocumentFromControllerRequest(msg) {
    const uri = _workspaceUriFromInput(msg.uri || msg.resourceUri || msg.path || '');
    const existing = _notebookDocuments.get(uri.toString());
    if (existing) return existing;
    const viewType = String(
        msg.viewType
        || msg.notebookType
        || msg.notebook?.view_type
        || msg.notebook?.viewType
        || 'interactive');
    return _createNotebookDocument(
        viewType,
        uri,
        msg.notebook && typeof msg.notebook === 'object' ? msg.notebook : { cells: [] },
        { transientPreview: true });
}

function _sendNotebookControllerResponse(requestId, ok, extra = {}) {
    send({
        type: 'notebook_controller_response',
        requestId,
        ok: !!ok,
        ...extra,
    });
}

async function _handleNotebookControllersRequest(msg) {
    const requestId = String(msg.requestId || '');
    const notebookType = String(msg.notebookType || msg.viewType || '').trim();
    const values = Array.from(_notebookControllers.values())
        .filter(controller => !notebookType || controller.notebookType === notebookType)
        .map(_notebookControllerPayload)
        .filter(Boolean);
    _sendNotebookControllerResponse(requestId, true, { value: values });
}

async function _handleNotebookControllerSelectRequest(msg) {
    const requestId = String(msg.requestId || '');
    try {
        const notebook = _notebookDocumentFromControllerRequest(msg);
        const controller = _notebookControllerByHandleOrId(
            msg.handle,
            msg.controllerId || msg.id,
            msg.notebookType || msg.viewType || notebook.notebookType);
        if (!controller) throw new Error('Notebook controller not found');
        if (controller.notebookType !== notebook.notebookType) {
            throw new Error('Notebook controller does not match notebook type');
        }
        const uri = notebook.uri.toString();
        const selected = msg.selected !== false;
        const previousHandle = _selectedNotebookControllers.get(uri);
        if (selected) {
            if (previousHandle && previousHandle !== controller._handle) {
                const previous = _notebookControllers.get(previousHandle);
                previous?._selectedEmitter?.fire?.({ notebook, selected: false });
                send({
                    type: 'notebook_controller_selection_changed',
                    handle: previousHandle,
                    id: previous?.id || '',
                    notebookType: notebook.notebookType,
                    uri,
                    selected: false,
                    extensionId: previous?.extensionId || '',
                });
            }
            _selectedNotebookControllers.set(uri, controller._handle);
        } else if (previousHandle === controller._handle) {
            _selectedNotebookControllers.delete(uri);
        }
        controller._selectedEmitter.fire({ notebook, selected });
        send({
            type: 'notebook_controller_selection_changed',
            handle: controller._handle,
            id: controller.id,
            notebookType: notebook.notebookType,
            uri,
            selected,
            extensionId: controller.extensionId,
        });
        _sendNotebookControllerResponse(requestId, true, {
            value: {
                controller: _notebookControllerPayload(controller),
                selected,
                uri,
            },
        });
    } catch (err) {
        _sendNotebookControllerResponse(requestId, false, {
            error: err?.message || String(err),
            value: null,
        });
    }
}

async function _handleNotebookControllerExecuteRequest(msg) {
    const requestId = String(msg.requestId || '');
    try {
        const notebook = _notebookDocumentFromControllerRequest(msg);
        const controller = _notebookControllerByHandleOrId(
            msg.handle,
            msg.controllerId || msg.id,
            msg.notebookType || msg.viewType || notebook.notebookType);
        if (!controller) throw new Error('Notebook controller not found');
        if (typeof controller.executeHandler !== 'function') {
            throw new Error('Notebook controller does not implement executeHandler');
        }
        if (controller.notebookType !== notebook.notebookType) {
            throw new Error('Notebook controller does not match notebook type');
        }
        const rawIndices = Array.isArray(msg.cellIndices)
            ? msg.cellIndices
            : (msg.cellIndex === undefined ? [] : [msg.cellIndex]);
        const cells = (rawIndices.length ? rawIndices : notebook.getCells().map(cell => cell.index))
            .map(value => Number(value))
            .filter(index => Number.isFinite(index) && index >= 0 && index < notebook.cellCount)
            .map(index => notebook.cellAt(index));
        await Promise.resolve(controller.executeHandler(cells, notebook, controller));
        _sendNotebookControllerResponse(requestId, true, {
            uri: notebook.uri.toString(),
            notebookType: notebook.notebookType,
            value: {
                controller: _notebookControllerPayload(controller),
                cellCount: cells.length,
                cellIndices: cells.map(cell => cell.index),
                notebook: _notebookDataPayload(notebook._data),
            },
        });
    } catch (err) {
        _sendNotebookControllerResponse(requestId, false, {
            error: err?.message || String(err),
            value: null,
        });
    }
}

function _createNotebookCellExecution(controller, cell) {
    const notebook = cell?.notebook;
    const index = Number(cell?.index);
    if (!notebook || !Number.isFinite(index) || index < 0 || index >= notebook.cellCount) {
        throw new Error('Notebook cell execution requires a live notebook cell');
    }
    let executionOrder = undefined;
    return {
        get token() {
            return { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
        },
        get executionOrder() { return executionOrder; },
        set executionOrder(value) {
            const numeric = Number(value);
            executionOrder = Number.isFinite(numeric) ? numeric : undefined;
            const data = notebook._data.cells[index];
            data.executionSummary = Object.assign(
                {}, data.executionSummary || {}, { executionOrder });
            _fireNotebookChange(notebook, {
                cellChanges: [{ cell: notebook.cellAt(index), executionSummary: data.executionSummary }],
            });
        },
        start(startTime) {
            const data = notebook._data.cells[index];
            data.executionSummary = Object.assign({}, data.executionSummary || {}, {
                timing: Object.assign({}, data.executionSummary?.timing || {}, {
                    startTime: Number(startTime || Date.now()),
                }),
            });
            _fireNotebookChange(notebook, {
                cellChanges: [{ cell: notebook.cellAt(index), executionSummary: data.executionSummary }],
            });
        },
        end(success, endTime) {
            const data = notebook._data.cells[index];
            data.executionSummary = Object.assign({}, data.executionSummary || {}, {
                success: success !== false,
                timing: Object.assign({}, data.executionSummary?.timing || {}, {
                    endTime: Number(endTime || Date.now()),
                }),
            });
            _fireNotebookChange(notebook, {
                cellChanges: [{ cell: notebook.cellAt(index), executionSummary: data.executionSummary }],
            });
        },
        clearOutput() {
            notebook._data.cells[index].outputs = [];
            _fireNotebookChange(notebook, {
                cellChanges: [{ cell: notebook.cellAt(index), outputs: [] }],
            });
            return Promise.resolve();
        },
        replaceOutput(outputs) {
            const next = (Array.isArray(outputs) ? outputs : [outputs])
                .filter(value => value !== undefined && value !== null)
                .map(_notebookOutputFromPlain);
            notebook._data.cells[index].outputs = next;
            _fireNotebookChange(notebook, {
                cellChanges: [{ cell: notebook.cellAt(index), outputs: next }],
            });
            return Promise.resolve();
        },
        appendOutput(outputs) {
            const next = (Array.isArray(outputs) ? outputs : [outputs])
                .filter(value => value !== undefined && value !== null)
                .map(_notebookOutputFromPlain);
            notebook._data.cells[index].outputs = [
                ...(notebook._data.cells[index].outputs || []),
                ...next,
            ];
            _fireNotebookChange(notebook, {
                cellChanges: [{ cell: notebook.cellAt(index), outputs: notebook._data.cells[index].outputs }],
            });
            return Promise.resolve();
        },
        replaceOutputItems(items, output) {
            const data = notebook._data.cells[index];
            const targetId = output && output.id;
            const nextItems = NotebookCellOutput.ensureUniqueMimeTypes(
                Array.isArray(items) ? items : [items]);
            for (const item of data.outputs || []) {
                if (!targetId || item.id === targetId) {
                    item.items = nextItems;
                    _fireNotebookChange(notebook, {
                        cellChanges: [{ cell: notebook.cellAt(index), outputs: data.outputs }],
                    });
                    return Promise.resolve();
                }
            }
            return Promise.resolve();
        },
        appendOutputItems(items, output) {
            const data = notebook._data.cells[index];
            const targetId = output && output.id;
            const nextItems = (Array.isArray(items) ? items : [items])
                .map(_notebookOutputItemFromPlain);
            for (const item of data.outputs || []) {
                if (!targetId || item.id === targetId) {
                    item.items = NotebookCellOutput.ensureUniqueMimeTypes([
                        ...(item.items || []),
                        ...nextItems,
                    ]);
                    _fireNotebookChange(notebook, {
                        cellChanges: [{ cell: notebook.cellAt(index), outputs: data.outputs }],
                    });
                    return Promise.resolve();
                }
            }
            return Promise.resolve();
        },
    };
}

async function _openNotebookDocument(uriOrType, content) {
    if (uriOrType instanceof Uri) {
        const key = uriOrType.toString();
        const existing = _notebookDocuments.get(key);
        if (existing) return existing;
        const explicitViewType = String(content?.viewType || content?.notebookType || '').trim();
        if (content instanceof NotebookData || (content && Array.isArray(content.cells))) {
            const viewType = explicitViewType || 'interactive';
            const serializer = _notebookSerializerByViewType(viewType);
            return _createNotebookDocument(viewType, uriOrType, content, {
                isDirty: true,
                serializerHandle: serializer?.handle,
                serializerOptions: serializer?.entry?.options || {},
            });
        }
        let serializer = explicitViewType
            ? _notebookSerializerByViewType(explicitViewType)
            : null;
        if (!serializer) serializer = await _notebookSerializerForUri(uriOrType);
        if (!serializer) throw new Error(`No notebook serializer found for ${uriOrType.toString()}`);
        const tokenSource = new CancellationTokenSource();
        const rawBytes = await _workspaceFsReadFile(uriOrType);
        const notebookData = await serializer.entry.serializer.deserializeNotebook(
            rawBytes,
            tokenSource.token,
        );
        return _createNotebookDocument(serializer.entry.viewType, uriOrType, notebookData, {
            isDirty: false,
            serializerHandle: serializer.handle,
            serializerOptions: serializer.entry.options || {},
        });
    }
    if (typeof uriOrType === 'string') {
        const viewType = String(uriOrType || '').trim();
        if (!viewType) throw new Error('Invalid notebook type');
        const serializer = _notebookSerializerByViewType(viewType);
        const uri = new Uri(
            'untitled',
            '',
            `/Untitled-${_nextNotebookDocumentHandle++}.${viewType.replace(/[^a-z0-9_.-]/gi, '-')}`,
            '',
            '',
        );
        return _createNotebookDocument(viewType, uri, content || new NotebookData([]), {
            isDirty: !!content,
            serializerHandle: serializer?.handle,
            serializerOptions: serializer?.entry?.options || {},
        });
    }
    throw new Error('Invalid notebook arguments');
}

async function _handleNotebookDeserializeRequest(msg) {
    const requestId = msg.requestId;
    try {
        const found = _notebookSerializerByHandleOrViewType(msg.handle, msg.viewType);
        if (!found) throw new Error('Notebook serializer not found');
        const tokenSource = new CancellationTokenSource();
        const data = await found.entry.serializer.deserializeNotebook(
            _notebookBytesFromMessage(msg),
            tokenSource.token,
        );
        send({
            type: 'notebook_deserialize_response',
            requestId,
            ok: true,
            handle: found.handle,
            viewType: found.entry.viewType,
            value: _notebookDataPayload(data),
        });
    } catch (err) {
        send({
            type: 'notebook_deserialize_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function _handleNotebookSerializeRequest(msg) {
    const requestId = msg.requestId;
    try {
        const found = _notebookSerializerByHandleOrViewType(msg.handle, msg.viewType);
        if (!found) throw new Error('Notebook serializer not found');
        const tokenSource = new CancellationTokenSource();
        const bytes = await found.entry.serializer.serializeNotebook(
            _notebookDataFromPlain(msg.notebook || msg.value || { cells: [] }),
            tokenSource.token,
        );
        send({
            type: 'notebook_serialize_response',
            requestId,
            ok: true,
            handle: found.handle,
            viewType: found.entry.viewType,
            ..._notebookBytesPayload(bytes),
        });
    } catch (err) {
        send({
            type: 'notebook_serialize_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function handleExtensionTaskExecuteRequest(msg) {
    const requestId = String(msg.requestId || '');
    const taskType = String(msg.taskType || msg.type || '').trim();
    try {
        if (!taskType) throw new Error('taskType is required');
        await _activateKnownExtensionsForEvent(`onTaskType:${taskType}`);
        const provider = _taskProviders.get(taskType);
        if (!provider) throw new Error(`No task provider registered for type ${taskType}`);
        const provided = await _callTaskProvider(provider, 'provideTasks');
        const tasks = (Array.isArray(provided) ? provided : [])
            .filter(task => _taskMatchesFilter(task, { type: taskType }));
        const task = (Array.isArray(tasks) ? tasks : [])[0];
        if (!task) throw new Error(`No task provided for type ${taskType}`);
        if (task && typeof task === 'object'
                && task.definition && !task.definition.type) {
            task.definition.type = taskType;
        }
        const resolved = await _resolveTask(task);
        const executionId = `task-request-${Date.now()}-${Math.floor(Math.random() * 10000)}`;
        const executionSpec = _taskExecutionSpec(resolved);
        const bridgeMetadata = _taskBridgeMetadata(resolved, executionSpec, {
            requested: true,
            executionId,
        });
        send({
            type: 'task_execute',
            executionId,
            task: _serializeTask(resolved),
            commandLine: executionSpec.commandLine || '',
            metadata: bridgeMetadata,
        });
        send({
            type: 'extension_task_execute_response',
            requestId,
            ok: true,
            value: {
                executionId,
                task: _serializeTask(resolved),
                commandLine: executionSpec.commandLine || '',
                metadata: bridgeMetadata,
                taskType,
                taskCount: tasks.length,
            },
        });
    } catch (err) {
        send({
            type: 'extension_task_execute_response',
            requestId,
            ok: false,
            error: String(err && err.message || err),
            value: { taskType },
        });
    }
}

async function handleExtensionDebugStartRequest(msg) {
    const requestId = String(msg.requestId || '');
    const debugType = String(msg.debugType || msg.type || '').trim();
    try {
        if (!debugType) throw new Error('debugType is required');
        const rawConfig = msg.config && typeof msg.config === 'object'
            ? msg.config
            : {};
        const config = {
            type: debugType,
            request: String(rawConfig.request || 'launch'),
            name: String(rawConfig.name || msg.label || debugType || 'Debug'),
            ...rawConfig,
        };
        await _activateKnownExtensionsForEvent('onDebug');
        await _activateKnownExtensionsForEvent(`onDebugResolve:${debugType}`);
        const providers = _debugConfigProviders.get(debugType) || [];
        for (const entry of providers) {
            const provider = entry && entry.provider;
            if (provider && typeof provider.resolveDebugConfiguration === 'function') {
                const resolved = await Promise.resolve(
                    provider.resolveDebugConfiguration(
                        undefined, { ...config }, _debugProviderToken()));
                if (!resolved) throw new Error(`Debug configuration rejected for ${debugType}`);
                Object.assign(config, resolved);
            }
        }
        const session = {
            id: `debug-request-${_nextDebugSessionHandle++}`,
            type: String(config.type || debugType || 'debug'),
            name: String(config.name || config.type || 'Debug'),
            workspaceFolder: undefined,
            configuration: { ...config },
        };
        _debugUpdateActive(session, null);
        send({
            type: 'debug_start',
            session: _debugSessionPayload(session),
            config: _plainBridgeValue(config || {}),
            requested: true,
        });
        send({
            type: 'extension_debug_start_response',
            requestId,
            ok: true,
            value: {
                started: true,
                debugType,
                config: _plainBridgeValue(config),
                session: _debugSessionPayload(session),
            },
        });
    } catch (err) {
        send({
            type: 'extension_debug_start_response',
            requestId,
            ok: false,
            error: String(err && err.message || err),
            value: { debugType },
        });
    }
}

function _debugUpdateActive(session, emitter) {
    _activeDebugSession = session || null;
    if (emitter) emitter.fire(_activeDebugSession);
}

function _terminalBridgePayload(terminal) {
    if (!terminal) return null;
    return Object.assign(
        { id: terminal._handle, active: _activeTerminal === terminal },
        terminal.metadata());
}

function _terminalSetActive(terminal) {
    if (_activeTerminal === terminal) return;
    _activeTerminal = terminal;
    _onDidChangeActiveTerminalEmitter.fire(terminal);
    send({
        type: 'terminal_active',
        terminal: _terminalBridgePayload(terminal),
    });
}

function _terminalRemove(terminal) {
    const index = _terminals.indexOf(terminal);
    if (index < 0) return;
    _terminals.splice(index, 1);
    if (_activeTerminal === terminal) {
        _activeTerminal = _terminals.length ? _terminals[_terminals.length - 1] : undefined;
        _onDidChangeActiveTerminalEmitter.fire(_activeTerminal);
        send({
            type: 'terminal_active',
            terminal: _terminalBridgePayload(_activeTerminal),
        });
    }
    _onDidCloseTerminalEmitter.fire(terminal);
}

function _terminalSubscribeEvent(event, callback) {
    if (typeof event !== 'function') return null;
    try {
        return event(callback);
    } catch (err) {
        log(`terminal event subscription failed: ${err?.message || err}`);
        return null;
    }
}

class TerminalObject {
    constructor(options = {}) {
        const normalizedOptions = _terminalNormalizeOptions(options);
        this._handle = _nextTerminalHandle++;
        this._disposed = false;
        this._ptyOpened = false;
        this._ptyDisposables = [];
        this.creationOptions = normalizedOptions;
        this._resolvedEnv = _terminalResolveEnv(normalizedOptions);
        this.name = String(normalizedOptions?.name || `Terminal ${this._handle}`);
        this.processId = Promise.resolve(undefined);
        this.exitStatus = undefined;
        this.state = { isInteractedWith: false };
        this.shellIntegration = undefined;
        this._shellIntegrationObject = null;
        this._shellExecutions = [];
        this.dimensions = undefined;
        this._pty = normalizedOptions?.pty && typeof normalizedOptions.pty.open === 'function'
            ? normalizedOptions.pty
            : null;
        this._bindPseudoterminal();
    }
    _markInteracted() {
        if (this.state && this.state.isInteractedWith === true) return false;
        this.state = Object.assign({}, this.state || {}, { isInteractedWith: true });
        _onDidChangeTerminalStateEmitter.fire(this);
        send({
            type: 'terminal_state',
            terminal: _terminalBridgePayload(this),
            state: _terminalOptionValue(this.state),
        });
        return true;
    }
    _bindPseudoterminal() {
        const pty = this._pty;
        if (!pty) return;
        const writeDisposable = _terminalSubscribeEvent(pty.onDidWrite, data => {
            if (!this._ptyOpened || this._disposed) return;
            this._appendShellExecutionOutput(data);
            send({
                type: 'terminal_write',
                id: this._handle,
                name: this.name,
                text: String(data ?? ''),
            });
        });
        const closeDisposable = _terminalSubscribeEvent(pty.onDidClose, code => {
            if (!this._ptyOpened || this._disposed) return;
            this._closeFromPty(code);
        });
        const nameDisposable = _terminalSubscribeEvent(pty.onDidChangeName, name => {
            if (!this._ptyOpened || this._disposed) return;
            const previousName = this.name;
            this.name = String(name || this.name);
            send({
                type: 'terminal_rename',
                id: this._handle,
                previousName,
                name: this.name,
            });
        });
        const dimensionsDisposable = _terminalSubscribeEvent(
            pty.onDidOverrideDimensions, dimensions => {
                if (!this._ptyOpened || this._disposed) return;
                const columns = Number(dimensions?.columns);
                const rows = Number(dimensions?.rows);
                this.dimensions = (
                    Number.isFinite(columns) && Number.isFinite(rows)
                        ? { columns, rows }
                        : undefined
                );
                send({
                    type: 'terminal_dimensions',
                    id: this._handle,
                    name: this.name,
                    dimensions: this.dimensions || null,
                });
            });
        this._ptyDisposables = [
            writeDisposable,
            closeDisposable,
            nameDisposable,
            dimensionsDisposable,
        ].filter(Boolean);
    }
    _disposePtySubscriptions() {
        for (const disposable of this._ptyDisposables.splice(0)) {
            try { disposable?.dispose?.(); } catch {}
        }
    }
    _openPty() {
        if (!this._pty || this._ptyOpened || this._disposed) return;
        this._ptyOpened = true;
        try {
            this._pty.open(undefined);
        } catch (err) {
            log(`terminal pty open failed for ${this.name}: ${err?.message || err}`);
            this._closeFromPty(1);
        }
    }
    _closeFromPty(code) {
        if (this._disposed) return;
        this._disposed = true;
        const numericCode = typeof code === 'number' ? code : undefined;
        this.exitStatus = { code: numericCode, reason: 2 };
        send({ type: 'terminal_dispose', id: this._handle, name: this.name });
        this._disposePtySubscriptions();
        _terminalRemove(this);
    }
    _ensureShellIntegration() {
        if (this._disposed) return undefined;
        if (this.shellIntegration) return this.shellIntegration;
        this._shellIntegrationObject = new TerminalShellIntegration(this);
        this.shellIntegration = this._shellIntegrationObject.value;
        const event = { terminal: this, shellIntegration: this.shellIntegration };
        _onDidChangeTerminalShellIntegrationEmitter.fire(event);
        send({
            type: 'terminal_shell_integration',
            terminal: _terminalBridgePayload(this),
            cwd: this._shellIntegrationObject.cwdPayload(),
            env: this._shellIntegrationObject.envPayload(),
        });
        return this.shellIntegration;
    }
    _trackShellExecution(execution) {
        if (!execution || this._shellExecutions.includes(execution)) return;
        this._shellExecutions.push(execution);
    }
    _finishShellExecution(execution) {
        const index = this._shellExecutions.indexOf(execution);
        if (index >= 0) this._shellExecutions.splice(index, 1);
    }
    _appendShellExecutionOutput(data) {
        for (const execution of this._shellExecutions.slice()) {
            execution._append(data);
        }
    }
    sendText(text, shouldExecute = true) {
        if (this._disposed) return;
        this._markInteracted();
        this._ensureShellIntegration();
        if (this._pty) {
            this._openPty();
            const data = String(text ?? '') + (shouldExecute === false ? '' : '\r');
            try {
                this._pty.handleInput?.(data);
            } catch (err) {
                log(`terminal pty input failed for ${this.name}: ${err?.message || err}`);
            }
            return;
        }
        if (shouldExecute === false) {
            send({
                type: 'terminal_input',
                id: this._handle,
                name: this.name,
                text: String(text ?? ''),
                metadata: this.metadata(),
            });
            return;
        }
        send({
            type: 'terminal_command',
            id: this._handle,
            name: this.name,
            text: String(text ?? ''),
            shouldExecute: shouldExecute !== false,
            metadata: this.metadata(),
        });
    }
    metadata() {
        const options = this.creationOptions || {};
        return {
            id: this._handle,
            name: this.name,
            cwd: _terminalOptionValue(options.cwd),
            env: _terminalOptionValue(options.env),
            shellPath: _terminalOptionValue(options.shellPath),
            shellArgs: _terminalOptionValue(options.shellArgs),
            message: _terminalOptionValue(options.message),
            location: _terminalOptionValue(options.location),
            iconPath: _terminalOptionValue(options.iconPath),
            color: _terminalOptionValue(options.color),
            shellIntegrationNonce: !!options.shellIntegrationNonce,
            isTransient: options.isTransient === true,
            strictEnv: options.strictEnv === true,
            hideFromUser: options.hideFromUser === true,
            isPseudoterminal: !!this._pty,
            shellIntegration: !!this.shellIntegration,
            state: _terminalOptionValue(this.state),
            dimensions: _terminalOptionValue(this.dimensions),
        };
    }
    show(preserveFocus = false) {
        if (this._disposed) return;
        if (!preserveFocus) _terminalSetActive(this);
        this._ensureShellIntegration();
        this._openPty();
        send({
            type: 'terminal_show',
            id: this._handle,
            name: this.name,
            preserveFocus: !!preserveFocus,
            metadata: this.metadata(),
        });
    }
    hide() {
        if (this._disposed) return;
        send({ type: 'terminal_hide', id: this._handle, name: this.name });
    }
    dispose() {
        if (this._disposed) return;
        this._disposed = true;
        this.exitStatus = { code: undefined, reason: 4 };
        if (this._pty) {
            try { this._pty.close?.(); } catch {}
            this._disposePtySubscriptions();
        }
        send({ type: 'terminal_dispose', id: this._handle, name: this.name });
        _terminalRemove(this);
    }
}

function _terminalShellCommandLine(commandLineOrExecutable, args) {
    let value = String(commandLineOrExecutable ?? '');
    if (Array.isArray(args)) {
        for (const rawArg of args) {
            const arg = String(rawArg ?? '');
            value += /\s/.test(arg) && !/["'`]/.test(arg)
                ? ` "${arg}"`
                : ` ${arg}`;
        }
    }
    return value;
}

class TerminalShellExecution {
    constructor(terminal, commandLine) {
        this._handle = _nextTerminalShellExecutionHandle++;
        this.terminal = terminal;
        this.commandLine = {
            value: String(commandLine || ''),
            confidence: 2,
            isTrusted: true,
        };
        this.cwd = terminal?._shellIntegrationObject?.cwd;
        this._chunks = [];
    }
    _append(data) {
        this._chunks.push(String(data ?? ''));
    }
    async *read() {
        for (const chunk of this._chunks) {
            yield chunk;
        }
    }
}

class TerminalShellIntegration {
    constructor(terminal) {
        this._terminal = terminal;
        const rawCwd = terminal.creationOptions?.cwd;
        this.cwd = rawCwd instanceof Uri
            ? rawCwd
            : (typeof rawCwd === 'string' && rawCwd ? Uri.file(rawCwd) : undefined);
        const envValue = terminal._resolvedEnv;
        this.env = envValue && typeof envValue === 'object'
            ? Object.freeze({ isTrusted: true, value: Object.freeze(Object.assign({}, envValue)) })
            : undefined;
        const self = this;
        this.value = {
            get cwd() { return self.cwd; },
            get env() { return self.env; },
            executeCommand(commandLineOrExecutable, args) {
                return self.executeCommand(commandLineOrExecutable, args);
            },
        };
    }
    cwdPayload() {
        return _plainBridgeValue(this.cwd);
    }
    envPayload() {
        const resolvedEnv = _terminalOptionValue(this._terminal._resolvedEnv);
        return resolvedEnv && typeof resolvedEnv === 'object'
            ? { isTrusted: true, value: resolvedEnv }
            : undefined;
    }
    executeCommand(commandLineOrExecutable, args) {
        const commandLine = _terminalShellCommandLine(commandLineOrExecutable, args);
        const execution = new TerminalShellExecution(this._terminal, commandLine);
        execution._append(commandLine + '\n');
        this._terminal._trackShellExecution(execution);
        const startEvent = {
            terminal: this._terminal,
            shellIntegration: this.value,
            execution,
        };
        _onDidStartTerminalShellExecutionEmitter.fire(startEvent);
        send({
            type: 'terminal_shell_execution_start',
            terminal: _terminalBridgePayload(this._terminal),
            execution: {
                id: execution._handle,
                commandLine: execution.commandLine,
                cwd: _plainBridgeValue(execution.cwd),
            },
        });
        this._terminal.sendText(commandLine, true);
        setTimeout(() => {
            const endEvent = {
                terminal: this._terminal,
                shellIntegration: this.value,
                execution,
                exitCode: undefined,
            };
            _onDidEndTerminalShellExecutionEmitter.fire(endEvent);
            this._terminal._finishShellExecution(execution);
            send({
                type: 'terminal_shell_execution_end',
                terminal: _terminalBridgePayload(this._terminal),
                execution: {
                    id: execution._handle,
                    commandLine: execution.commandLine,
                    cwd: _plainBridgeValue(execution.cwd),
                },
                exitCode: null,
            });
        }, 0);
        return execution;
    }
}

class TerminalProfile {
    constructor(options) {
        this.options = options && typeof options === 'object'
            ? options
            : {};
    }
}

class TerminalLink {
    constructor(startIndex, length, tooltip) {
        this.startIndex = Math.max(0, Number(startIndex || 0));
        this.length = Math.max(0, Number(length || 0));
        if (tooltip !== undefined && tooltip !== null) {
            this.tooltip = String(tooltip);
        }
    }
}

function _createTerminal(nameOrOptions, shellPath, shellArgs) {
    const terminal = new TerminalObject(
        _terminalOptionsFromArgs(nameOrOptions, shellPath, shellArgs));
    _terminals.push(terminal);
    _terminalSetActive(terminal);
    _onDidOpenTerminalEmitter.fire(terminal);
    terminal._openPty();
    return terminal;
}

async function _createTerminalFromProfileProvider(profileId, options) {
    const id = String(profileId || '');
    if (id) await _activateKnownExtensionsForEvent(`onTerminalProfile:${id}`);
    const entry = _terminalProfileProviders.get(id);
    if (!entry) {
        throw new Error(`No terminal profile provider registered for id "${id}"`);
    }
    const tokenSource = new CancellationTokenSource();
    let profile = await entry.provider.provideTerminalProfile(tokenSource.token);
    if (tokenSource.token.isCancellationRequested) {
        return { cancelled: true };
    }
    if (profile && typeof profile === 'object' && !Object.prototype.hasOwnProperty.call(profile, 'options')) {
        profile = new TerminalProfile(profile);
    }
    if (!profile || typeof profile !== 'object' || !profile.options) {
        throw new Error(`No terminal profile options provided for id "${id}"`);
    }
    const terminalOptions = _terminalMergeOptions(
        profile.options || {},
        options && typeof options === 'object' ? options : {});
    const terminal = _createTerminal(terminalOptions);
    terminal.show(!!(options && options.preserveFocus));
    return {
        cancelled: false,
        profile: _plainBridgeValue({ options: profile.options || {} }),
        terminal: Object.assign({ id: terminal._handle }, terminal.metadata()),
    };
}

function _disposeTerminalById(id) {
    const handle = Number(id);
    if (!Number.isFinite(handle)) return false;
    const terminal = _terminals.find(item => item && item._handle === handle);
    if (!terminal) return false;
    terminal.dispose();
    return true;
}

function _terminalForLinkRequest(msg) {
    const id = Number(msg.terminalId ?? msg.id ?? msg.handle);
    if (Number.isFinite(id)) {
        const byId = _terminals.find(terminal => terminal._handle === id);
        if (byId) return byId;
    }
    const name = String(msg.name || msg.terminalName || '');
    if (name) {
        const byName = _terminals.find(terminal => terminal.name === name);
        if (byName) return byName;
    }
    return _activeTerminal;
}

function _terminalLinkPayload(provider, rawLink, terminalHandle) {
    if (!rawLink || typeof rawLink !== 'object') return null;
    const startIndex = Math.max(0, Number(rawLink.startIndex || 0));
    const length = Math.max(0, Number(rawLink.length || 0));
    if (!length) return null;
    const link = rawLink instanceof TerminalLink
        ? rawLink
        : new TerminalLink(startIndex, length, rawLink.tooltip);
    const id = _nextTerminalLinkHandle++;
    let cache = _terminalLinkCache.get(terminalHandle);
    if (!cache) {
        cache = new Map();
        _terminalLinkCache.set(terminalHandle, cache);
    }
    cache.set(id, { provider, link });
    return {
        id,
        startIndex: link.startIndex,
        length: link.length,
        tooltip: link.tooltip || '',
        label: link.tooltip || '',
    };
}

function _normalizeFileSystemScheme(scheme) {
    return String(scheme || '').trim().toLowerCase();
}

const FileChangeType = {
    Changed: 1,
    Created: 2,
    Deleted: 3,
};

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

function _fileSystemProviderParentUri(uri) {
    const normalized = uri instanceof Uri ? uri : _workspaceUriFromInput(uri);
    const parentPath = path.posix.dirname(normalized.path || '/');
    return normalized.with({ path: parentPath || '/' });
}

function _fileSystemProviderReadonly(entry) {
    return !!(entry && entry.options && entry.options.isReadonly);
}

function _fileSystemProviderHasEvents(entry) {
    return typeof (entry && entry.provider && entry.provider.onDidChangeFile) === 'function';
}

function _ensureFileSystemProviderWritable(entry, uri) {
    if (_fileSystemProviderReadonly(entry)) {
        throw FileSystemError.NoPermissions(uri);
    }
}

function _providerFileSystemError(error, uri) {
    if (error instanceof FileSystemError) return error;
    if (error && error.code && String(error.name || '') === 'FileSystemError') {
        return error;
    }
    const message = error && error.message ? error.message : uri;
    const text = String(message || '').toLowerCase();
    if (text.includes('not found') || text.includes('missing')) {
        return FileSystemError.FileNotFound(message);
    }
    if (text.includes('exists')) return FileSystemError.FileExists(message);
    if (text.includes('permission') || text.includes('readonly') || text.includes('read-only')) {
        return FileSystemError.NoPermissions(message);
    }
    return new FileSystemError(message, error && error.code ? error.code : 'Unknown');
}

async function _withProviderFileSystemErrors(uri, fn) {
    try {
        return await fn();
    } catch (error) {
        throw _providerFileSystemError(error, uri);
    }
}

async function _callFileSystemProvider(uri, methodNames, args, fallback) {
    const entry = await _fileSystemProviderForUri(uri);
    if (!entry) return fallback();
    for (const name of methodNames) {
        const method = entry.provider && entry.provider[name];
        if (typeof method === 'function') {
            return await _withProviderFileSystemErrors(
                uri,
                () => method.apply(entry.provider, args));
        }
    }
    throw new Error(`Filesystem provider for ${uri.scheme} is missing ${methodNames[0]}`);
}

async function _workspaceFsProviderMkdirp(entry, targetUri) {
    const provider = entry && entry.provider;
    if (!provider || typeof provider.createDirectory !== 'function') return;
    let directory = targetUri instanceof Uri ? targetUri : _workspaceUriFromInput(targetUri);
    const pending = [];
    while (directory.path && directory.path !== '/' && directory.path !== '.') {
        try {
            const stat = await _withProviderFileSystemErrors(
                directory,
                () => provider.stat(directory));
            if (!(Number(stat && stat.type) & 2)) {
                throw FileSystemError.FileExists(directory);
            }
            break;
        } catch (error) {
            if (!(error instanceof FileSystemError && error.code === 'FileNotFound')) {
                throw error;
            }
            pending.push(directory);
            const parent = _fileSystemProviderParentUri(directory);
            if (parent.toString() === directory.toString()) break;
            directory = parent;
        }
    }
    for (const uri of pending.reverse()) {
        await _withProviderFileSystemErrors(
            uri,
            () => provider.createDirectory(uri));
    }
}

function _normalizeFileSystemProviderEvents(events) {
    const rawEvents = Array.isArray(events) ? events : [events];
    return rawEvents
        .filter(Boolean)
        .map((event) => ({
            uri: _workspaceUriFromInput(event.uri || event.resource || event).toString(),
            type: Number(event.type || FileChangeType.Changed),
        }));
}

function _subscribeFileSystemProviderEvents(entry) {
    const provider = entry && entry.provider;
    const eventSource = provider && provider.onDidChangeFile;
    if (typeof eventSource !== 'function') return null;
    return eventSource.call(provider, (events) => {
        const normalizedEvents = _normalizeFileSystemProviderEvents(events);
        if (!normalizedEvents.length) return;
        entry.lastEvents = normalizedEvents;
        _workspaceFireFileWatcherEvents(normalizedEvents);
        send({
            type: 'filesystem_changed',
            scheme: entry.scheme,
            extensionId: entry.extensionId || '',
            events: normalizedEvents,
        });
    });
}

function _nodeFileSystemError(error, uri) {
    if (error instanceof FileSystemError) return error;
    const code = String(error?.code || '');
    if (code === 'ENOENT') return FileSystemError.FileNotFound(uri);
    if (code === 'EEXIST' || code === 'ERR_FS_CP_EEXIST') return FileSystemError.FileExists(uri);
    if (code === 'ENOTDIR') return FileSystemError.FileNotADirectory(uri);
    if (code === 'EISDIR' || code === 'ERR_FS_EISDIR') return FileSystemError.FileIsADirectory(uri);
    if (code === 'EACCES' || code === 'EPERM') return FileSystemError.NoPermissions(uri);
    return new FileSystemError(error?.message || uri, 'Unknown');
}

async function _withFileSystemErrors(uri, fn) {
    try {
        return await fn();
    } catch (error) {
        throw _nodeFileSystemError(error, uri);
    }
}

async function _workspaceFsTargetExists(uri) {
    const entry = await _fileSystemProviderForUri(uri);
    if (entry) {
        try {
            await _workspaceFsStat(uri);
            return true;
        } catch (error) {
            if (error instanceof FileSystemError && error.code === 'FileNotFound') return false;
            throw error;
        }
    }
    try {
        await fsp.lstat(uri.fsPath);
        return true;
    } catch (error) {
        if (String(error?.code || '') === 'ENOENT') return false;
        throw error;
    }
}

async function _workspaceFsReadFile(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const value = await _callFileSystemProvider(
        uri,
        ['readFile', 'read_file'],
        [uri],
        () => _withFileSystemErrors(uri, () => fsp.readFile(uri.fsPath)),
    );
    return Uint8Array.from(_contentToUint8Array(value));
}

async function _workspaceFsWriteFile(uriInput, content) {
    const uri = _workspaceUriFromInput(uriInput);
    const bytes = _contentToUint8Array(content);
    const entry = await _fileSystemProviderForUri(uri);
    const existed = await _workspaceFsTargetExists(uri).catch(error => {
        if (error instanceof FileSystemError && error.code === 'FileNotFound') return false;
        throw error;
    });
    if (entry) {
        _ensureFileSystemProviderWritable(entry, uri);
        await _workspaceFsProviderMkdirp(entry, _fileSystemProviderParentUri(uri));
    }
    const result = await _callFileSystemProvider(
        uri,
        ['writeFile', 'write_file'],
        [uri, bytes, { create: true, overwrite: true }],
        async () => {
            await _withFileSystemErrors(
                uri,
                () => fsp.mkdir(path.dirname(uri.fsPath), { recursive: true }));
            await _withFileSystemErrors(uri, () => fsp.writeFile(uri.fsPath, bytes));
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
    if (!entry || !_fileSystemProviderHasEvents(entry)) {
        _workspaceFireFileWatchers(uri, existed ? FileChangeType.Changed : FileChangeType.Created);
    }
    return result;
}

async function _workspaceFsStat(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const value = await _callFileSystemProvider(
        uri,
        ['stat'],
        [uri],
        async () => {
            const s = await _withFileSystemErrors(uri, () => fsp.stat(uri.fsPath));
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
        () => _withFileSystemErrors(
            uri,
            () => fsp.readdir(uri.fsPath, { withFileTypes: true }))
            .then(ents => ents.map(e => [e.name, e.isDirectory() ? 2 : 1])),
    );
}

async function _workspaceFsCreateDirectory(uriInput) {
    const uri = _workspaceUriFromInput(uriInput);
    const entry = await _fileSystemProviderForUri(uri);
    const existed = await _workspaceFsTargetExists(uri).catch(error => {
        if (error instanceof FileSystemError && error.code === 'FileNotFound') return false;
        throw error;
    });
    if (entry) {
        _ensureFileSystemProviderWritable(entry, uri);
        const result = await _workspaceFsProviderMkdirp(entry, uri);
        if (!existed && !_fileSystemProviderHasEvents(entry)) {
            _workspaceFireFileWatchers(uri, FileChangeType.Created);
        }
        return result;
    }
    const result = await _callFileSystemProvider(
        uri,
        ['createDirectory', 'create_directory'],
        [uri],
        () => _withFileSystemErrors(uri, () => fsp.mkdir(uri.fsPath, { recursive: true })),
    );
    if (!existed) _workspaceFireFileWatchers(uri, FileChangeType.Created);
    return result;
}

async function _workspaceFsDelete(uriInput, options) {
    const uri = _workspaceUriFromInput(uriInput);
    const entry = await _fileSystemProviderForUri(uri);
    if (entry) _ensureFileSystemProviderWritable(entry, uri);
    const result = await _callFileSystemProvider(
        uri,
        ['delete', 'deleteFile', 'delete_file'],
        [uri, options || {}],
        async () => {
            await _withFileSystemErrors(
                uri,
                () => fsp.rm(uri.fsPath, {
                    recursive: !!(options && options.recursive),
                    force: false,
                }),
            );
            _workspaceCloseTextDocument(uri);
        },
    );
    if (!entry || !_fileSystemProviderHasEvents(entry)) {
        _workspaceFireFileWatchers(uri, FileChangeType.Deleted);
    }
    return result;
}

async function _workspaceFsRename(srcInput, dstInput, options) {
    const src = _workspaceUriFromInput(srcInput);
    const dst = _workspaceUriFromInput(dstInput);
    if (src.scheme !== dst.scheme) {
        await _workspaceFsCopy(src, dst, options || {});
        await _workspaceFsDelete(src, {});
        return;
    }
    const entry = await _fileSystemProviderForUri(src);
    if (entry) {
        _ensureFileSystemProviderWritable(entry, src);
        _ensureFileSystemProviderWritable(entry, dst);
    }
    const dstExisted = await _workspaceFsTargetExists(dst).catch(error => {
        if (error instanceof FileSystemError && error.code === 'FileNotFound') return false;
        throw error;
    });
    const result = await _callFileSystemProvider(
        src,
        ['rename', 'renameFile', 'rename_file'],
        [src, dst, options || {}],
        async () => {
            const overwrite = !!(options && options.overwrite);
            if (path.resolve(src.fsPath) === path.resolve(dst.fsPath)) return;
            await _withFileSystemErrors(
                dst,
                () => fsp.mkdir(path.dirname(dst.fsPath), { recursive: true }));
            await _withFileSystemErrors(src, async () => {
                if (await _workspaceFsTargetExists(dst)) {
                    if (!overwrite) throw FileSystemError.FileExists(dst);
                    await fsp.rm(dst.fsPath, { recursive: true, force: false });
                }
                await fsp.rename(src.fsPath, dst.fsPath);
            });
            const cached = _workspaceCloseTextDocument(src);
            if (cached) {
                const text = await _withFileSystemErrors(dst, () => fsp.readFile(dst.fsPath, 'utf8'));
                _workspaceStoreTextDocument(_createLanguageDocument({
                    uri: dst,
                    text,
                    languageId: _languageIdForUri(dst),
                    version: Date.now(),
                }), true);
            }
        },
    );
    if (!entry || !_fileSystemProviderHasEvents(entry)) {
        _workspaceFireFileWatchers(src, FileChangeType.Deleted);
        _workspaceFireFileWatchers(dst, dstExisted ? FileChangeType.Changed : FileChangeType.Created);
    }
    return result;
}

async function _workspaceFsCopy(srcInput, dstInput, options) {
    const src = _workspaceUriFromInput(srcInput);
    const dst = _workspaceUriFromInput(dstInput);
    if (src.scheme !== dst.scheme) {
        const overwrite = !!(options && options.overwrite);
        const dstEntry = await _fileSystemProviderForUri(dst);
        if (dstEntry) _ensureFileSystemProviderWritable(dstEntry, dst);
        if (!overwrite && await _workspaceFsTargetExists(dst)) {
            throw FileSystemError.FileExists(dst);
        }
        const content = await _workspaceFsReadFile(src);
        await _workspaceFsWriteFile(dst, content);
        return;
    }
    const entry = await _fileSystemProviderForUri(src);
    if (entry) {
        _ensureFileSystemProviderWritable(entry, dst);
        const copy = entry.provider && entry.provider.copy;
        if (typeof copy === 'function') {
            const dstExisted = await _workspaceFsTargetExists(dst).catch(error => {
                if (error instanceof FileSystemError && error.code === 'FileNotFound') return false;
                throw error;
            });
            const result = await _withProviderFileSystemErrors(
                src,
                () => copy.call(entry.provider, src, dst, options || {}));
            if (!_fileSystemProviderHasEvents(entry)) {
                _workspaceFireFileWatchers(dst, dstExisted ? FileChangeType.Changed : FileChangeType.Created);
            }
            return result;
        }
        if (!options?.overwrite) {
            try {
                await _workspaceFsStat(dst);
                throw FileSystemError.FileExists(dst);
            } catch (error) {
                if (error instanceof FileSystemError && error.code === 'FileExists') throw error;
                if (!(error instanceof FileSystemError && error.code === 'FileNotFound')) throw error;
            }
        }
        const content = await _workspaceFsReadFile(src);
        return _workspaceFsWriteFile(dst, content);
    }
    const dstExisted = await _workspaceFsTargetExists(dst).catch(error => {
        if (error instanceof FileSystemError && error.code === 'FileNotFound') return false;
        throw error;
    });
    return _withFileSystemErrors(src, async () => {
        const overwrite = !!(options && options.overwrite);
        if (dstExisted) {
            if (!overwrite) throw FileSystemError.FileExists(dst);
            await fsp.rm(dst.fsPath, { recursive: true, force: false });
        }
        await fsp.mkdir(path.dirname(dst.fsPath), { recursive: true });
        const stat = await fsp.stat(src.fsPath);
        if (stat.isDirectory()) {
            await fsp.cp(src.fsPath, dst.fsPath, { recursive: true, force: overwrite });
        } else {
            await fsp.copyFile(src.fsPath, dst.fsPath);
        }
        _workspaceFireFileWatchers(dst, dstExisted ? FileChangeType.Changed : FileChangeType.Created);
    });
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

function _workspacePatternBaseUri(pattern) {
    if (!pattern || typeof pattern !== 'object') return Uri.file(_workspaceRoot);
    const base = pattern.baseUri || pattern.base || pattern.uri;
    if (!base) return Uri.file(_workspaceRoot);
    return _workspaceUriFromInput(base);
}

function _workspaceUriMatchTexts(uri, pattern) {
    const normalized = uri instanceof Uri ? uri : _workspaceUriFromInput(uri);
    const texts = new Set();
    if (pattern && typeof pattern === 'object') {
        const baseUri = _workspacePatternBaseUri(pattern);
        if (!baseUri || baseUri.scheme !== normalized.scheme
            || String(baseUri.authority || '') !== String(normalized.authority || '')) {
            return [];
        }
        if (normalized.scheme === 'file') {
            const rel = path.relative(baseUri.fsPath, normalized.fsPath).replace(/\\/g, '/');
            if (rel && !rel.startsWith('..') && !path.isAbsolute(rel)) texts.add(rel);
        } else {
            const basePath = String(baseUri.path || '/').replace(/\/+$/, '');
            const uriPath = String(normalized.path || '');
            if (!basePath || uriPath === basePath || uriPath.startsWith(basePath + '/')) {
                const rel = uriPath.slice(basePath.length).replace(/^\/+/, '');
                if (rel) texts.add(rel);
            }
        }
        return Array.from(texts).filter(Boolean);
    }
    const pathText = String(normalized.path || '').replace(/^\/+/, '');
    if (pathText) texts.add(pathText);
    if (normalized.scheme === 'file') {
        const rel = path.relative(_workspaceRoot, normalized.fsPath).replace(/\\/g, '/');
        if (rel && !rel.startsWith('..') && !path.isAbsolute(rel)) texts.add(rel);
        texts.add(path.basename(normalized.fsPath));
    } else {
        const parts = String(normalized.path || '').split('/').filter(Boolean);
        if (parts.length) texts.add(parts[parts.length - 1]);
    }
    return Array.from(texts).filter(Boolean);
}

function _workspaceCreateFileSystemWatcher(pattern, optionsOrIgnoreCreate, ignoreChange, ignoreDelete) {
    const options = optionsOrIgnoreCreate && typeof optionsOrIgnoreCreate === 'object'
        ? {
            ignoreCreateEvents: !!optionsOrIgnoreCreate.ignoreCreateEvents,
            ignoreChangeEvents: !!optionsOrIgnoreCreate.ignoreChangeEvents,
            ignoreDeleteEvents: !!optionsOrIgnoreCreate.ignoreDeleteEvents,
        }
        : {
            ignoreCreateEvents: !!optionsOrIgnoreCreate,
            ignoreChangeEvents: !!ignoreChange,
            ignoreDeleteEvents: !!ignoreDelete,
        };
    const createEmitter = new EventEmitter();
    const changeEmitter = new EventEmitter();
    const deleteEmitter = new EventEmitter();
    const record = {
        pattern,
        regex: _globToRegExp(_workspacePatternText(pattern)),
        options,
        createEmitter,
        changeEmitter,
        deleteEmitter,
        disposed: false,
    };
    const watcher = {
        get ignoreCreateEvents() { return record.options.ignoreCreateEvents; },
        get ignoreChangeEvents() { return record.options.ignoreChangeEvents; },
        get ignoreDeleteEvents() { return record.options.ignoreDeleteEvents; },
        onDidCreate: createEmitter.event,
        onDidChange: changeEmitter.event,
        onDidDelete: deleteEmitter.event,
        dispose() {
            if (record.disposed) return;
            record.disposed = true;
            _fileSystemWatchers.delete(record);
            createEmitter.dispose();
            changeEmitter.dispose();
            deleteEmitter.dispose();
        },
    };
    record.watcher = watcher;
    _fileSystemWatchers.add(record);
    return watcher;
}

function _workspaceWatcherMatches(record, uri) {
    if (!record || record.disposed) return false;
    return _workspaceUriMatchTexts(uri, record.pattern).some(text => record.regex.test(text));
}

function _workspaceFireFileWatchers(uriInput, type) {
    if (!_fileSystemWatchers.size) return;
    const uri = uriInput instanceof Uri ? uriInput : _workspaceUriFromInput(uriInput);
    for (const record of Array.from(_fileSystemWatchers)) {
        if (!_workspaceWatcherMatches(record, uri)) continue;
        if (type === FileChangeType.Created && !record.options.ignoreCreateEvents) {
            record.createEmitter.fire(uri);
        } else if (type === FileChangeType.Changed && !record.options.ignoreChangeEvents) {
            record.changeEmitter.fire(uri);
        } else if (type === FileChangeType.Deleted && !record.options.ignoreDeleteEvents) {
            record.deleteEmitter.fire(uri);
        }
    }
}

function _workspaceFireFileWatcherEvents(events) {
    for (const event of events || []) {
        _workspaceFireFileWatchers(event.uri, Number(event.type || FileChangeType.Changed));
    }
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
        const opened = _workspaceStoreTextDocument(doc, true);
        await _activateKnownExtensionsForDocumentLanguage(opened);
        return opened;
    }
    const uri = _workspaceUriFromInput(uriOrPath);
    const contentEntry = _textDocumentContentProviders.get(
        _normalizeFileSystemScheme(uri.scheme));
    if (contentEntry) {
        return _workspaceOpenTextDocumentFromContentProvider(
            uri, contentEntry, true);
    }
    const cached = _workspaceTextDocuments.get(uri.toString());
    if (cached) {
        const opened = _workspacePromoteTextDocument(cached);
        await _activateKnownExtensionsForDocumentLanguage(opened);
        return opened;
    }
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
    const opened = _workspaceStoreTextDocument(doc, true);
    await _activateKnownExtensionsForDocumentLanguage(opened);
    return opened;
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
        const opened = fireOpen ? _workspacePromoteTextDocument(existing) : existing;
        if (fireOpen) await _activateKnownExtensionsForDocumentLanguage(opened);
        return opened;
    }
    const doc = _createLanguageDocument({
        uri,
        text,
        languageId: _languageIdForUri(uri),
        version: Date.now(),
    });
    doc.isDirty = false;
    doc.__contentProviderScheme = _normalizeFileSystemScheme(uri.scheme);
    const opened = _workspaceStoreTextDocument(doc, fireOpen);
    if (fireOpen) await _activateKnownExtensionsForDocumentLanguage(opened);
    return opened;
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
    if (entry._notebookEdit || entry.kind === 'notebook') return 'notebook';
    const kind = String(entry.kind || entry.type || entry.operation || '').toLowerCase();
    if (kind === 'createfile' || kind === 'create') return 'create';
    if (kind === 'deletefile' || kind === 'delete') return 'delete';
    if (kind === 'renamefile' || kind === 'rename') return 'rename';
    if (kind === 'notebook' || kind === 'notebookedit') return 'notebook';
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
    const editor = _visibleTextEditors.get(key);
    if (editor) {
        _visibleTextEditors.delete(key);
        if (_activeTextEditor === editor) {
            _activeTextEditor = undefined;
            _onDidChangeActiveTextEditorEmitter.fire(undefined);
        }
        _fireVisibleTextEditorsChanged('close', editor);
    }
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

function _notebookDocumentForEdit(uri) {
    if (!uri) return undefined;
    const key = uri.toString();
    const notebook = _notebookDocuments.get(key);
    return notebook && !notebook.isClosed ? notebook : undefined;
}

function _notebookRangeFromEdit(entry, notebook) {
    const cellCount = Math.max(0, Number(notebook?.cellCount || 0));
    const raw = entry?.range;
    const start = Math.max(0, Math.min(cellCount, Number(raw?.start ?? raw?.from ?? 0)));
    const end = Math.max(start, Math.min(cellCount, Number(raw?.end ?? raw?.to ?? start)));
    return { start, end };
}

function _workspaceApplyNotebookEdit(entry) {
    const uri = _workspaceEditUri(entry);
    const notebook = _notebookDocumentForEdit(uri);
    if (!notebook) return false;
    const event = { cells: [], cellChanges: [] };
    if (entry.newNotebookMetadata !== undefined || entry.metadata !== undefined) {
        const metadata = entry.newNotebookMetadata !== undefined
            ? entry.newNotebookMetadata
            : entry.metadata;
        notebook.metadata = metadata && typeof metadata === 'object' ? metadata : {};
        notebook._data.metadata = notebook.metadata;
        event.metadata = notebook.metadata;
    }
    if (entry.index !== undefined || entry.cellIndex !== undefined) {
        const index = Number(entry.index ?? entry.cellIndex);
        if (!Number.isFinite(index) || index < 0 || index >= notebook._data.cells.length) {
            return false;
        }
        const metadata = entry.newCellMetadata !== undefined
            ? entry.newCellMetadata
            : (entry.cellMetadata || {});
        notebook._data.cells[index].metadata = metadata && typeof metadata === 'object'
            ? metadata
            : {};
        const cell = notebook.cellAt(index);
        event.cellChanges.push({ cell, metadata: cell.metadata });
    }
    if (Array.isArray(entry.newCells)) {
        const range = _notebookRangeFromEdit(entry, notebook);
        const newCells = entry.newCells.map(_notebookCellDataClone);
        notebook._data.cells.splice(range.start, range.end - range.start, ...newCells);
        event.cells.push({
            start: range.start,
            deletedCount: range.end - range.start,
            deletedItems: [],
            items: notebook.getCells(new NotebookRange(
                range.start, range.start + newCells.length)),
        });
    }
    _fireNotebookChange(notebook, event);
    return true;
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
        if (kind === 'notebook') {
            if (!(await flushTextEdits())) return false;
            if (!_workspaceApplyNotebookEdit(entry)) return false;
            continue;
        }
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
        if (!uri || !entry.range) return false;
        const key = uri.toString();
        if (uri.scheme !== 'file' && !_workspaceTextDocuments.has(key)) {
            return false;
        }
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
    let docUri = document.uri;
    const docScheme = (docUri && docUri.scheme) || 'file';
    const notebook = document.notebook || {};
    const notebookType = document.notebookType ?? notebook.notebookType ?? notebook.type;
    const notebookUri = document.notebookUri ?? notebook.uri;
    const selectors = Array.isArray(selector) ? selector : [selector];
    let best = 0;
    for (const sel of selectors) {
        let score = 0;
        let candidateUri = docUri;
        if (typeof sel === 'string') {
            score = sel === '*' ? 5 : (sel === docLang ? 10 : 0);
        } else if (typeof sel === 'object' && sel !== null) {
            if (sel.scheme) {
                if (sel.scheme === docScheme) score = 10;
                else if (sel.scheme === '*') score = Math.max(score, 5);
                else continue;
            }
            if (sel.language) {
                if (sel.language === docLang) score = 10;
                else if (sel.language === '*') score = Math.max(score, 5);
                else continue;
            }
            if (sel.notebookType) {
                if (sel.notebookType === notebookType) {
                    score = 10;
                    if (notebookUri) candidateUri = notebookUri;
                } else if (sel.notebookType === '*' && notebookType !== undefined && notebookType !== null) {
                    score = Math.max(score, 5);
                    if (notebookUri) candidateUri = notebookUri;
                } else {
                    continue;
                }
            }
            const pattern = sel.pattern === undefined ? sel.filenamePattern : sel.pattern;
            if (pattern) {
                if (_documentSelectorPatternMatches(pattern, candidateUri)) score = 10;
                else continue;
            }
        }
        best = Math.max(best, score);
    }
    return best;
}

function _documentSelectorPatternMatches(pattern, uri) {
    if (!pattern || !uri) return false;
    const rawPattern = _workspacePatternText(pattern, '');
    if (!rawPattern) return false;
    const normalizedPath = String(uri.fsPath || uri.path || uri.toString?.() || '').replace(/\\/g, '/');
    const basePath = pattern && typeof pattern === 'object'
        ? String(_pathFromUriLike(pattern.baseUri || pattern.base || pattern.uri) || '').replace(/\\/g, '/')
        : '';
    let relativePath = '';
    if (basePath && normalizedPath) {
        relativePath = path.relative(basePath, normalizedPath.replace(/\//g, path.sep)).replace(/\\/g, '/');
        if (relativePath.startsWith('../') || relativePath === '..' || path.isAbsolute(relativePath)) {
            relativePath = '';
        }
    }
    const candidates = new Set([
        normalizedPath,
        String(uri.path || '').replace(/\\/g, '/'),
        relativePath,
        path.posix.basename(normalizedPath),
    ].filter(Boolean));
    const regex = _globToRegExp(rawPattern);
    for (const candidate of candidates) {
        if (candidate === rawPattern || regex.test(candidate)) return true;
    }
    return false;
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

function _normalizeTreeViewText(value) {
    return value === undefined || value === null ? '' : String(value);
}

function _normalizeTreeViewBadge(value) {
    if (value === undefined || value === null || value === '') return undefined;
    const rawValue = Number(value.value);
    if (!Number.isFinite(rawValue)) return undefined;
    return {
        value: Math.trunc(rawValue),
        tooltip: value.tooltip === undefined || value.tooltip === null
            ? ''
            : String(value.tooltip),
    };
}

function _treeDndMimeTypes(value) {
    if (typeof value === 'string') return value ? [value] : [];
    if (!Array.isArray(value)) return [];
    return value.map(item => String(item || '')).filter(Boolean);
}

function _treeDragDropPayload(controller) {
    if (!controller || typeof controller !== 'object') return undefined;
    return {
        dragMimeTypes: _treeDndMimeTypes(controller.dragMimeTypes),
        dropMimeTypes: _treeDndMimeTypes(controller.dropMimeTypes),
        canDrag: typeof controller.handleDrag === 'function',
        canDrop: typeof controller.handleDrop === 'function',
    };
}

function _sendTreeDragDropControllerRegistered(viewId, controller) {
    const payload = _treeDragDropPayload(controller);
    if (!payload) return;
    send({
        type: 'tree_drag_drop_controller_registered',
        viewId,
        ...payload,
    });
}

function createTreeViewObject(viewId, treeDataProvider, dragAndDropController) {
    const normalized = String(viewId || '');
    const expandEmitter = new EventEmitter();
    const collapseEmitter = new EventEmitter();
    const selectionEmitter = new EventEmitter();
    const visibilityEmitter = new EventEmitter();
    const checkboxEmitter = new EventEmitter();
    let visible = true;
    let selection = [];
    let message = '';
    let title = '';
    let description = '';
    let badge = undefined;
    const setVisibleFromHost = (nextVisible) => {
        const normalizedVisible = !!nextVisible;
        if (visible === normalizedVisible) return;
        visible = normalizedVisible;
        visibilityEmitter.fire({ visible });
    };
    const sendStateChanged = (reason) => {
        send({
            type: 'tree_view_state_changed',
            viewId: normalized,
            reason: String(reason || ''),
            state: {
                visible,
                message,
                title,
                description,
                badge: badge === undefined ? null : _serializeLanguageValue(badge),
            },
        });
    };
    const providerDisposable = treeDataProvider
        ? registerTreeDataProviderInternal(normalized, treeDataProvider)
        : undefined;
    const dndController = dragAndDropController || undefined;
    const view = {
        id: normalized,
        onDidExpandElement: expandEmitter.event,
        onDidCollapseElement: collapseEmitter.event,
        onDidChangeSelection: selectionEmitter.event,
        onDidChangeVisibility: visibilityEmitter.event,
        onDidChangeCheckboxState: checkboxEmitter.event,
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
            visible = false;
            visibilityEmitter.fire({ visible: false });
            if (dndController) {
                send({
                    type: 'tree_drag_drop_controller_disposed',
                    viewId: normalized,
                });
            }
            sendStateChanged('visible');
        },
        _onDidExpandElement: expandEmitter,
        _onDidCollapseElement: collapseEmitter,
        _onDidChangeSelection: selectionEmitter,
        _onDidChangeVisibility: visibilityEmitter,
        _onDidChangeCheckboxState: checkboxEmitter,
        _dragAndDropController: dndController,
        _setVisibleFromHost: setVisibleFromHost,
    };
    Object.defineProperties(view, {
        visible: {
            enumerable: true,
            get: () => visible,
        },
        selection: {
            enumerable: true,
            get: () => selection.slice(),
            set: (value) => {
                selection = Array.isArray(value) ? value.slice() : [];
            },
        },
        message: {
            enumerable: true,
            get: () => message,
            set: (value) => {
                const next = _normalizeTreeViewText(value);
                if (next === message) return;
                message = next;
                sendStateChanged('message');
            },
        },
        title: {
            enumerable: true,
            get: () => title,
            set: (value) => {
                const next = _normalizeTreeViewText(value);
                if (next === title) return;
                title = next;
                sendStateChanged('title');
            },
        },
        description: {
            enumerable: true,
            get: () => description,
            set: (value) => {
                const next = _normalizeTreeViewText(value);
                if (next === description) return;
                description = next;
                sendStateChanged('description');
            },
        },
        badge: {
            enumerable: true,
            get: () => badge,
            set: (value) => {
                const next = _normalizeTreeViewBadge(value);
                const previous = badge === undefined
                    ? undefined
                    : JSON.stringify(badge);
                const current = next === undefined
                    ? undefined
                    : JSON.stringify(next);
                if (previous === current) return;
                badge = next;
                sendStateChanged('badge');
            },
        },
    });
    _treeViews.set(normalized, view);
    _sendTreeDragDropControllerRegistered(normalized, dndController);
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
    const normalized = String(event || '').trim();
    if (!normalized) return 0;
    const existing = _activationEventInFlight.get(normalized);
    if (existing) return existing;
    const promise = (async () => {
        const targets = [];
        for (const [extensionId, known] of _knownExtensions.entries()) {
            if (_extensions.has(extensionId)) continue;
            if (_activationEventMatches(known?.manifest?.activationEvents, normalized)) {
                targets.push(extensionId);
            }
        }
        for (const extensionId of targets) {
            try {
                await _activateKnownExtension(extensionId);
            } catch (err) {
                log(`activation event ${normalized} failed for ${extensionId}: ${err.message}`);
            }
        }
        return targets.length;
    })();
    _activationEventInFlight.set(normalized, promise);
    try {
        return await promise;
    } finally {
        _activationEventInFlight.delete(normalized);
    }
}

async function _activateKnownExtensionsForEventPrefix(prefix) {
    const normalized = String(prefix || '').trim();
    if (!normalized) return 0;
    const events = new Set();
    for (const known of _knownExtensions.values()) {
        const activationEvents = known?.manifest?.activationEvents;
        if (!Array.isArray(activationEvents)) continue;
        for (const event of activationEvents) {
            const value = String(event || '').trim();
            if (value.startsWith(normalized)) events.add(value);
        }
    }
    let activated = 0;
    for (const event of events) {
        activated += await _activateKnownExtensionsForEvent(event);
    }
    return activated;
}

async function _activateKnownExtensionsForDocumentLanguage(document) {
    const languageId = String(document?.languageId || '').trim();
    if (!languageId) return 0;
    return _activateKnownExtensionsForEvent(`onLanguage:${languageId}`);
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

function _quickPickItemIsPickable(item) {
    return item !== undefined && !_quickPickItemIsSeparator(item);
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
    const folders = _workspaceFoldersSnapshot().filter(folder => folder && folder.uri);
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

function _quickInputNormalizeValidationMessage(value) {
    if (value === undefined || value === null || value === '') {
        return { message: undefined, severity: 0 };
    }
    if (typeof value === 'string') {
        return { message: value, severity: 3 };
    }
    if (typeof value === 'object') {
        const message = value.message === undefined || value.message === null
            ? ''
            : String(value.message);
        const severity = Number(value.severity);
        return {
            message,
            severity: Number.isFinite(severity) ? severity : 3,
        };
    }
    return { message: String(value), severity: 3 };
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
        this._ignoreFocusOut = true;
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
        this._matchOnDescription = true;
        this._matchOnDetail = true;
        this._sortByLabel = true;
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
        this._activeItems = this._filterPickableItems(this._activeItems);
        this._selectedItems = this._filterPickableItems(this._selectedItems);
        const picked = this._items.filter(item => _quickPickItemIsPickable(item) && item.picked);
        if (!this._selectedItems.length && picked.length) {
            this._selectedItems = this._canSelectMany ? picked : [picked[0]];
        }
        if (!this._activeItems.length) {
            const first = picked[0] || this._items.find(_quickPickItemIsPickable);
            this._activeItems = first ? [first] : [];
        }
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
    get sortByLabel() { this._assertAlive(); return this._sortByLabel; }
    set sortByLabel(value) {
        this._assertAlive();
        this._sortByLabel = value !== false;
        this._send('update', { changed: 'sortByLabel' });
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
        this._activeItems = this._filterPickableItems(value);
        this._onDidChangeActiveEmitter.fire(this._activeItems);
        this._send('changeActive', { activeItems: this._activeItems });
    }
    get selectedItems() { this._assertAlive(); return this._selectedItems; }
    set selectedItems(value) {
        this._assertAlive();
        this._selectedItems = this._filterPickableItems(value);
        this._onDidChangeSelectionEmitter.fire(this._selectedItems);
        this._send('changeSelection', { selectedItems: this._selectedItems });
    }
    _filterPickableItems(value) {
        const source = Array.isArray(value) ? value : [];
        const current = new Set(this._items.filter(_quickPickItemIsPickable));
        return source.filter(item => current.has(item));
    }
    _setActiveItemsFromHost(value) {
        this._activeItems = this._filterPickableItems(value);
        this._onDidChangeActiveEmitter.fire(this._activeItems);
        this._send('changeActive', { activeItems: this._activeItems });
    }
    _setSelectedItemsFromHost(value) {
        this._selectedItems = this._filterPickableItems(value);
        this._onDidChangeSelectionEmitter.fire(this._selectedItems);
        this._send('changeSelection', { selectedItems: this._selectedItems });
    }
    _accept() {
        this._assertAlive();
        this._onDidAcceptEmitter.fire();
        this._send('accept');
    }
    _triggerButton(button, checked = undefined) {
        this._assertAlive();
        if (checked !== undefined && button && button.toggle) {
            button.toggle.checked = !!checked;
        }
        this._onDidTriggerButtonEmitter.fire(button);
        this._send('triggerButton', { button });
    }
    _triggerItemButton(item, button, checked = undefined) {
        this._assertAlive();
        if (checked !== undefined && button && button.toggle) {
            button.toggle.checked = !!checked;
        }
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
            sortByLabel: this._sortByLabel,
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
        this._validationSeverity = 0;
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
        this._validationSeverity = _quickInputNormalizeValidationMessage(value).severity;
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
            validationMessage: _quickInputNormalizeValidationMessage(
                this._validationMessage).message,
            severity: this._validationSeverity,
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

function _quickInputButtonForHandle(input, handle) {
    const buttons = Array.isArray(input?._buttons) ? input._buttons : [];
    const h = Number(handle);
    if (!Number.isInteger(h)) return undefined;
    if (h === -1) {
        return buttons.find(button => String(button?.tooltip || '').toLowerCase() === 'back');
    }
    return h >= 0 && h < buttons.length ? buttons[h] : undefined;
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
        const enabled = input._enabled !== false;
        if (action === 'changeValue') {
            if (!enabled) return;
            input.value = String(msg.value ?? '');
        } else if (action === 'changeActive' && input instanceof QuickPickInput) {
            if (!enabled) return;
            input._setActiveItemsFromHost(
                _quickInputItemsForIndices(input, msg.itemIndices || msg.indices));
        } else if (action === 'changeSelection' && input instanceof QuickPickInput) {
            if (!enabled) return;
            input._setSelectedItemsFromHost(
                _quickInputItemsForIndices(input, msg.itemIndices || msg.indices));
        } else if (action === 'triggerButton') {
            if (!enabled) return;
            const button = msg.buttonHandle !== undefined
                ? _quickInputButtonForHandle(input, msg.buttonHandle)
                : _quickInputButtonForIndex(input, msg.buttonIndex);
            if (button !== undefined && typeof input._triggerButton === 'function') {
                input._triggerButton(button, msg.checked);
            }
        } else if (action === 'triggerItemButton' && input instanceof QuickPickInput) {
            if (!enabled) return;
            const item = _quickInputItemForIndex(input, msg.itemIndex);
            const buttons = Array.isArray(item?.buttons) ? item.buttons : [];
            const buttonIndex = msg.buttonHandle !== undefined
                ? Number(msg.buttonHandle)
                : Number(msg.buttonIndex);
            const button = Number.isInteger(buttonIndex) && buttonIndex >= 0
                ? buttons[buttonIndex]
                : undefined;
            if (item !== undefined && button !== undefined) {
                input._triggerItemButton(item, button, msg.checked);
            }
        } else if (action === 'accept' && typeof input._accept === 'function') {
            if (!enabled) return;
            if (input instanceof QuickPickInput && msg.itemIndex !== undefined) {
                const item = _quickInputItemForIndex(input, msg.itemIndex);
                if (_quickPickItemIsPickable(item)) {
                    input._setActiveItemsFromHost([item]);
                    if (!input._canSelectMany) input._setSelectedItemsFromHost([item]);
                }
            }
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
    return _requestWindowMessage(level, message, options, items, normalizedItems);
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

function _statusBarText(value) {
    if (value === undefined || value === null) return '';
    if (typeof value === 'string') return value;
    if (typeof value === 'object' && value.value !== undefined) return String(value.value ?? '');
    return String(value);
}

function _statusBarColor(value) {
    if (value === undefined || value === null) return '';
    if (typeof value === 'string') return value;
    if (value instanceof ThemeColor) return { id: value.id };
    if (typeof value === 'object' && value.id) return { id: String(value.id) };
    return String(value);
}

function _statusBarCommand(value) {
    if (value === undefined || value === null || value === '') return '';
    if (typeof value === 'string') return value;
    if (typeof value === 'object' && value.command) {
        return {
            command: String(value.command),
            title: value.title === undefined || value.title === null
                ? ''
                : String(value.title),
            arguments: Array.isArray(value.arguments)
                ? value.arguments.map(arg => _serializeArgForPython(arg))
                : [],
        };
    }
    return '';
}

function _statusBarAccessibility(value) {
    if (!value || typeof value !== 'object') return null;
    return {
        label: value.label === undefined || value.label === null
            ? ''
            : String(value.label),
        role: value.role === undefined || value.role === null
            ? ''
            : String(value.role),
    };
}

function _statusBarPriority(value) {
    if (typeof value !== 'number' || Number.isNaN(value)) return undefined;
    if (value === Number.POSITIVE_INFINITY) return Number.MAX_VALUE;
    if (value === Number.NEGATIVE_INFINITY) return -Number.MAX_VALUE;
    return value;
}

function _createStatusBarItemObject(id, alignment, priority) {
    const state = {
        alignment,
        priority: _statusBarPriority(priority),
        text: '',
        tooltip: '',
        color: '',
        backgroundColor: undefined,
        command: undefined,
        name: '',
        accessibilityInformation: undefined,
        visible: false,
        disposed: false,
    };
    const item = { _id: id };
    Object.defineProperty(item, 'id', {
        enumerable: true,
        get() { return id; },
    });
    function payload() {
        return {
            type: 'status_bar_show',
            id,
            text: _statusBarText(state.text),
            tooltip: _statusBarText(state.tooltip),
            command: _statusBarCommand(state.command),
            alignment: state.alignment,
            priority: state.priority,
            color: _statusBarColor(state.color),
            backgroundColor: _statusBarColor(state.backgroundColor),
            name: _statusBarText(state.name),
            accessibilityInformation: _statusBarAccessibility(
                state.accessibilityInformation),
        };
    }
    function refreshIfVisible() {
        if (!state.disposed && state.visible) send(payload());
    }
    [
        'alignment', 'priority', 'text', 'tooltip', 'color',
        'backgroundColor', 'command', 'name', 'accessibilityInformation',
    ].forEach(prop => {
        Object.defineProperty(item, prop, {
            enumerable: true,
            get() { return state[prop]; },
            set(value) { state[prop] = value; refreshIfVisible(); },
        });
    });
    item.show = () => {
        if (state.disposed) return;
        state.visible = true;
        send(payload());
    };
    item.hide = () => {
        if (state.disposed) return;
        state.visible = false;
        send({ type: 'status_bar_hide', id });
    };
    item.dispose = () => {
        if (state.disposed) return;
        state.disposed = true;
        state.visible = false;
        send({ type: 'status_bar_dispose', id });
    };
    return item;
}

function _languageStatusSeverity(value) {
    const n = Number(value);
    if (n === 2) return 2;
    if (n === 1) return 1;
    return 0;
}

function _languageStatusLabel(value) {
    if (value === undefined || value === null) return '';
    if (typeof value === 'string') return value;
    if (typeof value === 'object' && Object.prototype.hasOwnProperty.call(value, 'value')) {
        return {
            value: String(value.value ?? ''),
            shortValue: value.shortValue === undefined || value.shortValue === null
                ? ''
                : String(value.shortValue),
        };
    }
    return String(value);
}

function _languageStatusSource(extDesc, name) {
    return String(
        name
        || extDesc.manifest?.displayName
        || extDesc.displayName
        || extDesc.name
        || extDesc.extensionId
        || 'Extension');
}

function _createLanguageStatusItemObject(extDesc, id, selector) {
    const itemId = String(id || '');
    if (!itemId) throw new Error('LanguageStatusItem id is required');
    const extensionId = String(extDesc.extensionId || extDesc.name || 'extension');
    const fullyQualifiedId = `${extensionId}/${itemId}`;
    if (_languageStatusItems.has(fullyQualifiedId)) {
        throw new Error(`LanguageStatusItem with id '${itemId}' ALREADY exists`);
    }
    const state = {
        handle: _nextLanguageStatusHandle++,
        id: itemId,
        fullyQualifiedId,
        extensionId,
        selector,
        source: _languageStatusSource(extDesc, ''),
        name: _languageStatusSource(extDesc, ''),
        severity: 0,
        text: '',
        detail: '',
        command: undefined,
        accessibilityInformation: undefined,
        busy: false,
        disposed: false,
        updateTimer: null,
    };
    _languageStatusItems.set(fullyQualifiedId, state);

    function payload() {
        return {
            type: 'language_status_set',
            handle: state.handle,
            id: state.fullyQualifiedId,
            itemId: state.id,
            extensionId: state.extensionId,
            source: state.source,
            name: String(state.name || state.source),
            selector: _serializeLanguageValue(state.selector),
            label: _languageStatusLabel(state.text),
            text: _languageStatusLabel(state.text),
            detail: state.detail === undefined || state.detail === null
                ? ''
                : String(state.detail),
            severity: _languageStatusSeverity(state.severity),
            command: _statusBarCommand(state.command),
            accessibilityInfo: _statusBarAccessibility(state.accessibilityInformation),
            accessibilityInformation: _statusBarAccessibility(state.accessibilityInformation),
            busy: !!state.busy,
        };
    }

    function scheduleUpdate() {
        if (state.disposed) {
            console.warn(`LanguageStatusItem (${itemId}) from ${extensionId} has been disposed and CANNOT be updated anymore`);
            return;
        }
        if (state.updateTimer) clearTimeout(state.updateTimer);
        state.updateTimer = setTimeout(() => {
            state.updateTimer = null;
            if (!state.disposed && _languageStatusItems.get(fullyQualifiedId) === state) {
                send(payload());
            }
        }, 0);
    }

    const item = {};
    Object.defineProperty(item, 'id', { enumerable: true, get() { return state.id; } });
    ['name', 'selector', 'text', 'detail', 'severity', 'command',
     'accessibilityInformation', 'busy'].forEach(prop => {
        Object.defineProperty(item, prop, {
            enumerable: true,
            get() { return state[prop]; },
            set(value) {
                if (prop === 'severity') state[prop] = _languageStatusSeverity(value);
                else if (prop === 'busy') state[prop] = !!value;
                else state[prop] = value;
                scheduleUpdate();
            },
        });
    });
    item.dispose = () => {
        if (state.disposed) return;
        state.disposed = true;
        if (state.updateTimer) {
            clearTimeout(state.updateTimer);
            state.updateTimer = null;
        }
        _languageStatusItems.delete(fullyQualifiedId);
        send({
            type: 'language_status_remove',
            handle: state.handle,
            id: state.fullyQualifiedId,
            itemId: state.id,
            extensionId: state.extensionId,
        });
    };
    return item;
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
    const environmentCollectionShared = _environmentCollectionShared(
        extDesc.extensionId || extDesc.name || '');
    const environmentVariableCollection = new EnvironmentVariableCollection(
        environmentCollectionShared,
    );
    subscriptions.push(new Disposable(() => {
        if (_environmentVariableCollections.get(environmentCollectionShared.extensionId)
                !== environmentCollectionShared) {
            return;
        }
        environmentCollectionShared.map.clear();
        environmentCollectionShared.descriptions.clear();
        environmentCollectionShared.emitter.dispose();
        _environmentVariableCollections.delete(
            environmentCollectionShared.extensionId);
    }));
    const extensionUri = Uri.file(extensionPath);
    const extensionKind = _extensionKindValue(extDesc.manifest?.extensionKind);

    const context = {
        subscriptions,
        extensionPath,
        extensionUri,
        asAbsolutePath(relativePath) {
            return path.join(extensionPath, relativePath || '');
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
        environmentVariableCollection,
        languageModelAccessInformation: {
            onDidChange: new EventEmitter().event,
            canSendRequest: () => true,
        },
    };

    // Build the vscode namespace
    const vscode = {
        l10n,
        // vscode.version was entirely missing - real gap, confirmed root
        // cause of cpptools' "Cannot read properties of undefined (reading
        // 'split')" (its own getVsCodeVersion() does vscode.version.
        // split('.')) - extensions commonly parse this as SemVer to gate
        // features. A recent, real VS Code release string so >= comparisons
        // against modern feature checks behave the way they would in a
        // genuinely up-to-date VS Code.
        version: '1.95.0',
        // --- Types ---
        Uri,
        Position,
        Range,
        Selection,
        Location,
        TabInputText,
        ThemeColor,
        ThemeIcon,
        FileDecoration,
        DocumentHighlight,
        EvaluatableExpression,
        InlineValueText,
        InlineValueVariableLookup,
        InlineValueEvaluatableExpression,
        InlineValueContext,
        SymbolInformation,
        CallHierarchyItem,
        CallHierarchyIncomingCall,
        CallHierarchyOutgoingCall,
        TypeHierarchyItem,
        NotebookCellData,
        NotebookCellOutput,
        NotebookCellOutputItem,
        NotebookCellStatusBarItem,
        NotebookData,
        NotebookEdit,
        NotebookRange,
        Disposable,
        EventEmitter,
        CancellationTokenSource,
        Breakpoint,
        SourceBreakpoint,
        FunctionBreakpoint,
        DataBreakpoint,
        TerminalProfile,
        TerminalLink,
        McpStdioServerDefinition,
        McpHttpServerDefinition,

        // --- Enums ---
        ViewColumn: { One: 1, Two: 2, Three: 3, Active: -1, Beside: -2 },
        StatusBarAlignment: { Left: 1, Right: 2 },
        ConfigurationTarget: {
            Global: 1,
            Workspace: 2,
            WorkspaceFolder: 3,
        },
        TerminalLocation: { Panel: 1, Editor: 2 },
        TaskScope: { Global: 1, Workspace: 2 },
        DebugConsoleMode: { Separate: 0, MergeWithParent: 1 },
        DebugConfigurationProviderTriggerKind: { Initial: 1, Dynamic: 2 },
        TerminalExitReason: {
            Unknown: 0,
            Shutdown: 1,
            Process: 2,
            User: 3,
            Extension: 4,
        },
        TerminalShellExecutionCommandLineConfidence: {
            Low: 0,
            Medium: 1,
            High: 2,
        },
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
        TreeItemCheckboxState: { Unchecked: 0, Checked: 1 },
        ExtensionKind: { UI: 1, Workspace: 2 },
        ExtensionMode: { Production: 1, Development: 2, Test: 3 },
        EnvironmentVariableMutatorType,
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
        // languages.setLanguageConfiguration's onEnterRules use this enum -
        // real gap, confirmed shared root cause across ms-python.python,
        // ms-python.vscode-pylance, and ms-vscode.cpptools (2026-07-02
        // sweep) - all three build a language configuration referencing
        // IndentAction at module load time.
        IndentAction: { None: 0, Indent: 1, IndentOutdent: 2, Outdent: 3 },
        LanguageStatusSeverity: { Information: 0, Warning: 1, Error: 2 },
        NotebookCellKind,
        NotebookCellStatusBarAlignment,
        NotebookControllerAffinity,
        NotebookControllerAffinity2: NotebookControllerAffinity,
        DocumentHighlightKind: { Text: 0, Read: 1, Write: 2 },
        ProgressLocation,
        CompletionItemKind: Object.fromEntries([
            'Text', 'Method', 'Function', 'Constructor', 'Field', 'Variable',
            'Class', 'Interface', 'Module', 'Property', 'Unit', 'Value',
            'Enum', 'Keyword', 'Snippet', 'Color', 'File', 'Reference',
            'Folder', 'EnumMember', 'Constant', 'Struct', 'Event', 'Operator',
            'TypeParameter',
        ].map((n, i) => [n, i])),
        CompletionItemInsertTextRule: { None: 0, KeepWhitespace: 1, InsertAsSnippet: 4 },
        CompletionItemTag: { Deprecated: 1 },
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
                send({
                    type: 'command_registered',
                    commandId: id,
                    extensionId: extDesc.extensionId || '',
                    kind: 'command',
                    editorRequired: false,
                });
                const d = new Disposable(() => {
                    _commands.delete(id);
                    send({
                        type: 'command_disposed',
                        commandId: id,
                        extensionId: extDesc.extensionId || '',
                        kind: 'command',
                        editorRequired: false,
                    });
                });
                subscriptions.push(d);
                return d;
            },
            executeCommand(id, ...args) {
                const handler = _commands.get(id);
                if (handler) return Promise.resolve(handler(...args));
                return _executePythonCommand(id, args);
            },
            registerTextEditorCommand(id, handler, thisArg) {
                _commands.set(id, (...args) => {
                    const editor = _activeTextEditor;
                    if (!editor || !editor.document) {
                        log(`Cannot execute ${id} because there is no active text editor.`);
                        return undefined;
                    }
                    return editor.edit(editBuilder => (
                        handler.apply(thisArg, [editor, editBuilder, ...args])));
                });
                send({
                    type: 'command_registered',
                    commandId: id,
                    extensionId: extDesc.extensionId || '',
                    kind: 'textEditorCommand',
                    editorRequired: true,
                });
                const d = new Disposable(() => {
                    _commands.delete(id);
                    send({
                        type: 'command_disposed',
                        commandId: id,
                        extensionId: extDesc.extensionId || '',
                        kind: 'textEditorCommand',
                        editorRequired: true,
                    });
                });
                subscriptions.push(d);
                return d;
            },
            getCommands(filterInternal) {
                return Promise.resolve([..._commands.keys()]);
            },
        },

        // --- Namespace: notebooks ---
        notebooks: {
            createNotebookController(id, notebookType, label, handler) {
                const controller = _createNotebookController(
                    extDesc, id, notebookType, label, handler);
                subscriptions.push(new Disposable(() => controller.dispose()));
                return controller;
            },
            createNotebookControllerDetectionTask(notebookType) {
                const normalized = String(notebookType || '').trim();
                if (!normalized) throw new Error('notebookType cannot be empty or just whitespace');
                send({
                    type: 'notebook_controller_detection_task_registered',
                    notebookType: normalized,
                    extensionId: extDesc.extensionId || '',
                });
                const disposable = new Disposable(() => send({
                    type: 'notebook_controller_detection_task_disposed',
                    notebookType: normalized,
                    extensionId: extDesc.extensionId || '',
                }));
                subscriptions.push(disposable);
                return disposable;
            },
            registerNotebookCellStatusBarItemProvider(notebookType, provider) {
                const disposable = _registerNotebookCellStatusBarItemProvider(
                    extDesc, notebookType, provider);
                subscriptions.push(disposable);
                return disposable;
            },
        },

        // --- Namespace: window ---
        window: {
            createNotebookController(id, notebookType, label, handler) {
                const controller = _createNotebookController(
                    extDesc, id, notebookType, label, handler);
                subscriptions.push(new Disposable(() => controller.dispose()));
                return controller;
            },
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
                    extensionId: extDesc.extensionId || '',
                    extensionPath,
                });
                send({
                    type: 'webview_view_provider_registered',
                    viewType,
                    extensionId: extDesc.extensionId || '',
                    options: _plainBridgeValue(options || {}),
                });
                if (!_webviewViewActivationResolving.has(String(viewType || ''))) {
                    setImmediate(() => { void resolveWebviewView(viewType); });
                }
                log(`registered WebviewViewProvider: ${viewType}`);
                return new Disposable(() => {
                    _webviewViewProviders.delete(viewType);
                    send({
                        type: 'webview_view_provider_disposed',
                        viewType,
                        extensionId: extDesc.extensionId || '',
                        options: _plainBridgeValue(options || {}),
                    });
                });
            },
            registerCustomEditorProvider(viewType, provider, options) {
                const normalized = String(viewType || '');
                if (!normalized) {
                    throw new Error('Custom editor viewType is required');
                }
                if (!provider || (
                        typeof provider.resolveCustomTextEditor !== 'function'
                        && typeof provider.resolveCustomEditor !== 'function')) {
                    throw new Error(
                        'Custom editor provider must implement resolveCustomTextEditor or resolveCustomEditor');
                }
                if (_customEditorProviders.has(normalized)) {
                    throw new Error(
                        `CustomEditorProvider already registered for viewType: ${normalized}`);
                }
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
                    capabilities: {
                        text: typeof provider.resolveCustomTextEditor === 'function',
                        custom: typeof provider.resolveCustomEditor === 'function',
                        move: typeof provider.moveCustomTextEditor === 'function',
                        save: typeof provider.saveCustomDocument === 'function',
                        saveAs: typeof provider.saveCustomDocumentAs === 'function',
                        revert: typeof provider.revertCustomDocument === 'function',
                        backup: typeof provider.backupCustomDocument === 'function',
                    },
                });
                log(`registered CustomEditorProvider: ${normalized}`);
                return new Disposable(() => {
                    changeSubscription?.dispose?.();
                    _customEditorProviders.delete(normalized);
                    const disposedDocuments = new Set();
                    for (const [key, entry] of [..._customEditorDocuments.entries()]) {
                        if (entry.viewType !== normalized) continue;
                        _customEditorDocuments.delete(key);
                        if (entry.viewId) _customEditorViewKeys.delete(entry.viewId);
                        if (!disposedDocuments.has(entry.document)) {
                            disposedDocuments.add(entry.document);
                            try { entry.document?.dispose?.(); } catch {}
                        }
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
                const languageId = typeof options === 'string'
                    ? options
                    : (options && typeof options.languageId === 'string' ? options.languageId : '');
                const isLog = !!(options && typeof options === 'object' && options.log === true);
                const ch = new OutputChannel(name, languageId, isLog);
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
                const id = typeof alignmentOrId === 'string' && alignmentOrId
                    ? alignmentOrId
                    : 'sbi-node-' + (++_sbiCounter);
                const align = typeof alignmentOrId === 'number'
                    ? alignmentOrId
                    : (typeof priorityOrAlignment === 'number'
                        ? priorityOrAlignment
                        : 2);
                const pri = typeof alignmentOrId === 'number'
                    ? (typeof priorityOrAlignment === 'number'
                        ? priorityOrAlignment
                        : 0)
                    : (typeof priority === 'number' ? priority : 0);
                return _createStatusBarItemObject(id, align, pri);
            },
            registerTreeDataProvider(viewId, treeDataProvider) {
                log(`registerTreeDataProvider ${viewId}`);
                const d = registerTreeDataProviderInternal(viewId, treeDataProvider);
                subscriptions.push(d);
                return d;
            },
            registerFileDecorationProvider(provider) {
                const handle = _nextFileDecorationProviderHandle++;
                const hasChangeEvent = !!(
                    provider && typeof provider.onDidChangeFileDecorations === 'function'
                );
                const hasProvider = !!(
                    provider && typeof provider.provideFileDecoration === 'function'
                );
                const entry = {
                    handle,
                    extensionId: extDesc.extensionId || '',
                    provider,
                    hasChangeEvent,
                    hasProvider,
                };
                _fileDecorationProviders.push(entry);
                let changeSubscription = null;
                if (hasChangeEvent) {
                    changeSubscription = provider.onDidChangeFileDecorations((value) => {
                        const change = _fileDecorationChangedPayload(value);
                        send({
                            type: 'file_decoration_changed',
                            handle,
                            extensionId: entry.extensionId,
                            all: change.all,
                            value: change.uris,
                            count: change.count,
                            capped: change.capped,
                        });
                    });
                }
                send({
                    type: 'file_decoration_provider_registered',
                    handle,
                    extensionId: entry.extensionId,
                    hasChangeEvent,
                    hasProvider,
                });
                const d = new Disposable(() => {
                    const idx = _fileDecorationProviders.indexOf(entry);
                    if (idx >= 0) _fileDecorationProviders.splice(idx, 1);
                    changeSubscription?.dispose?.();
                    send({
                        type: 'file_decoration_provider_disposed',
                        handle,
                        extensionId: entry.extensionId,
                    });
                });
                subscriptions.push(d);
                return d;
            },
            createTreeView(viewId, options) {
                log(`createTreeView ${viewId}`);
                const view = createTreeViewObject(
                    viewId,
                    options?.treeDataProvider,
                    options?.dragAndDropController);
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
            createTextEditorDecorationType(options) {
                return _createTextEditorDecorationType(options || {});
            },
            showTextDocument(documentOrUri, columnOrOptions, preserveFocus) {
                return _windowShowTextDocument(
                    documentOrUri, columnOrOptions, preserveFocus);
            },
            createTerminal(nameOrOptions, shellPath, shellArgs) {
                return _createTerminal(nameOrOptions, shellPath, shellArgs);
            },
            registerTerminalLinkProvider(provider) {
                if (!provider || typeof provider.provideTerminalLinks !== 'function') {
                    throw new Error('TerminalLinkProvider must implement provideTerminalLinks');
                }
                const entry = {
                    provider,
                    extensionId: extDesc.extensionId || '',
                };
                _terminalLinkProviders.add(entry);
                send({
                    type: 'terminal_link_provider_registered',
                    extensionId: entry.extensionId,
                    providerCount: _terminalLinkProviders.size,
                });
                const d = new Disposable(() => {
                    if (!_terminalLinkProviders.delete(entry)) return;
                    for (const cache of _terminalLinkCache.values()) {
                        for (const [linkId, cached] of [...cache.entries()]) {
                            if (cached.provider === provider) cache.delete(linkId);
                        }
                    }
                    send({
                        type: 'terminal_link_provider_disposed',
                        extensionId: entry.extensionId,
                        providerCount: _terminalLinkProviders.size,
                    });
                });
                subscriptions.push(d);
                return d;
            },
            registerTerminalProfileProvider(id, provider) {
                const normalized = String(id || '');
                if (!normalized) {
                    throw new Error('Terminal profile provider id is required');
                }
                if (!provider || typeof provider.provideTerminalProfile !== 'function') {
                    throw new Error('TerminalProfileProvider must implement provideTerminalProfile');
                }
                if (_terminalProfileProviders.has(normalized)) {
                    throw new Error(`Terminal profile provider "${normalized}" already registered`);
                }
                const entry = {
                    provider,
                    extensionId: extDesc.extensionId || '',
                };
                _terminalProfileProviders.set(normalized, entry);
                send({
                    type: 'terminal_profile_provider_registered',
                    id: normalized,
                    extensionId: entry.extensionId,
                });
                const d = new Disposable(() => {
                    if (_terminalProfileProviders.get(normalized) !== entry) return;
                    _terminalProfileProviders.delete(normalized);
                    send({
                        type: 'terminal_profile_provider_disposed',
                        id: normalized,
                        extensionId: entry.extensionId,
                    });
                });
                subscriptions.push(d);
                return d;
            },
            get terminals() { return _terminals.slice(); },
            get activeTerminal() { return _activeTerminal; },
            onDidChangeActiveTerminal: _onDidChangeActiveTerminalEmitter.event,
            onDidOpenTerminal: _onDidOpenTerminalEmitter.event,
            onDidCloseTerminal: _onDidCloseTerminalEmitter.event,
            onDidChangeTerminalState: _onDidChangeTerminalStateEmitter.event,
            onDidChangeTerminalShellIntegration: _onDidChangeTerminalShellIntegrationEmitter.event,
            onDidStartTerminalShellExecution: _onDidStartTerminalShellExecutionEmitter.event,
            onDidEndTerminalShellExecution: _onDidEndTerminalShellExecutionEmitter.event,
            get state() { return { ..._windowState }; },
            onDidChangeWindowState: _onDidChangeWindowStateEmitter.event,
            get activeTextEditor() { return _activeTextEditor; },
            get visibleTextEditors() { return Array.from(_visibleTextEditors.values()); },
            get activeColorTheme() { return { kind: 2 }; }, // Dark
            onDidChangeActiveTextEditor: _onDidChangeActiveTextEditorEmitter.event,
            onDidChangeVisibleTextEditors: _onDidChangeVisibleTextEditorsEmitter.event,
            onDidChangeTextEditorSelection: _onDidChangeTextEditorSelectionEmitter.event,
            onDidChangeTextEditorOptions: _onDidChangeTextEditorOptionsEmitter.event,
            onDidChangeTextEditorVisibleRanges: _onDidChangeTextEditorVisibleRangesEmitter.event,
            onDidChangeTextEditorViewColumn: _onDidChangeTextEditorViewColumnEmitter.event,
            onDidChangeTextEditorDiffInformation: _onDidChangeTextEditorDiffInformationEmitter.event,
            onDidChangeActiveColorTheme: new EventEmitter().event,
            get tabGroups() {
                return _tabGroupsApiObject();
            },
        },

        // --- Namespace: workspace ---
        workspace: {
            getConfiguration(section, scope) {
                return _createConfigProxy(
                    section, _configOverrideIdentifierFromScope(scope));
            },
            get workspaceFolders() {
                const folders = _workspaceFoldersSnapshot();
                return folders.length ? folders : undefined;
            },
            get name() {
                if (!_workspaceFolders.length) return undefined;
                return _workspaceFolders.length === 1
                    ? _workspaceFolders[0].name
                    : _workspaceName;
            },
            get rootPath() {
                const folder = _workspaceFolder();
                return folder ? folder.uri.fsPath : undefined;
            },
            get isTrusted() {
                return _workspaceTrusted;
            },
            requestWorkspaceTrust() {
                const wasTrusted = _workspaceTrusted;
                _workspaceTrusted = true;
                if (!wasTrusted) _onDidGrantWorkspaceTrustEmitter.fire();
                return Promise.resolve(true);
            },
            onDidGrantWorkspaceTrust: _onDidGrantWorkspaceTrustEmitter.event,
            get textDocuments() {
                return Array.from(_workspaceTextDocuments.values()).filter(
                    _workspaceDocumentIsOpened);
            },
            get notebookDocuments() {
                return Array.from(_notebookDocuments.values()).filter(
                    document => document && !document.isClosed);
            },
            onDidOpenTextDocument: _onDidOpenTextDocumentEmitter.event,
            onDidCloseTextDocument: _onDidCloseTextDocumentEmitter.event,
            onDidChangeTextDocument: _onDidChangeTextDocumentEmitter.event,
            onDidSaveTextDocument: _onDidSaveTextDocumentEmitter.event,
            onDidOpenNotebookDocument: _onDidOpenNotebookDocumentEmitter.event,
            onDidCloseNotebookDocument: _onDidCloseNotebookDocumentEmitter.event,
            onDidChangeNotebookDocument: _onDidChangeNotebookDocumentEmitter.event,
            onDidSaveNotebookDocument: _onDidSaveNotebookDocumentEmitter.event,
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
                isWritableFileSystem(scheme) {
                    const normalized = _normalizeFileSystemScheme(scheme);
                    if (!normalized) return undefined;
                    if (normalized === 'file') return true;
                    const entry = _fileSystemProviders.get(normalized);
                    if (!entry) return undefined;
                    return _fileSystemProviderReadonly(entry)
                        ? false
                        : true;
                },
            },
            registerFileSystemProvider(scheme, provider, options) {
                const normalized = _normalizeFileSystemScheme(scheme);
                if (!normalized || normalized === 'file') {
                    throw new Error(`Invalid filesystem provider scheme: ${scheme}`);
                }
                if (!provider || typeof provider !== 'object') {
                    throw new Error(`Filesystem provider is required for scheme: ${scheme}`);
                }
                if (_fileSystemProviders.has(normalized)) {
                    throw new Error(`Filesystem provider already registered for scheme: ${normalized}`);
                }
                const entry = {
                    scheme: normalized,
                    provider,
                    options: options || {},
                    extensionId: extDesc.extensionId || '',
                    eventSubscription: null,
                    lastEvents: [],
                };
                entry.eventSubscription = _subscribeFileSystemProviderEvents(entry);
                _fileSystemProviders.set(normalized, entry);
                return new Disposable(() => {
                    if (_fileSystemProviders.get(normalized) === entry) {
                        try { entry.eventSubscription?.dispose?.(); } catch {}
                        _fileSystemProviders.delete(normalized);
                    }
                });
            },
            onDidChangeConfiguration: _onDidChangeConfigurationEmitter.event,
            onDidChangeWorkspaceFolders: _onDidChangeWorkspaceFoldersEmitter.event,
            onWillCreateFiles: _onWillCreateFilesEmitter.event,
            onDidCreateFiles: _onDidCreateFilesEmitter.event,
            onWillDeleteFiles: _onWillDeleteFilesEmitter.event,
            onDidDeleteFiles: _onDidDeleteFilesEmitter.event,
            onWillRenameFiles: _onWillRenameFilesEmitter.event,
            onDidRenameFiles: _onDidRenameFilesEmitter.event,
            registerPortAttributesProvider(portSelector, provider) {
                const handle = _nextPortAttributesProviderHandle++;
                _portAttributesProviders.set(handle, { provider, portSelector });
                return new Disposable(() => _portAttributesProviders.delete(handle));
            },
            updateWorkspaceFolders(start, deleteCount, ...workspaceFoldersToAdd) {
                return _workspaceUpdateWorkspaceFolders(
                    start, deleteCount, ...workspaceFoldersToAdd);
            },
            getWorkspaceFolder(uri) {
                return _workspaceGetWorkspaceFolder(uri);
            },
            findFiles(include, exclude, maxResults) {
                return _diagnoseAsync(
                    'workspace.findFiles',
                    _workspacePatternText(include),
                    () => _workspaceFindFiles(include, exclude, maxResults),
                );
            },
            createFileSystemWatcher(pattern, optionsOrIgnoreCreate, ignoreChange, ignoreDelete) {
                return _workspaceCreateFileSystemWatcher(
                    pattern, optionsOrIgnoreCreate, ignoreChange, ignoreDelete);
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
            openNotebookDocument(uriOrType, content) {
                return _diagnoseAsync(
                    'workspace.openNotebookDocument',
                    uriOrType instanceof Uri ? uriOrType.toString() : String(uriOrType || ''),
                    () => _openNotebookDocument(uriOrType, content),
                );
            },
            registerNotebookSerializer(viewType, serializer, options) {
                const disposable = _registerNotebookSerializer(
                    extDesc, viewType, serializer, options);
                subscriptions.push(disposable);
                return disposable;
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
            appHost: 'desktop',
            appRoot: process.cwd(),
            language: 'en',
            machineId: 'node-ext-host',
            sessionId: `session-${Date.now()}`,
            uriScheme: 'vscode',
            remoteName: undefined,
            isNewAppInstall: false,
            isAppPortable: false,
            isTelemetryEnabled: false,
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
            get logLevel() { return _envLogLevel; },
            onDidChangeTelemetryEnabled: _onDidChangeTelemetryEnabledEmitter.event,
            onDidChangeShell: _onDidChangeShellEmitter.event,
            onDidChangeLogLevel: _onDidChangeLogLevelEmitter.event,
            createTelemetryLogger: _createTelemetryLogger,
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
                const handle = _nextLanguageProviderHandle++;
                const extensionId = extDesc.extensionId || '';
                const displayName = extDesc.manifest?.displayName
                    || extDesc.displayName
                    || extDesc.name
                    || extensionId
                    || kind;
                const providerId = String(
                    (extra && (extra.providerId || extra.id || extra.extensionId))
                    || extensionId
                    || `${kind}:${handle}`);
                const entry = Object.assign({
                    handle,
                    extensionId,
                    providerId,
                    id: providerId,
                    displayName: String(
                        (extra && (extra.displayName || extra.name))
                        || displayName),
                    kind,
                    selector,
                    provider,
                }, extra || {});
                _languageProviders.push(entry);
                const resolveSupport = _languageProviderResolveSupport(kind, provider);
                send({
                    type: 'language_provider_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    providerId: entry.providerId,
                    displayName: entry.displayName,
                    kind,
                    selector,
                    triggers: entry.triggers || [],
                    resolveSupport,
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
                registerEvaluatableExpressionProvider(selector, provider) {
                    return _registerLangProvider('evaluatableExpression', selector, provider);
                },
                registerInlineValuesProvider(selector, provider) {
                    return _registerLangProvider('inlineValue', selector, provider);
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
                registerDocumentSymbolProvider(selector, provider, metadata) {
                    const extra = { metadata: metadata || null };
                    if (metadata && metadata.label) extra.displayName = metadata.label;
                    return _registerLangProvider('documentSymbol', selector, provider, extra);
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
                setTextDocumentLanguage(document, languageId) {
                    return _setTextDocumentLanguage(document, languageId);
                },
                setLanguageConfiguration(language, configuration) {
                    return _setLanguageConfiguration(language, configuration);
                },
                createLanguageStatusItem(id, selector) {
                    return _createLanguageStatusItemObject(extDesc, id, selector);
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
                getLanguages() { return Promise.resolve(_knownLanguageIds()); },
            };
        })(),

        // --- Namespace: lm (Language Models) ---
        lm: {
            async selectChatModels(selector) {
                const pythonModels = await _requestPythonLm('selectChatModels', {
                    selector: _serializeLanguageValue(selector || {}),
                    extensionId: extDesc.extensionId || '',
                }, 5000);
                const wrappedPython = (Array.isArray(pythonModels) ? pythonModels : [])
                    .map(_languageModelChatFromPayload)
                    .filter(model => !!model.id);
                const nodeModels = await _nodeLmChatModelsForSelector(
                    selector || {});
                return [...wrappedPython, ...nodeModels];
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
                if (toolName) await _activateKnownExtensionsForEvent(
                    `onLanguageModelTool:${toolName}`);
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
            onDidChangeTools: _onDidChangeLmToolsEmitter.event,
            onDidChangeChatModels: _onDidChangeLmChatModelsEmitter.event,
            registerMcpServerDefinitionProvider(id, provider) {
                const providerId = String(id || '');
                if (!providerId) {
                    throw new Error('MCP server definition provider id is required');
                }
                if (!provider || typeof provider !== 'object') {
                    throw new Error('MCP server definition provider is required');
                }
                const entry = {
                    handle: _nextMcpServerDefinitionProviderHandle++,
                    id: providerId,
                    provider,
                    extensionId: extDesc.extensionId || '',
                };
                _mcpServerDefinitionProviders.set(entry.handle, entry);
                send({
                    type: 'mcp_server_definition_provider_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    id: entry.id,
                });
                let changeDisposable;
                if (typeof provider.onDidChangeMcpServerDefinitions === 'function') {
                    changeDisposable = provider.onDidChangeMcpServerDefinitions(() => {
                        send({
                            type: 'mcp_server_definition_provider_changed',
                            handle: entry.handle,
                            extensionId: entry.extensionId,
                            id: entry.id,
                        });
                    });
                }
                const disposable = new Disposable(() => {
                    if (_mcpServerDefinitionProviders.get(entry.handle) === entry) {
                        _mcpServerDefinitionProviders.delete(entry.handle);
                        try { changeDisposable?.dispose?.(); } catch {}
                        send({
                            type: 'mcp_server_definition_provider_disposed',
                            handle: entry.handle,
                            extensionId: entry.extensionId,
                            id: entry.id,
                        });
                    }
                });
                subscriptions.push(disposable);
                return disposable;
            },
            registerLanguageModelChatProvider(vendor, provider) {
                const providerVendor = String(vendor || '');
                if (!providerVendor) {
                    throw new Error('Language model chat provider vendor is required');
                }
                if (!provider || typeof provider !== 'object') {
                    throw new Error('Language model chat provider is required');
                }
                const entry = {
                    handle: _nextLmChatProviderHandle++,
                    vendor: providerVendor,
                    provider,
                    extensionId: extDesc.extensionId || '',
                };
                _lmChatProviders.set(providerVendor, entry);
                send({
                    type: 'lm_chat_provider_registered',
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    vendor: entry.vendor,
                });
                let changeDisposable;
                if (typeof provider.onDidChangeLanguageModelChatInformation === 'function') {
                    changeDisposable = provider.onDidChangeLanguageModelChatInformation(() => {
                        _onDidChangeLmChatModelsEmitter.fire({
                            vendor: entry.vendor,
                            extensionId: entry.extensionId,
                        });
                        send({
                            type: 'lm_chat_provider_changed',
                            handle: entry.handle,
                            extensionId: entry.extensionId,
                            vendor: entry.vendor,
                        });
                    });
                }
                const d = new Disposable(() => {
                    if (_lmChatProviders.get(providerVendor) === entry) {
                        _lmChatProviders.delete(providerVendor);
                        try { changeDisposable?.dispose?.(); } catch {}
                        send({
                            type: 'lm_chat_provider_disposed',
                            handle: entry.handle,
                            extensionId: entry.extensionId,
                            vendor: entry.vendor,
                        });
                        _onDidChangeLmChatModelsEmitter.fire({
                            vendor: entry.vendor,
                            extensionId: entry.extensionId,
                        });
                    }
                });
                subscriptions.push(d);
                _onDidChangeLmChatModelsEmitter.fire({
                    vendor: entry.vendor,
                    extensionId: entry.extensionId,
                });
                return d;
            },
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
            registerChatWorkspaceContextProvider(id, provider) {
                return _registerChatContextProvider(
                    'workspace', id, provider, undefined,
                    extDesc, subscriptions);
            },
            registerChatExplicitContextProvider(id, provider) {
                return _registerChatContextProvider(
                    'explicit', id, provider, undefined,
                    extDesc, subscriptions);
            },
            registerChatResourceContextProvider(selector, id, provider) {
                return _registerChatContextProvider(
                    'resource', id, provider, selector,
                    extDesc, subscriptions);
            },
            registerChatContextProvider(selector, id, provider) {
                return _registerChatContextProvider(
                    'legacy', id, provider, selector,
                    extDesc, subscriptions);
            },
        },

        // --- Namespace: authentication ---
        authentication: {
            async getSession(providerId, scopes, options) {
                const id = String(providerId || '');
                if (id) await _activateKnownExtensionsForEvent(
                    `onAuthenticationRequest:${id}`);
                return _authGetSession(providerId, scopes || [], options || {});
            },
            registerAuthenticationProvider(id, label, provider, options) {
                return _authRegisterProvider(id, label, provider, options || {});
            },
            onDidChangeSessions: _onDidChangeAuthenticationSessionsEmitter.event,
        },

        // --- Namespace: scm ---
        scm: {
            createSourceControl(id, label, rootUri, options) {
                log(`scm: createSourceControl "${id}" ("${label}")`);
                const groups = new Map();
                let contextValue = String((options && options.contextValue) || '');
                let acceptInputCommand = undefined;
                let count = 0;
                let quickDiffProvider = undefined;
                let historyProvider = undefined;
                let historyProviderDisposables = [];
                let statusBarCommands = undefined;
                let actionButton = undefined;
                let validateInput = undefined;
                const inputBoxState = {
                    value: '',
                    placeholder: '',
                    visible: true,
                    enabled: true,
                    validationProvider: false,
                    validationMessage: null,
                };
                const emitProviderState = (type) => send({
                    type,
                    id: String(id || ''),
                    providerId: String(id || ''),
                    label: String(label || id || ''),
                    rootUri: rootUri ? _plainBridgeValue(rootUri) : '',
                    contextValue,
                    acceptInputCommand: _plainBridgeValue(acceptInputCommand),
                    count,
                    hasQuickDiffProvider: !!quickDiffProvider,
                    quickDiffLabel: quickDiffProvider && quickDiffProvider.label
                        ? String(quickDiffProvider.label) : '',
                    hasHistoryProvider: !!historyProvider,
                    ..._scmHistoryCurrentRefs(historyProvider),
                    statusBarCommands: _plainBridgeValue(statusBarCommands),
                    actionButton: _plainBridgeValue(actionButton),
                    inputBox: { ...inputBoxState },
                });
                const clearHistoryProviderListeners = () => {
                    for (const disposable of historyProviderDisposables) {
                        try { disposable && disposable.dispose && disposable.dispose(); }
                        catch (_) {}
                    }
                    historyProviderDisposables = [];
                };
                const bindHistoryProviderListeners = () => {
                    clearHistoryProviderListeners();
                    if (!historyProvider) return;
                    if (typeof historyProvider.onDidChangeCurrentHistoryItemRefs === 'function') {
                        historyProviderDisposables.push(
                            historyProvider.onDidChangeCurrentHistoryItemRefs(() => {
                                emitProviderState('scm_provider_updated');
                            }));
                    }
                    if (typeof historyProvider.onDidChangeHistoryItemRefs === 'function') {
                        historyProviderDisposables.push(
                            historyProvider.onDidChangeHistoryItemRefs((event) => {
                                send({
                                    type: 'scm_history_refs_changed',
                                    providerId: String(id || ''),
                                    added: _plainBridgeValue(event && event.added || []),
                                    modified: _plainBridgeValue(event && event.modified || []),
                                    removed: _plainBridgeValue(event && event.removed || []),
                                    silent: !!(event && event.silent),
                                });
                                emitProviderState('scm_provider_updated');
                            }));
                    }
                };
                const inputBox = {
                    get value() { return inputBoxState.value; },
                    set value(value) {
                        inputBoxState.value = String(value || '');
                        emitProviderState('scm_provider_updated');
                    },
                    get placeholder() { return inputBoxState.placeholder; },
                    set placeholder(value) {
                        inputBoxState.placeholder = String(value || '');
                        emitProviderState('scm_provider_updated');
                    },
                    get visible() { return inputBoxState.visible; },
                    set visible(value) {
                        inputBoxState.visible = !!value;
                        emitProviderState('scm_provider_updated');
                    },
                    get enabled() { return inputBoxState.enabled; },
                    set enabled(value) {
                        inputBoxState.enabled = !!value;
                        emitProviderState('scm_provider_updated');
                    },
                    get validateInput() { return validateInput; },
                    set validateInput(value) {
                        if (value && typeof value !== 'function') {
                            throw new Error('Invalid SCM input box validation function');
                        }
                        validateInput = value || undefined;
                        inputBoxState.validationProvider = !!validateInput;
                        inputBoxState.validationMessage = null;
                        emitProviderState('scm_provider_updated');
                    },
                    showValidationMessage(message, type) {
                        inputBoxState.validationMessage = _normalizeScmInputValidation({
                            message,
                            type,
                        });
                        emitProviderState('scm_provider_updated');
                    },
                    async _validateInput(value, cursorPosition) {
                        if (typeof validateInput !== 'function') {
                            inputBoxState.validationMessage = null;
                            emitProviderState('scm_provider_updated');
                            return null;
                        }
                        const result = await validateInput(
                            String(value || ''),
                            Number.isFinite(Number(cursorPosition))
                                ? Number(cursorPosition) : 0);
                        const validation = _normalizeScmInputValidation(result);
                        inputBoxState.validationMessage = validation;
                        emitProviderState('scm_provider_updated');
                        return validation;
                    },
                };
                const sc = {
                    id,
                    label,
                    rootUri: rootUri || null,
                    inputBox,
                    get count() { return count; },
                    set count(value) {
                        const numeric = Number(value);
                        count = Number.isFinite(numeric) ? numeric : 0;
                        emitProviderState('scm_provider_updated');
                    },
                    get quickDiffProvider() { return quickDiffProvider; },
                    set quickDiffProvider(value) {
                        quickDiffProvider = value;
                        emitProviderState('scm_provider_updated');
                    },
                    get historyProvider() { return historyProvider; },
                    set historyProvider(value) {
                        historyProvider = value || undefined;
                        bindHistoryProviderListeners();
                        emitProviderState('scm_provider_updated');
                    },
                    get statusBarCommands() { return statusBarCommands; },
                    set statusBarCommands(value) {
                        statusBarCommands = Array.isArray(value)
                            ? value.slice() : value;
                        emitProviderState('scm_provider_updated');
                    },
                    get actionButton() { return actionButton; },
                    set actionButton(value) {
                        actionButton = value;
                        emitProviderState('scm_provider_updated');
                    },
                    get contextValue() { return contextValue; },
                    set contextValue(value) {
                        contextValue = String(value || '');
                        emitProviderState('scm_provider_updated');
                    },
                    get acceptInputCommand() { return acceptInputCommand; },
                    set acceptInputCommand(value) {
                        acceptInputCommand = value;
                        emitProviderState('scm_provider_updated');
                    },
                    createResourceGroup(groupId, groupLabel) {
                        const normalizedGroupId = String(groupId || '');
                        let groupContextValue = '';
                        let groupResourceStates = [];
                        const serializeResourceState = (state) => {
                            const resourceUri = state && state.resourceUri
                                ? _plainBridgeValue(state.resourceUri) : '';
                            return {
                                resourceUri,
                                contextValue: String(
                                    state && state.contextValue || ''),
                                command: _plainBridgeValue(
                                    state && state.command || undefined),
                            };
                        };
                        const emitGroupState = (type) => send({
                            type,
                            providerId: String(id || ''),
                            groupId: normalizedGroupId,
                            label: String(groupLabel || normalizedGroupId),
                            contextValue: groupContextValue,
                            resourceStates: groupResourceStates
                                .map(serializeResourceState),
                        });
                        const group = {
                            id: normalizedGroupId,
                            label: groupLabel,
                            hideWhenEmpty: false,
                            get contextValue() { return groupContextValue; },
                            set contextValue(value) {
                                groupContextValue = String(value || '');
                                emitGroupState('scm_resource_group_updated');
                            },
                            get resourceStates() {
                                return groupResourceStates.slice();
                            },
                            set resourceStates(value) {
                                groupResourceStates = Array.isArray(value)
                                    ? value.slice() : [];
                                emitGroupState('scm_resource_group_updated');
                            },
                            dispose() {
                                groups.delete(normalizedGroupId);
                                emitGroupState('scm_resource_group_disposed');
                            },
                        };
                        groups.set(normalizedGroupId, group);
                        emitGroupState('scm_resource_group_registered');
                        return group;
                    },
                    dispose() {
                        for (const group of groups.values()) {
                            if (group && typeof group.dispose === 'function') {
                                group.dispose();
                            }
                        }
                        groups.clear();
                        clearHistoryProviderListeners();
                        _scmProviders.delete(id);
                        emitProviderState('scm_provider_disposed');
                        log(`scm: disposed "${id}"`);
                    },
                };
                _scmProviders.set(id, sc);
                emitProviderState('scm_provider_registered');
                return sc;
            },
        },

        // --- Namespace: debug ---
        debug: (() => {
            const _onDidStartDebugSession = new EventEmitter();
            const _onDidTerminateDebugSession = new EventEmitter();
            const _onDidChangeActiveDebugSession = new EventEmitter();
            const _onDidReceiveDebugSessionCustomEvent = new EventEmitter();
            const _onDidChangeActiveStackItem = new EventEmitter();
            const _onDidChangeBreakpoints = new EventEmitter();
            const _breakpoints = [];
            const _knownBreakpointSourceUris = new Map();
            let _activeDebugStackItem = undefined;
            const protocolBreakpointFor = (breakpoint) => {
                if (!breakpoint) return undefined;
                const idNumber = Number(String(breakpoint.id || '').replace(/^\D+/, ''));
                const payload = {
                    id: Number.isFinite(idNumber) ? idNumber : undefined,
                    verified: breakpoint.enabled !== false,
                };
                if (breakpoint instanceof SourceBreakpoint && breakpoint.location) {
                    const range = breakpoint.location.range || {};
                    const start = range.start || {};
                    payload.source = {
                        path: breakpoint.location.uri && breakpoint.location.uri.fsPath,
                        name: breakpoint.location.uri
                            ? path.basename(breakpoint.location.uri.fsPath || breakpoint.location.uri.path || '')
                            : undefined,
                    };
                    payload.line = Number(start.line || 0) + 1;
                    payload.column = Number(start.character || 0) + 1;
                } else if (breakpoint instanceof FunctionBreakpoint) {
                    payload.name = breakpoint.functionName;
                } else if (breakpoint instanceof DataBreakpoint) {
                    payload.dataId = breakpoint.dataId;
                    payload.accessType = breakpoint.accessType;
                }
                return payload;
            };
            const addBreakpoints = (items) => {
                const added = [];
                for (const breakpoint of Array.isArray(items) ? items : []) {
                    if (!(breakpoint instanceof Breakpoint)) continue;
                    if (_breakpoints.includes(breakpoint)) continue;
                    _breakpoints.push(breakpoint);
                    added.push(breakpoint);
                }
                if (added.length) {
                    _onDidChangeBreakpoints.fire({
                        added: added.slice(),
                        removed: [],
                        changed: [],
                    });
                    syncActiveDebugBreakpoints();
                }
            };
            const removeBreakpoints = (items) => {
                const removed = [];
                for (const breakpoint of Array.isArray(items) ? items : []) {
                    const index = _breakpoints.indexOf(breakpoint);
                    if (index < 0) continue;
                    removed.push(..._breakpoints.splice(index, 1));
                }
                if (removed.length) {
                    _onDidChangeBreakpoints.fire({
                        added: [],
                        removed: removed.slice(),
                        changed: [],
                    });
                    syncActiveDebugBreakpoints();
                }
            };
            function breakpointSourceKey(uri) {
                return String(uri?.fsPath || uri?.path || uri || '');
            }
            function sourceDescriptorFor(uri) {
                const sourcePath = breakpointSourceKey(uri);
                return {
                    path: sourcePath,
                    name: sourcePath ? path.basename(sourcePath) : undefined,
                };
            }
            function sourceBreakpointPayload(breakpoint) {
                const start = breakpoint.location?.range?.start || {};
                return {
                    line: Number(start.line || 0) + 1,
                    column: Number(start.character || 0) + 1,
                    condition: breakpoint.condition,
                    hitCondition: breakpoint.hitCondition,
                    logMessage: breakpoint.logMessage,
                };
            }
            async function synchronizeBreakpointsToTransport(transport) {
                if (!transport || transport.error) return [];
                const syncLog = [];
                const sourceGroups = new Map();
                const functionBreakpoints = [];
                const dataBreakpoints = [];
                for (const breakpoint of _breakpoints) {
                    if (breakpoint instanceof SourceBreakpoint && breakpoint.location?.uri) {
                        const key = breakpointSourceKey(breakpoint.location.uri);
                        if (!sourceGroups.has(key)) {
                            sourceGroups.set(key, {
                                uri: breakpoint.location.uri,
                                breakpoints: [],
                            });
                        }
                        sourceGroups.get(key).breakpoints.push(
                            sourceBreakpointPayload(breakpoint));
                        _knownBreakpointSourceUris.set(key, breakpoint.location.uri);
                    } else if (breakpoint instanceof FunctionBreakpoint) {
                        functionBreakpoints.push({
                            name: breakpoint.functionName,
                            condition: breakpoint.condition,
                            hitCondition: breakpoint.hitCondition,
                        });
                    } else if (breakpoint instanceof DataBreakpoint) {
                        dataBreakpoints.push({
                            dataId: breakpoint.dataId,
                            accessType: breakpoint.accessType,
                            condition: breakpoint.condition,
                            hitCondition: breakpoint.hitCondition,
                        });
                    }
                }
                for (const [key, uri] of _knownBreakpointSourceUris.entries()) {
                    const group = sourceGroups.get(key) || { uri, breakpoints: [] };
                    await transport.sendRequest('setBreakpoints', {
                        source: sourceDescriptorFor(group.uri),
                        breakpoints: group.breakpoints,
                        lines: group.breakpoints.map(item => item.line),
                    });
                    syncLog.push({
                        command: 'setBreakpoints',
                        source: breakpointSourceKey(group.uri),
                        count: group.breakpoints.length,
                    });
                }
                await transport.sendRequest('setFunctionBreakpoints', {
                    breakpoints: functionBreakpoints,
                });
                syncLog.push({
                    command: 'setFunctionBreakpoints',
                    count: functionBreakpoints.length,
                });
                await transport.sendRequest('setDataBreakpoints', {
                    breakpoints: dataBreakpoints,
                });
                syncLog.push({
                    command: 'setDataBreakpoints',
                    count: dataBreakpoints.length,
                });
                transport.breakpointSyncLog = syncLog;
                return syncLog;
            }
            function syncActiveDebugBreakpoints() {
                const session = _activeDebugSession;
                const transport = session && session._debugAdapterTransport;
                if (!transport) return;
                const run = async () => {
                    if (transport.ready) await transport.ready;
                    await synchronizeBreakpointsToTransport(transport);
                };
                run().catch(err => _debugCallTrackers(session, 'onError', err));
            }
            async function sendActiveDebugAdapterRequest(command, args = {}) {
                const session = _activeDebugSession;
                const transport = session && session._debugAdapterTransport;
                if (!transport) return false;
                if (transport.ready) await transport.ready;
                if (transport.error) return false;
                return await transport.sendRequest(command, args);
            }
            function activeDebugThreadArgs() {
                const threadId = _activeDebugStackItem?.thread?.id;
                return threadId === undefined || threadId === null ? {} : { threadId };
            }
            function activeDebugEvaluateArgs(expression, frameId, context = 'repl') {
                const payload = {
                    expression: String(expression ?? ''),
                    context: String(context || 'repl'),
                };
                const explicitFrameId = Number(frameId);
                if (Number.isFinite(explicitFrameId) && explicitFrameId > 0) {
                    payload.frameId = explicitFrameId;
                } else {
                    const activeFrameId = Number(_activeDebugStackItem?.frame?.id);
                    if (Number.isFinite(activeFrameId) && activeFrameId > 0) {
                        payload.frameId = activeFrameId;
                    }
                }
                return payload;
            }
            function stopDebugSession(target) {
                target = target || _activeDebugSession;
                if (!target) return Promise.resolve(false);
                _debugCallTrackers(target, 'onWillStopSession');
                try { target._debugAdapterTransport?.dispose?.(); } catch {}
                if (_activeDebugSession && _activeDebugSession.id === target.id) {
                    _debugUpdateActive(null, _onDidChangeActiveDebugSession);
                }
                if (_activeDebugStackItem
                        && _activeDebugStackItem.session
                        && _activeDebugStackItem.session.id === target.id) {
                    _activeDebugStackItem = undefined;
                    _onDidChangeActiveStackItem.fire(undefined);
                }
                _onDidTerminateDebugSession.fire(target);
                send({
                    type: 'debug_stop',
                    session: _debugSessionPayload(target),
                });
                _debugCallTrackers(target, 'onExit', undefined, undefined);
                return Promise.resolve(true);
            }
            const debugCommandSpecs = [
                ['workbench.action.debug.continue', 'continue', true],
                ['workbench.action.debug.stepOver', 'next', true],
                ['workbench.action.debug.stepInto', 'stepIn', true],
                ['workbench.action.debug.stepOut', 'stepOut', true],
                ['workbench.action.debug.pause', 'pause', true],
                ['workbench.action.debug.restart', 'restart', false],
            ];
            for (const [commandId, dapCommand, usesThread] of debugCommandSpecs) {
                if (!_commands.has(commandId)) {
                    _commands.set(commandId, () => sendActiveDebugAdapterRequest(
                        dapCommand,
                        usesThread ? activeDebugThreadArgs() : {},
                    ));
                }
            }
            if (!_commands.has('workbench.action.debug.stop')) {
                _commands.set('workbench.action.debug.stop', () => stopDebugSession());
            }
            if (!_commands.has('workbench.action.debug.evaluate')) {
                _commands.set('workbench.action.debug.evaluate',
                    (expression, frameId, context = 'repl') =>
                        sendActiveDebugAdapterRequest(
                            'evaluate',
                            activeDebugEvaluateArgs(expression, frameId, context),
                        ));
            }
            if (!_commands.has('workbench.debug.action.evaluateRepl')) {
                _commands.set('workbench.debug.action.evaluateRepl',
                    (expression, frameId) =>
                        _commands.get('workbench.action.debug.evaluate')(
                            expression, frameId, 'repl'));
            }
            if (!_commands.has('repl.action.acceptInput')) {
                _commands.set('repl.action.acceptInput',
                    (expression, frameId) =>
                        _commands.get('workbench.action.debug.evaluate')(
                            expression, frameId, 'repl'));
            }
            if (!_commands.has('workbench.panel.repl.view.focus')) {
                _commands.set('workbench.panel.repl.view.focus', () => {
                    send({
                        type: 'debug_console_focus',
                        session: _debugSessionPayload(_activeDebugSession),
                    });
                    return true;
                });
            }
            if (!_commands.has('repl.action.copyAll')) {
                _commands.set('repl.action.copyAll', () => {
                    send({
                        type: 'debug_console_copy_all',
                        session: _debugSessionPayload(_activeDebugSession),
                    });
                    return true;
                });
            }
            const activeDebugConsole = {
                append(value) {
                    if (!value) return;
                    send({
                        type: 'debug_console',
                        text: String(value),
                        newline: false,
                    });
                },
                appendLine(value) {
                    send({
                        type: 'debug_console',
                        text: String(value ?? ''),
                        newline: true,
                    });
                },
            };
            return {
                registerDebugAdapterDescriptorFactory(type, factory) {
                    const key = String(type || '');
                    const handle = _nextDebugAdapterFactoryHandle++;
                    _debugAdapterFactories.set(key, factory);
                    _debugAdapterFactoryStates.set(key, {
                        handle,
                        type: key,
                        extensionId: extDesc.extensionId || '',
                        hasCreateDebugAdapterDescriptor: !!(factory && typeof factory.createDebugAdapterDescriptor === 'function'),
                    });
                    send({
                        type: 'debug_adapter_factory_registered',
                        handle,
                        debugType: key,
                        extensionId: extDesc.extensionId || '',
                        hasCreateDebugAdapterDescriptor: !!(factory && typeof factory.createDebugAdapterDescriptor === 'function'),
                    });
                    log(`debug: registered adapter factory for "${type}"`);
                    return new Disposable(() => {
                        _debugAdapterFactories.delete(key);
                        _debugAdapterFactoryStates.delete(key);
                        send({
                            type: 'debug_adapter_factory_disposed',
                            handle,
                            debugType: key,
                            extensionId: extDesc.extensionId || '',
                        });
                    });
                },
                registerDebugAdapterTrackerFactory(type, factory) {
                    const handle = _nextDebugAdapterTrackerHandle++;
                    const entry = {
                        handle,
                        type: String(type || '*'),
                        extensionId: extDesc.extensionId || '',
                        factory,
                    };
                    _debugAdapterTrackerFactories.push(entry);
                    send({
                        type: 'debug_adapter_tracker_registered',
                        handle,
                        debugType: entry.type,
                        extensionId: extDesc.extensionId || '',
                        hasCreateDebugAdapterTracker: !!(factory && typeof factory.createDebugAdapterTracker === 'function'),
                    });
                    log(`debug: registered adapter tracker for "${entry.type}"`);
                    return new Disposable(() => {
                        const index = _debugAdapterTrackerFactories.indexOf(entry);
                        if (index >= 0) _debugAdapterTrackerFactories.splice(index, 1);
                        send({
                            type: 'debug_adapter_tracker_disposed',
                            handle,
                            debugType: entry.type,
                            extensionId: extDesc.extensionId || '',
                        });
                    });
                },
                // registerDebugVisualizationTreeProvider/
                // registerDebugVisualizationProvider were entirely missing -
                // real gap, confirmed root cause of "r.debug.
                // registerDebugVisualizationTreeProvider is not a function"
                // in ms-python.debugpy (2026-07-02 sweep), which registers
                // its variable-visualization tree during activation. Same
                // scope as the tracker/adapter factory registries above:
                // a real registry so activation doesn't crash, without a
                // Debug Console variable-visualization UI to consume it
                // (this app has no such panel yet - a separate, larger
                // feature).
                registerDebugVisualizationTreeProvider(id, provider) {
                    const key = String(id || '');
                    _debugVisualizationTreeProviders.set(key, provider);
                    return new Disposable(() => _debugVisualizationTreeProviders.delete(key));
                },
                registerDebugVisualizationProvider(id, provider) {
                    const key = String(id || '');
                    _debugVisualizationProviders.set(key, provider);
                    return new Disposable(() => _debugVisualizationProviders.delete(key));
                },
                registerDebugConfigurationProvider(type, provider, triggerKind = 1) {
                    const key = String(type || '');
                    const handle = _nextDebugConfigProviderHandle++;
                    const entry = {
                        handle,
                        provider,
                        triggerKind,
                        extensionId: extDesc.extensionId || '',
                        hasProvideDebugConfigurations: !!(provider && typeof provider.provideDebugConfigurations === 'function'),
                        hasResolveDebugConfiguration: !!(provider && typeof provider.resolveDebugConfiguration === 'function'),
                        hasResolveDebugConfigurationWithSubstitutedVariables: !!(provider && typeof provider.resolveDebugConfigurationWithSubstitutedVariables === 'function'),
                    };
                    const list = _debugConfigProviders.get(key) || [];
                    list.push(entry);
                    _debugConfigProviders.set(key, list);
                    send({
                        type: 'debug_config_provider_registered',
                        handle,
                        debugType: key,
                        extensionId: extDesc.extensionId || '',
                        triggerKind: Number(triggerKind || 1),
                        hasProvideDebugConfigurations: entry.hasProvideDebugConfigurations,
                        hasResolveDebugConfiguration: entry.hasResolveDebugConfiguration,
                        hasResolveDebugConfigurationWithSubstitutedVariables: entry.hasResolveDebugConfigurationWithSubstitutedVariables,
                    });
                    log(`debug: registered config provider for "${type}"`);
                    return new Disposable(() => {
                        const current = _debugConfigProviders.get(key) || [];
                        const next = current.filter(item => item !== entry);
                        if (next.length) _debugConfigProviders.set(key, next);
                        else _debugConfigProviders.delete(key);
                        send({
                            type: 'debug_config_provider_disposed',
                            handle,
                            debugType: key,
                            extensionId: extDesc.extensionId || '',
                        });
                    });
                },
                async startDebugging(folder, config, options) {
                    log('debug: startDebugging');
                    if (typeof config === 'string') {
                        const wantedName = config;
                        await _activateKnownExtensionsForEvent('onDebug');
                        let providedConfig = null;
                        for (const entries of _debugConfigProviders.values()) {
                            for (const entry of entries || []) {
                                const provider = entry && entry.provider;
                                if (provider
                                        && typeof provider.provideDebugConfigurations === 'function') {
                                    const provided = await Promise.resolve(
                                        provider.provideDebugConfigurations(
                                            folder, _debugProviderToken()));
                                    const match = (Array.isArray(provided) ? provided : [])
                                        .find(item => item && item.name === wantedName);
                                    if (match) {
                                        providedConfig = match;
                                        break;
                                    }
                                }
                            }
                            if (providedConfig) break;
                        }
                        config = providedConfig;
                    }
                    if (!config || typeof config !== 'object') return false;
                    const debugType = String(config.type || '');
                    await _activateKnownExtensionsForEvent('onDebug');
                    if (debugType) {
                        await _activateKnownExtensionsForEvent(
                            `onDebugResolve:${debugType}`);
                    }
                    const providers = _debugConfigProviders.get(debugType) || [];
                    for (const entry of providers) {
                        const provider = entry && entry.provider;
                        if (provider
                                && typeof provider.resolveDebugConfiguration === 'function') {
                            const resolved = await Promise.resolve(
                                provider.resolveDebugConfiguration(
                                    folder, { ...config }, _debugProviderToken()));
                            if (!resolved) return false;
                            config = resolved;
                        }
                    }
                    const normalizedOptions = options && options.id && options.type
                        ? { parentSession: options }
                        : (options && typeof options === 'object' ? options : {});
                    const session = {
                        id: `debug-${_nextDebugSessionHandle++}`,
                        type: String(config.type || debugType || 'debug'),
                        name: String(config.name || config.type || 'Debug'),
                        workspaceFolder: folder || undefined,
                        configuration: { ...config },
                        parentSession: normalizedOptions.parentSession,
                        async customRequest(command, args) {
                            const transport = session._debugAdapterTransport;
                            if (transport) {
                                try {
                                    if (transport.ready) await transport.ready;
                                    if (!transport.error) {
                                        return await transport.sendRequest(command, args);
                                    }
                                } catch (err) {
                                    _debugCallTrackers(session, 'onError', err);
                                }
                            }
                            const request = {
                                type: 'request',
                                command: String(command || ''),
                                arguments: _plainBridgeValue(args || {}),
                            };
                            _debugCallTrackers(session, 'onWillReceiveMessage', request);
                            const event = {
                                type: 'event',
                                event: String(command || ''),
                                body: _plainBridgeValue(args || {}),
                            };
                            _debugCallTrackers(session, 'onDidSendMessage', event);
                            _onDidReceiveDebugSessionCustomEvent.fire({
                                session,
                                event: event.event,
                                body: event.body,
                            });
                            return Promise.resolve({ command: event.event, body: event.body });
                        },
                        getDebugProtocolBreakpoint(breakpoint) {
                            return Promise.resolve(protocolBreakpointFor(breakpoint));
                        },
                    };
                    if (session.type) {
                        await _activateKnownExtensionsForEvent(
                            `onDebugAdapterProtocolTracker:${session.type}`);
                    }
                    const trackers = [];
                    for (const entry of _debugTrackerEntriesForType(session.type)) {
                        const factory = entry && entry.factory;
                        if (factory
                                && typeof factory.createDebugAdapterTracker === 'function') {
                            const tracker = await Promise.resolve(
                                factory.createDebugAdapterTracker(session));
                            if (tracker) trackers.push(tracker);
                        }
                    }
                    session._debugTrackers = trackers;
                    if (typeof config.debugServer === 'number') {
                        session.adapterDescriptor = new DebugAdapterServer(
                            config.debugServer);
                    }
                    const descriptorFactory = _debugAdapterFactories.get(session.type);
                    const packageExecutable = _debugExecutableFromPackage(session.type);
                    if (!session.adapterDescriptor
                            && descriptorFactory
                            && typeof descriptorFactory.createDebugAdapterDescriptor === 'function') {
                        const descriptor = await Promise.resolve(
                            descriptorFactory.createDebugAdapterDescriptor(
                                session,
                                packageExecutable));
                        if (descriptor) session.adapterDescriptor = descriptor;
                    }
                    if (!session.adapterDescriptor && packageExecutable) {
                        session.adapterDescriptor = packageExecutable;
                    }
                    session._debugAdapterTransport = _debugCreateAdapterTransport(
                        session,
                        session.adapterDescriptor,
                        config,
                        _onDidReceiveDebugSessionCustomEvent,
                        transport => synchronizeBreakpointsToTransport(transport));
                    if (session._debugAdapterTransport) {
                        session._debugAdapterTransport.onActiveStackItem = item => {
                            _activeDebugStackItem = item;
                            _onDidChangeActiveStackItem.fire(item);
                        };
                    }
                    _debugCallTrackers(session, 'onWillStartSession');
                    _debugUpdateActive(session, _onDidChangeActiveDebugSession);
                    _onDidStartDebugSession.fire(session);
                    send({
                        type: 'debug_start',
                        session: _debugSessionPayload(session),
                        config: _plainBridgeValue(config || {}),
                    });
                    _debugCallTrackers(session, 'onDidSendMessage', {
                        type: 'event',
                        event: 'initialized',
                    });
                    return true;
                },
                stopDebugging(session) {
                    return stopDebugSession(session).then(() => undefined);
                },
                get activeDebugSession() { return _activeDebugSession; },
                get activeDebugConsole() { return activeDebugConsole; },
                get breakpoints() { return _breakpoints.slice(); },
                addBreakpoints,
                removeBreakpoints,
                onDidChangeActiveDebugSession: _onDidChangeActiveDebugSession.event,
                onDidStartDebugSession: _onDidStartDebugSession.event,
                onDidReceiveDebugSessionCustomEvent:
                    _onDidReceiveDebugSessionCustomEvent.event,
                onDidTerminateDebugSession: _onDidTerminateDebugSession.event,
                onDidChangeBreakpoints: _onDidChangeBreakpoints.event,
                get activeStackItem() { return _activeDebugStackItem; },
                onDidChangeActiveStackItem: _onDidChangeActiveStackItem.event,
                _onDidStartDebugSession,
                _onDidTerminateDebugSession,
                _onDidChangeActiveDebugSession,
            };
        })(),

        // --- Namespace: tasks ---
        tasks: (() => {
            const _onDidStartTask = new EventEmitter();
            const _onDidEndTask = new EventEmitter();
            const _onDidStartTaskProcess = new EventEmitter();
            const _onDidEndTaskProcess = new EventEmitter();
            const endExecution = (execution, exitCode = undefined) => {
                if (!execution || execution._ended) return;
                execution._ended = true;
                try { execution._customPtyCloseDisposable?.dispose?.(); } catch {}
                try { execution._customPtyWriteDisposable?.dispose?.(); } catch {}
                const index = _taskExecutions.indexOf(execution);
                if (index >= 0) _taskExecutions.splice(index, 1);
                _onDidEndTask.fire({ execution, task: execution.task });
                _onDidEndTaskProcess.fire({ execution, exitCode });
            };
            const terminateTaskExecution = async (executionId) => {
                const wanted = String(executionId || '').trim();
                const target = wanted
                    ? _taskExecutions.find(item => item && String(item.id || '') === wanted)
                    : _taskExecutions[_taskExecutions.length - 1];
                if (!target || typeof target.terminate !== 'function') {
                    return false;
                }
                await Promise.resolve(target.terminate());
                return true;
            };
            if (!_commands.has('workbench.action.tasks.terminate')) {
                _commands.set('workbench.action.tasks.terminate', terminateTaskExecution);
            }
            if (!_commands.has('workbench.action.tasks.terminateTask')) {
                _commands.set('workbench.action.tasks.terminateTask', terminateTaskExecution);
            }
            return {
                registerTaskProvider(type, provider) {
                    const key = String(type || '');
                    const handle = _nextTaskProviderHandle++;
                    _taskProviders.set(key, provider);
                    _taskProviderStates.set(key, {
                        handle,
                        type: key,
                        extensionId: extDesc.extensionId || '',
                        hasProvideTasks: !!(provider && typeof provider.provideTasks === 'function'),
                        hasResolveTask: !!(provider && typeof provider.resolveTask === 'function'),
                    });
                    send({
                        type: 'task_provider_registered',
                        handle,
                        taskType: key,
                        extensionId: extDesc.extensionId || '',
                        hasProvideTasks: !!(provider && typeof provider.provideTasks === 'function'),
                        hasResolveTask: !!(provider && typeof provider.resolveTask === 'function'),
                    });
                    log(`tasks: registered provider for "${type}"`);
                    return new Disposable(() => {
                        _taskProviders.delete(key);
                        _taskProviderStates.delete(key);
                        send({
                            type: 'task_provider_disposed',
                            handle,
                            taskType: key,
                            extensionId: extDesc.extensionId || '',
                        });
                    });
                },
                async fetchTasks(filter) {
                    log('tasks: fetchTasks');
                    const taskTypeFilter = filter && typeof filter === 'object'
                        ? String(filter.type || '').trim()
                        : '';
                    if (taskTypeFilter) {
                        await _activateKnownExtensionsForEvent(
                            `onTaskType:${taskTypeFilter}`);
                    } else {
                        await _activateKnownExtensionsForEventPrefix('onTaskType:');
                    }
                    const result = [];
                    for (const [taskType, provider] of _taskProviders.entries()) {
                        const provided = await _callTaskProvider(provider, 'provideTasks');
                        for (const task of Array.isArray(provided) ? provided : []) {
                            if (task && typeof task === 'object'
                                    && task.definition && !task.definition.type) {
                                task.definition.type = taskType;
                            }
                            if (_taskMatchesFilter(task, filter)) result.push(task);
                        }
                    }
                    return result;
                },
                async executeTask(task) {
                    log('tasks: executeTask');
                    const resolved = await _resolveTask(task);
                    const execution = {
                        id: `task-${_nextTaskExecutionHandle++}`,
                        task: resolved,
                        terminate() {
                            if (execution._customPty && typeof execution._customPty.close === 'function') {
                                try { execution._customPty.close(); } catch {}
                            }
                            send({
                                type: 'task_terminate',
                                executionId: execution.id,
                                task: _serializeTask(resolved),
                                metadata: execution._bridgeMetadata || {},
                            });
                            endExecution(execution, undefined);
                            return Promise.resolve();
                        },
                    };
                    const executionSpec = _taskExecutionSpec(resolved);
                    const bridgeMetadata = _taskBridgeMetadata(
                        resolved, executionSpec, { executionId: execution.id });
                    execution._bridgeMetadata = bridgeMetadata;
                    _taskExecutions.push(execution);
                    _onDidStartTask.fire({ execution, task: resolved });
                    _onDidStartTaskProcess.fire({ execution, processId: 0 });
                    send({
                        type: 'task_execute',
                        executionId: execution.id,
                        task: _serializeTask(resolved),
                        commandLine: executionSpec.commandLine || '',
                        metadata: bridgeMetadata,
                    });
                    if (resolved.execution instanceof CustomExecution) {
                        try {
                            const pty = await Promise.resolve(
                                resolved.execution.callback(resolved.definition || {}));
                            if (pty && typeof pty === 'object') {
                                execution._customPty = pty;
                                execution._customPtyWriteDisposable =
                                    _terminalSubscribeEvent(pty.onDidWrite, data => {
                                        send({
                                            type: 'task_write',
                                            executionId: execution.id,
                                            text: String(data ?? ''),
                                        });
                                    });
                                execution._customPtyCloseDisposable =
                                    _terminalSubscribeEvent(pty.onDidClose, code => {
                                        endExecution(
                                            execution,
                                            typeof code === 'number' ? code : undefined);
                                    });
                                if (typeof pty.open === 'function') {
                                    pty.open(undefined);
                                }
                            }
                        } catch (err) {
                            endExecution(execution, 1);
                            throw err;
                        }
                    }
                    return execution;
                },
                onDidStartTask: _onDidStartTask.event,
                onDidEndTask: _onDidEndTask.event,
                onDidStartTaskProcess: _onDidStartTaskProcess.event,
                onDidEndTaskProcess: _onDidEndTaskProcess.event,
                get taskExecutions() { return _taskExecutions.slice(); },
                _onDidStartTask,
                _onDidEndTask,
                _onDidStartTaskProcess,
                _onDidEndTaskProcess,
            };
        })(),

        // --- Namespace: tests (Testing API) ---
        tests: {
            createTestController(id, label) {
                return _createTestController(id, label);
            },
        },
        TestRunProfileKind: { Run: 1, Debug: 2, Coverage: 3 },
        TestTag,
        TestMessage,
        TestRunRequest,

        // --- Types used by some extensions ---
        ThemeIcon,
        ThemeColor,
        ProcessExecution,
        ShellExecution,
        CustomExecution,
        Task,
        TaskGroup,
        Breakpoint,
        SourceBreakpoint,
        FunctionBreakpoint,
        DataBreakpoint,
        DebugAdapterExecutable,
        DebugAdapterServer,
        DebugAdapterNamedPipeServer,
        DebugAdapterInlineImplementation,
        FileDecoration,
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
        CompletionTriggerKind: { Invoke: 0, TriggerCharacter: 1, TriggerForIncompleteCompletions: 2 },
        CodeAction: class {
            constructor(title, kind) {
                this.title = title;
                this.kind = kind === undefined || kind === null
                    ? undefined
                    : _codeActionKindFromPayload(kind);
            }
        },
        CodeActionKind,
        CodeActionTriggerKind,
        Hover: class { constructor(contents, range) { this.contents = Array.isArray(contents) ? contents : [contents]; this.range = range; } },
        DocumentLink: class { constructor(range, target) { this.range = range; this.target = target; } },
        // vscode.Diagnostic/DiagnosticRelatedInformation/DiagnosticTag/
        // SnippetString were entirely missing from this module - a real,
        // very commonly used constructable type (any extension building its
        // own diagnostics client-side, or subclassing Diagnostic, hit
        // "Class extends value undefined" on this exact export). Confirmed
        // root cause of Vue.volar's activation failure (2026-07-02 real
        // extension sweep); several other language-support extensions hit
        // the identical error class and likely share this same gap.
        Diagnostic: class {
            constructor(range, message, severity) {
                this.range = range;
                this.message = message;
                this.severity = severity === undefined || severity === null ? 0 : severity;
                this.source = undefined;
                this.code = undefined;
                this.relatedInformation = undefined;
                this.tags = undefined;
            }
        },
        DiagnosticRelatedInformation: class {
            constructor(location, message) { this.location = location; this.message = message; }
        },
        DiagnosticTag: { Unnecessary: 1, Deprecated: 2 },
        SnippetString: class {
            constructor(value) { this.value = value === undefined || value === null ? '' : String(value); }
            appendText(text) { this.value += String(text ?? '').replace(/\$|}|\\/g, '\\$&'); return this; }
            appendTabstop(number) { this.value += '$' + (number ?? 0); return this; }
            appendPlaceholder(value, number) {
                const n = number ?? 0;
                const text = typeof value === 'function' ? '' : String(value ?? '');
                this.value += '${' + n + ':' + text.replace(/\$|}|\\/g, '\\$&') + '}';
                return this;
            }
            appendChoice(values, number) {
                const n = number ?? 0;
                this.value += '${' + n + '|' + (Array.isArray(values) ? values : []).join(',') + '|}';
                return this;
            }
            appendVariable(name, defaultValueOrFn) {
                const dv = typeof defaultValueOrFn === 'function' ? '' : String(defaultValueOrFn ?? '');
                this.value += dv ? ('${' + name + ':' + dv + '}') : ('$' + '{' + name + '}');
                return this;
            }
        },
        DocumentDropOrPasteEditKind,
        DocumentDropEdit,
        DocumentPasteEdit,
        DataTransfer,
        DataTransferItem,
        DocumentHighlight,
        EvaluatableExpression,
        InlineValueText,
        InlineValueVariableLookup,
        InlineValueEvaluatableExpression,
        InlineValueContext,
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
        FileChangeType,
        FilePermission: { Readonly: 1 },
        FileSystemError,
        CancellationError,
        EndOfLine: { LF: 1, CRLF: 2 },
        TextEditorRevealType: { Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3 },

        // --- Task types ---
        ShellExecution,
        ProcessExecution,
        CustomExecution,
        Task,
        TaskGroup,
        TaskScope: { Global: 1, Workspace: 2 },
        TaskRevealKind: { Always: 1, Silent: 2, Never: 3 },
        TaskPanelKind: { Shared: 1, Dedicated: 2, New: 3 },
        DebugConsoleMode: { Separate: 0, MergeWithParent: 1 },
        DebugConfigurationProviderTriggerKind: { Initial: 1, Dynamic: 2 },

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

function _configHasPath(data, pathParts) {
    return _configLookup(data, pathParts) !== _CONFIG_MISSING;
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

function _configLanguageIdsForPath(pathParts) {
    if (!Array.isArray(pathParts) || !pathParts.length) return [];
    const ids = new Set();
    for (const [languageId, defaults] of Object.entries(_configurationLanguageDefaults)) {
        if (_configHasPath(defaults, pathParts)) ids.add(languageId);
    }
    for (const key of Object.keys(_settings || {})) {
        for (const languageId of _configOverrideIdentifiersFromKey(key)) {
            const store = _settings[key];
            if (!store || typeof store !== 'object') continue;
            if (_configLanguageOverrideLookup(pathParts, languageId)
                    !== _CONFIG_MISSING) {
                ids.add(languageId);
            }
        }
    }
    return Array.from(ids).sort();
}

function _configShouldUpdateLanguage(pathParts, overrideIdentifier, overrideInLanguage) {
    if (overrideInLanguage === true) return !!overrideIdentifier;
    if (overrideInLanguage === false || !overrideIdentifier) return false;
    return _configLanguageDefaultLookup(pathParts, overrideIdentifier)
            !== _CONFIG_MISSING
        || _configLanguageOverrideLookup(pathParts, overrideIdentifier)
            !== _CONFIG_MISSING;
}

function _configDefaultLookup(pathParts, overrideIdentifier) {
    const languageDefault = _configLanguageDefaultLookup(
        pathParts, overrideIdentifier);
    if (languageDefault !== _CONFIG_MISSING) return languageDefault;
    return _configLookup(_configurationDefaults, pathParts);
}

function _configurationTargetName(target) {
    if (target === true || target === 1) return 'global';
    if (target === false || target === 2) return 'workspace';
    if (target === 3) return 'workspaceFolder';
    return '';
}

function _configurationTargetStore(targetName) {
    const name = String(targetName || '');
    return Object.prototype.hasOwnProperty.call(_configurationTargetValues, name)
        ? _configurationTargetValues[name]
        : null;
}

function _configurationDottedKey(pathParts) {
    return Array.isArray(pathParts) ? pathParts.join('.') : '';
}

function _configurationTrackedTarget(pathParts) {
    return _configurationUpdateTargets[_configurationDottedKey(pathParts)] || '';
}

function _configTargetLookup(pathParts, targetName) {
    const store = _configurationTargetStore(targetName);
    return store ? _configLookup(store, pathParts) : _CONFIG_MISSING;
}

function _configSetTargetValue(pathParts, targetName, value) {
    const store = _configurationTargetStore(targetName);
    const dotted = _configurationDottedKey(pathParts);
    if (!store || !dotted) return;
    _configSet(store, pathParts, _configCloneValue(value));
    _configurationUpdateTargets[dotted] = targetName;
}

function _configDeleteTargetValue(pathParts, targetName) {
    const store = _configurationTargetStore(targetName);
    const dotted = _configurationDottedKey(pathParts);
    if (!store || !dotted) return;
    _configDelete(store, pathParts);
    if (_configurationUpdateTargets[dotted] === targetName) {
        delete _configurationUpdateTargets[dotted];
    }
}

function _configWorkspaceLookup(pathParts) {
    const trackedTarget = _configurationTrackedTarget(pathParts);
    if (trackedTarget && trackedTarget !== 'workspace') return _CONFIG_MISSING;
    const workspaceValue = _configTargetLookup(pathParts, 'workspace');
    if (workspaceValue !== _CONFIG_MISSING) return workspaceValue;
    return _configLookup(_settings, pathParts);
}

function _rebuildConfigurationTargetsFromSettings() {
    for (const name of Object.keys(_configurationTargetValues)) {
        _configurationTargetValues[name] = {};
    }
    for (const key of Object.keys(_configurationUpdateTargets)) {
        delete _configurationUpdateTargets[key];
    }
    const aiEditor = _settings && typeof _settings === 'object'
        ? _settings.ai_editor
        : null;
    const targets = aiEditor && typeof aiEditor === 'object'
        ? aiEditor.configuration_targets
        : null;
    if (!targets || typeof targets !== 'object' || Array.isArray(targets)) {
        return;
    }
    for (const [key, entry] of Object.entries(targets)) {
        if (!entry || typeof entry !== 'object') continue;
        const pathParts = _configPath(key);
        const targetName = String(entry.target || '');
        if (!pathParts.length || !_configurationTargetStore(targetName)) continue;
        if (!Object.prototype.hasOwnProperty.call(entry, 'value')) continue;
        _configSetTargetValue(pathParts, targetName, entry.value);
    }
}

function _configEffectiveLookup(pathParts, overrideIdentifier) {
    const languageConfigured = _configLanguageOverrideLookup(
        pathParts, overrideIdentifier);
    if (languageConfigured !== _CONFIG_MISSING) return languageConfigured;
    const folderConfigured = _configTargetLookup(pathParts, 'workspaceFolder');
    if (folderConfigured !== _CONFIG_MISSING) return folderConfigured;
    const configured = _configWorkspaceLookup(pathParts);
    if (configured !== _CONFIG_MISSING) return configured;
    const globalConfigured = _configTargetLookup(pathParts, 'global');
    if (globalConfigured !== _CONFIG_MISSING) return globalConfigured;
    return _configDefaultLookup(pathParts, overrideIdentifier);
}

function _configEffectiveSection(sectionPath, overrideIdentifier) {
    const defaults = _configLookup(_configurationDefaults, sectionPath);
    const languageDefaults = _configLanguageDefaultLookup(
        sectionPath, overrideIdentifier);
    const globalConfigured = _configTargetLookup(sectionPath, 'global');
    const configured = _configWorkspaceLookup(sectionPath);
    const folderConfigured = _configTargetLookup(sectionPath, 'workspaceFolder');
    const languageConfigured = _configLanguageOverrideSection(
        sectionPath, overrideIdentifier);
    let result = _CONFIG_MISSING;
    result = _configMergeLayer(result, defaults);
    result = _configMergeLayer(result, languageDefaults);
    result = _configMergeLayer(result, globalConfigured);
    result = _configMergeLayer(result, configured);
    result = _configMergeLayer(result, folderConfigured);
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

function _fireConfigurationChanged(pathParts, overrideIdentifier = '') {
    const fullKey = pathParts.join('.');
    _onDidChangeConfigurationEmitter.fire({
        affectsConfiguration(sect, scope) {
            const probe = _configPath(sect).join('.');
            if (!probe) return true;
            const scopeOverride = _configOverrideIdentifierFromScope(scope);
            if (scopeOverride && overrideIdentifier
                    && scopeOverride !== overrideIdentifier) {
                return false;
            }
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
    const proxy = {
        get(key, defaultValue) {
            if (arguments.length === 0 || key === undefined) {
                const data = sectionData();
                return _configCloneValue(data);
            }
            const value = _configEffectiveLookup(
                _configFullPath(section, key), overrideIdentifier);
            return value === _CONFIG_MISSING
                ? defaultValue
                : _configCloneValue(value);
        },
        has(key) {
            return _configEffectiveLookup(
                _configFullPath(section, key), overrideIdentifier)
                !== _CONFIG_MISSING;
        },
        inspect(key) {
            const pathParts = _configFullPath(section, key);
            const fullKey = pathParts.join('.');
            const globalValue = _configTargetLookup(pathParts, 'global');
            const workspaceValue = _configWorkspaceLookup(pathParts);
            const workspaceFolderValue = _configTargetLookup(
                pathParts, 'workspaceFolder');
            const languageValue = _configLanguageOverrideLookup(
                pathParts, overrideIdentifier);
            const defaultValue = _configLookup(_configurationDefaults, pathParts);
            const defaultLanguageValue = _configLanguageDefaultLookup(
                pathParts, overrideIdentifier);
            const languageIds = _configLanguageIdsForPath(pathParts);
            if (globalValue === _CONFIG_MISSING
                    && workspaceValue === _CONFIG_MISSING
                    && workspaceFolderValue === _CONFIG_MISSING
                    && languageValue === _CONFIG_MISSING
                    && defaultValue === _CONFIG_MISSING
                    && defaultLanguageValue === _CONFIG_MISSING
                    && !languageIds.length) {
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
                globalValue: globalValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(globalValue),
                workspaceValue: workspaceValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(workspaceValue),
                globalLanguageValue: languageValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(languageValue),
                workspaceLanguageValue: languageValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(languageValue),
                workspaceFolderValue: workspaceFolderValue === _CONFIG_MISSING
                    ? undefined
                    : _configCloneValue(workspaceFolderValue),
                workspaceFolderLanguageValue: undefined,
                languageIds,
                target: _configurationUpdateTargets[fullKey] || undefined,
            };
        },
        update(key, value, configTarget, overrideInLanguage) {
            const pathParts = _configFullPath(section, key);
            const fullKey = pathParts.join('.');
            const targetName = _configurationTargetName(configTarget);
            const languageOverrideIdentifier = _configShouldUpdateLanguage(
                pathParts, overrideIdentifier, overrideInLanguage)
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
                const targetForStore = targetName || 'workspace';
                if (value === undefined) {
                    _configDeleteTargetValue(pathParts, targetForStore);
                    if (!targetName || targetName === 'workspace') {
                        _configDelete(_settings, pathParts);
                        _configMirrorAiEditorAlias(pathParts, value, true);
                    }
                } else {
                    _configSetTargetValue(pathParts, targetForStore, value);
                    if (!targetName || targetName === 'workspace') {
                        _configSet(_settings, pathParts, value);
                        _configMirrorAiEditorAlias(pathParts, value, false);
                    }
                }
            }
            if (targetName) _configurationUpdateTargets[fullKey] = targetName;
            const message = {
                type: 'config_set',
                section: section || '',
                key: String(key),
            };
            if (targetName) message.target = targetName;
            if (languageOverrideIdentifier) {
                message.overrideIdentifier = languageOverrideIdentifier;
            }
            if (value === undefined) {
                message.remove = true;
            } else {
                message.value = value;
            }
            send(message);
            _fireConfigurationChanged(pathParts, languageOverrideIdentifier);
            return Promise.resolve();
        },
    };
    const data = sectionData();
    if (data && typeof data === 'object' && !Array.isArray(data)) {
        for (const [key, value] of Object.entries(data)) {
            if (!Object.prototype.hasOwnProperty.call(proxy, key)) {
                proxy[key] = _configCloneValue(value);
            }
        }
    }
    return proxy;
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
            void resolveWebviewView(viewType);
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
async function resolveWebviewView(viewType, state) {
    const normalized = String(viewType || '');
    let reg = _webviewViewProviders.get(normalized);
    if (!reg && normalized) {
        _webviewViewActivationResolving.add(normalized);
        try {
            await _activateKnownExtensionsForEvent(`onView:${normalized}`);
        } finally {
            _webviewViewActivationResolving.delete(normalized);
        }
        reg = _webviewViewProviders.get(normalized);
    }
    if (!reg) {
        log(`no provider for viewType=${normalized}`);
        return;
    }
    const viewId = `view-${_nextViewHandle++}`;
    const viewOptions = reg.options && reg.options.webviewOptions
        ? reg.options.webviewOptions
        : {};
    const view = new WebviewView(
        viewId, normalized, viewOptions,
        _defaultLocalResourceRoots(reg.extensionPath));
    _webviewViews.set(viewId, view);
    view._emitMetadata();

    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const result = reg.provider.resolveWebviewView(view, { state: state ?? null }, token);
        if (result && typeof result.then === 'function') {
            result.catch(err => {
                log(`resolveWebviewView error for ${normalized}: ${err.message}`);
                send({ type: 'error', extensionId: normalized, error: err.message });
            });
        }
    } catch (err) {
        log(`resolveWebviewView error for ${normalized}: ${err.message}`);
        send({ type: 'error', extensionId: normalized, error: err.message });
    }
}

async function resolveCustomEditor(msg) {
    const viewType = String(msg.viewType || msg.customEditorId || '');
    if (viewType) await _activateKnownExtensionsForEvent(`onCustomEditor:${viewType}`);
    const reg = _customEditorProviders.get(viewType);
    if (!reg) {
        const error = `no custom editor provider for viewType=${viewType}`;
        log(error);
        send({ type: 'custom_editor_resolved', requestId: msg.requestId, viewType, ok: false, error });
        return;
    }
    const uri = _workspaceUriFromInput(msg.uri || msg.resource || msg.path);
    const supportsMultiple = _customEditorSupportsMultipleEditors(reg);
    if (!supportsMultiple) {
        const existingEntry = _customEditorSingletonEntry(viewType, uri);
        if (existingEntry) {
            send({
                type: 'custom_editor_resolved',
                requestId: msg.requestId,
                viewType,
                viewId: existingEntry.viewId,
                uri: existingEntry.uri.toString(),
                ok: true,
                reused: true,
                ..._customEditorStatePayload(
                    existingEntry, existingEntry.lastKind || 'resolved'),
            });
            return;
        }
    }
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
            const key = _customEditorDocumentEntryKey(
                viewType, document.uri, viewId, supportsMultiple);
            customEntry = {
                viewType,
                uri: _workspaceUriFromInput(document.uri),
                viewId,
                provider: reg.provider,
                supportsMultipleEditorsPerDocument: supportsMultiple,
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
            const key = _customEditorDocumentEntryKey(
                viewType, document.uri, viewId, supportsMultiple);
            customEntry = {
                viewType,
                uri: _workspaceUriFromInput(document.uri),
                viewId,
                provider: reg.provider,
                supportsMultipleEditorsPerDocument: supportsMultiple,
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
        _webviewPanels.delete(viewId);
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
        panel.webview._setState(state);
        const result = reg.serializer.deserializeWebviewPanel(panel, state);
        if (result && typeof result.then === 'function') await result;
        send({
            type: 'webview_panel_deserialized',
            requestId,
            viewType,
            viewId,
            ok: true,
            state,
            title: panel.title,
            viewColumn: panel.viewColumn,
            active: panel.active,
            visible: panel.visible,
            options: panel.options || {},
            webviewOptions: panel.webview._webviewOptionsPayload(),
        });
    } catch (err) {
        const error = err && err.message ? err.message : String(err);
        try { panel?.dispose?.(); } catch {}
        _webviewViews.delete(viewId);
        _webviewPanels.delete(viewId);
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
    const decoded = _deserializeWebviewMessageFromBridge(message);
    const view = _webviewViews.get(viewId);
    if (!view) {
        // Try matching by viewType
        for (const [id, v] of _webviewViews) {
            if (v.viewType === viewId) {
                v.webview._onDidReceiveMessage.fire(decoded);
                return;
            }
        }
        log(`webview_message: unknown viewId=${viewId}`);
        return;
    }
    view.webview._onDidReceiveMessage.fire(decoded);
}

function handleWebviewPanelViewState(msg) {
    const viewId = String(msg.viewId || msg.view_id || '');
    if (!viewId) return;
    const panel = _webviewPanels.get(viewId);
    if (!panel || typeof panel._updateViewStateFromHost !== 'function') {
        const view = _webviewViews.get(viewId);
        if (view && typeof view._updateVisibilityFromHost === 'function'
            && Object.prototype.hasOwnProperty.call(msg, 'visible')) {
            view._updateVisibilityFromHost(!!msg.visible);
        }
        return;
    }
    const nextState = {};
    if (Object.prototype.hasOwnProperty.call(msg, 'visible')) {
        nextState.visible = !!msg.visible;
    }
    if (Object.prototype.hasOwnProperty.call(msg, 'active')) {
        nextState.active = !!msg.active;
    }
    if (Object.prototype.hasOwnProperty.call(msg, 'viewColumn')) {
        nextState.viewColumn = msg.viewColumn;
    }
    panel._updateViewStateFromHost(nextState);
}

function handleWebviewState(msg) {
    const viewId = String(msg.viewId || msg.view_id || '');
    if (!viewId) return;
    const view = _webviewViews.get(viewId);
    if (view && view.webview && typeof view.webview._setState === 'function') {
        view.webview._setState(
            Object.prototype.hasOwnProperty.call(msg, 'state')
                ? msg.state
                : null);
        return;
    }
    for (const [, candidate] of _webviewViews) {
        if (candidate.viewType === viewId) {
            candidate.webview._setState(
                Object.prototype.hasOwnProperty.call(msg, 'state')
                    ? msg.state
                    : null);
            return;
        }
    }
}

function handleDisposeWebviewPanel(msg) {
    const viewId = String(msg.viewId || msg.view_id || '');
    if (!viewId) return;
    const panel = _webviewPanels.get(viewId);
    if (panel && typeof panel.dispose === 'function') {
        panel.dispose();
    }
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
    if (name) await _activateKnownExtensionsForEvent(
        `onLanguageModelTool:${name}`);
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

function _mcpServerDefinitionProviderEntries(id, handle) {
    const providerId = String(id || '');
    const providerHandle = Number(handle || 0);
    return Array.from(_mcpServerDefinitionProviders.values()).filter(entry => {
        if (providerHandle && entry.handle !== providerHandle) return false;
        if (providerId && entry.id !== providerId) return false;
        return true;
    });
}

async function handleMcpServerDefinitionsRequest(msg) {
    const requestId = String(msg.requestId || '');
    const providerId = String(msg.id || msg.providerId || '');
    if (providerId) await _activateKnownExtensionsForEvent(
        `onMcpCollection:${providerId}`);
    const entries = _mcpServerDefinitionProviderEntries(
        providerId, msg.handle);
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const providers = [];
        const servers = [];
        for (const entry of entries) {
            const provider = entry.provider || {};
            if (typeof provider.provideMcpServerDefinitions !== 'function') continue;
            const raw = provider.provideMcpServerDefinitions.call(provider, token);
            const result = raw && typeof raw.then === 'function' ? await raw : raw;
            const serialized = (Array.isArray(result) ? result : [])
                .map(_serializeMcpServerDefinition)
                .filter(Boolean)
                .map(server => Object.assign({}, server, {
                    providerId: entry.id,
                    providerHandle: entry.handle,
                    extensionId: entry.extensionId,
                }));
            providers.push({
                handle: entry.handle,
                id: entry.id,
                extensionId: entry.extensionId,
                servers: serialized,
            });
            servers.push(...serialized);
        }
        send({
            type: 'mcp_server_definitions_response',
            requestId,
            ok: true,
            value: { providerId, providers, servers },
        });
    } catch (err) {
        send({
            type: 'mcp_server_definitions_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function handleMcpServerResolveRequest(msg) {
    const requestId = String(msg.requestId || '');
    const providerId = String(msg.id || msg.providerId || '');
    if (providerId) await _activateKnownExtensionsForEvent(
        `onMcpCollection:${providerId}`);
    const entries = _mcpServerDefinitionProviderEntries(
        providerId, msg.handle || msg.providerHandle);
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const entry = entries[0];
        if (!entry) throw new Error(`MCP server definition provider not found: ${providerId || msg.handle || ''}`);
        const provider = entry.provider || {};
        const server = _mcpServerDefinitionFromPayload(msg.server || {});
        let resolved = server;
        if (typeof provider.resolveMcpServerDefinition === 'function') {
            const raw = provider.resolveMcpServerDefinition.call(provider, server, token);
            resolved = raw && typeof raw.then === 'function' ? await raw : raw;
        }
        send({
            type: 'mcp_server_resolve_response',
            requestId,
            ok: true,
            value: resolved === undefined || resolved === null
                ? null
                : _serializeMcpServerDefinition(resolved),
        });
    } catch (err) {
        send({
            type: 'mcp_server_resolve_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

function _chatContextProviderRecord(kind, id, provider, selector, extDesc) {
    const providerId = String(id || '');
    if (!providerId) throw new Error('Chat context provider id is required');
    if (!provider || typeof provider !== 'object') {
        throw new Error('Chat context provider is required');
    }
    const normalizedKind = String(kind || 'explicit');
    const entry = {
        handle: _nextChatContextProviderHandle++,
        kind: normalizedKind,
        id: providerId,
        selector: selector === undefined ? undefined : selector,
        provider,
        extensionId: extDesc.extensionId || '',
    };
    _chatContextProviders.set(entry.handle, entry);
    send({
        type: 'chat_context_provider_registered',
        handle: entry.handle,
        extensionId: entry.extensionId,
        id: entry.id,
        kind: entry.kind,
        selector: _serializeLanguageValue(entry.selector),
    });
    let changeDisposable;
    if (typeof provider.onDidChangeWorkspaceChatContext === 'function') {
        changeDisposable = provider.onDidChangeWorkspaceChatContext(() => {
            send({
                type: 'chat_context_provider_changed',
                handle: entry.handle,
                extensionId: entry.extensionId,
                id: entry.id,
                kind: entry.kind,
            });
        });
    }
    return { entry, changeDisposable };
}

function _registerChatContextProvider(kind, id, provider, selector, extDesc, subscriptions) {
    const { entry, changeDisposable } = _chatContextProviderRecord(
        kind, id, provider, selector, extDesc);
    const disposable = new Disposable(() => {
        if (_chatContextProviders.get(entry.handle) === entry) {
            _chatContextProviders.delete(entry.handle);
            try { changeDisposable?.dispose?.(); } catch {}
            send({
                type: 'chat_context_provider_disposed',
                handle: entry.handle,
                extensionId: entry.extensionId,
                id: entry.id,
                kind: entry.kind,
            });
        }
    });
    subscriptions.push(disposable);
    return disposable;
}

function _chatContextProviderEntries(kind, id, resource) {
    const requestedKind = String(kind || 'explicit');
    const providerId = String(id || '');
    const resourceDoc = resource
        ? _createLanguageDocument({
            uri: _uriFromPayload(resource),
            languageId: _languageIdForUri(_uriFromPayload(resource)),
            text: '',
            version: 1,
        })
        : null;
    return Array.from(_chatContextProviders.values()).filter(entry => {
        if (providerId && entry.id !== providerId) return false;
        if (entry.kind === requestedKind) {
            if (requestedKind === 'resource' && entry.selector !== undefined) {
                return _matchDocumentSelector(entry.selector, resourceDoc) > 0;
            }
            return true;
        }
        if (entry.kind !== 'legacy') return false;
        if (requestedKind === 'resource' && entry.selector !== undefined) {
            return _matchDocumentSelector(entry.selector, resourceDoc) > 0;
        }
        const provider = entry.provider || {};
        if (requestedKind === 'workspace') {
            return typeof provider.provideWorkspaceChatContext === 'function'
                || typeof provider.provideChatContext === 'function';
        }
        if (requestedKind === 'resource') {
            return typeof provider.provideChatContextForResource === 'function'
                || typeof provider.provideResourceChatContext === 'function'
                || typeof provider.provideChatContext === 'function';
        }
        return typeof provider.provideChatContextExplicit === 'function'
            || typeof provider.provideExplicitChatContext === 'function'
            || typeof provider.provideChatContext === 'function';
    });
}

function _chatContextProvideMethod(entry, kind) {
    const provider = entry.provider || {};
    if (kind === 'workspace') {
        return provider.provideWorkspaceChatContext || provider.provideChatContext;
    }
    if (kind === 'resource') {
        return provider.provideResourceChatContext
            || provider.provideChatContextForResource
            || provider.provideChatContext;
    }
    return provider.provideExplicitChatContext
        || provider.provideChatContextExplicit
        || provider.provideChatContext;
}

function _chatContextResolveMethod(entry, kind) {
    const provider = entry.provider || {};
    if (kind === 'resource') {
        return provider.resolveResourceChatContext || provider.resolveChatContext;
    }
    if (kind === 'explicit') {
        return provider.resolveExplicitChatContext || provider.resolveChatContext;
    }
    return provider.resolveChatContext;
}

function _serializeChatContextItem(item, entry, kind) {
    const value = _serializeLanguageValue(item && typeof item === 'object'
        ? item
        : { value: item === undefined || item === null ? '' : String(item) });
    if (value && typeof value === 'object') {
        value.providerId = entry.id;
        value.providerKind = kind;
        value.extensionId = entry.extensionId;
    }
    return value;
}

async function handleChatContextProviderRequest(msg) {
    const requestId = String(msg.requestId || '');
    const kind = String(msg.kind || msg.operation || 'explicit');
    const providerId = String(msg.id || msg.providerId || '');
    if (providerId) await _activateKnownExtensionsForEvent(
        `onChatContextProvider:${providerId}`);
    const entries = _chatContextProviderEntries(
        kind, providerId, msg.resource || msg.resourceUri);
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const providerResults = [];
        const items = [];
        for (const entry of entries) {
            const method = _chatContextProvideMethod(entry, kind);
            if (typeof method !== 'function') continue;
            const raw = kind === 'resource'
                ? method.call(entry.provider, {
                    resource: _uriFromPayload(msg.resource || msg.resourceUri),
                }, token)
                : method.call(entry.provider, token);
            const result = raw && typeof raw.then === 'function' ? await raw : raw;
            const rawItems = kind === 'resource'
                ? (result === undefined || result === null ? [] : [result])
                : (Array.isArray(result) ? result : []);
            const serialized = rawItems.map(item =>
                _serializeChatContextItem(item, entry, kind));
            providerResults.push({
                handle: entry.handle,
                id: entry.id,
                kind: entry.kind,
                extensionId: entry.extensionId,
                items: serialized,
            });
            items.push(...serialized);
        }
        send({
            type: 'chat_context_provider_response',
            requestId,
            ok: true,
            value: { kind, providerId, providers: providerResults, items },
        });
    } catch (err) {
        send({
            type: 'chat_context_provider_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function handleChatContextResolveRequest(msg) {
    const requestId = String(msg.requestId || '');
    const kind = String(msg.kind || msg.operation || 'explicit');
    const providerId = String(msg.id || msg.providerId || '');
    if (providerId) await _activateKnownExtensionsForEvent(
        `onChatContextProvider:${providerId}`);
    const entries = _chatContextProviderEntries(kind, providerId, msg.resource);
    const token = { isCancellationRequested: false, onCancellationRequested: new EventEmitter().event };
    try {
        const entry = entries.find(item =>
            !msg.handle || Number(msg.handle) === item.handle);
        if (!entry) throw new Error(`Chat context provider not found: ${providerId || kind}`);
        const method = _chatContextResolveMethod(entry, kind);
        if (typeof method !== 'function') {
            send({
                type: 'chat_context_provider_response',
                requestId,
                ok: true,
                value: {
                    kind,
                    providerId,
                    item: _serializeChatContextItem(msg.item || {}, entry, kind),
                },
            });
            return;
        }
        const raw = method.call(
            entry.provider,
            _deserializeArgFromPython(msg.item || {}),
            token);
        const result = raw && typeof raw.then === 'function' ? await raw : raw;
        send({
            type: 'chat_context_provider_response',
            requestId,
            ok: true,
            value: {
                kind,
                providerId,
                item: _serializeChatContextItem(result, entry, kind),
            },
        });
    } catch (err) {
        send({
            type: 'chat_context_provider_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function handleChatParticipantRequest(msg) {
    const requestId = String(msg.requestId || '');
    const participantId = String(msg.participantId || msg.id || '');
    if (participantId) await _activateKnownExtensionsForEvent(
        `onChatParticipant:${participantId}`);
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
                responseParts: stream._responseParts || [],
                response_parts: stream._responseParts || [],
                contentReferences: stream._contentReferences || [],
                content_references: stream._contentReferences || [],
                fileTrees: stream._fileTrees || [],
                file_trees: stream._fileTrees || [],
                textEdits: stream._textEdits || [],
                text_edits: stream._textEdits || [],
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
        completionResolve: 'resolveCompletionItem',
        hover: 'provideHover',
        signatureHelp: 'provideSignatureHelp',
        definition: 'provideDefinition',
        typeDefinition: 'provideTypeDefinition',
        declaration: 'provideDeclaration',
        implementation: 'provideImplementation',
        references: 'provideReferences',
        documentHighlight: 'provideDocumentHighlights',
        evaluatableExpression: 'provideEvaluatableExpression',
        inlineValue: 'provideInlineValues',
        prepareRename: 'prepareRename',
        rename: 'provideRenameEdits',
        documentLink: 'provideDocumentLinks',
        documentLinkResolve: 'resolveDocumentLink',
        inlayHint: 'provideInlayHints',
        inlayHintResolve: 'resolveInlayHint',
        inlineCompletion: 'provideInlineCompletionItems',
        codeLens: 'provideCodeLenses',
        codeLensResolve: 'resolveCodeLens',
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
        semanticTokensEdits: 'provideDocumentSemanticTokensEdits',
        semanticTokensLegend: 'provideDocumentSemanticTokens',
        semanticTokensRange: 'provideDocumentRangeSemanticTokens',
        semanticTokensRangeLegend: 'provideDocumentRangeSemanticTokens',
        documentSymbol: 'provideDocumentSymbols',
        diagnostics: 'getDiagnostics',
        codeActions: 'provideCodeActions',
        codeActionResolve: 'resolveCodeAction',
        codeActionsResolve: 'resolveCodeAction',
        formattingProviders: 'formattingProviders',
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

function _languageProviderMatchesProviderId(entry, providerId) {
    const expected = String(providerId || '').trim().toLowerCase();
    if (!expected) return true;
    return [
        entry.providerId,
        entry.id,
        entry.extensionId,
        entry.displayName,
    ].some(value => String(value || '').trim().toLowerCase() === expected);
}

function _formattingProviderPayload(entry, capability) {
    const providerId = String(entry.providerId || entry.id || entry.extensionId || '').trim();
    if (!providerId) return null;
    return {
        id: providerId,
        providerId,
        extensionId: String(entry.extensionId || providerId),
        displayName: String(entry.displayName || providerId),
        kind: entry.kind,
        capability,
    };
}

function _addFormattingProviderPayload(values, index, entry, capability) {
    const payload = _formattingProviderPayload(entry, capability);
    if (!payload) return;
    const key = payload.providerId.toLowerCase();
    const existing = index.get(key);
    if (!existing) {
        payload.capabilities = [capability];
        values.push(payload);
        index.set(key, payload);
        return;
    }
    if (!existing.capabilities.includes(capability)) {
        existing.capabilities.push(capability);
    }
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

function _serializeCompletionItemHandle(handle, item) {
    const value = _serializeLanguageValue(item);
    if (value && typeof value === 'object' && !Array.isArray(value)) {
        value._nodeCompletionHandle = String(handle || '');
    }
    return value;
}

function _cacheCompletionItem(provider, item, resolved = false) {
    const handle = String(_nextCompletionItemHandle++);
    _completionItemCache.set(handle, { provider, item, resolved: !!resolved });
    while (_completionItemCache.size > 2000) {
        const first = _completionItemCache.keys().next().value;
        if (first === undefined) break;
        _completionItemCache.delete(first);
    }
    return _serializeCompletionItemHandle(handle, item);
}

function _resolvableLanguageItemKind(kind) {
    return ({
        documentLink: 'documentLink',
        documentLinkResolve: 'documentLink',
        inlayHint: 'inlayHint',
        inlayHintResolve: 'inlayHint',
        codeLens: 'codeLens',
        codeLensResolve: 'codeLens',
        codeActions: 'codeActions',
        codeActionResolve: 'codeActions',
        codeActionsResolve: 'codeActions',
    })[kind] || '';
}

function _resolvableLanguageItemHandleKey(kind) {
    return ({
        documentLink: '_nodeDocumentLinkHandle',
        inlayHint: '_nodeInlayHintHandle',
        codeLens: '_nodeCodeLensHandle',
        codeActions: '_nodeCodeActionHandle',
    })[_resolvableLanguageItemKind(kind) || kind] || '';
}

function _resolvableLanguageItemPayloadKey(kind) {
    return ({
        documentLink: 'link',
        inlayHint: 'hint',
        codeLens: 'lens',
        codeActions: 'action',
    })[_resolvableLanguageItemKind(kind) || kind] || 'item';
}

function _serializeResolvableLanguageItem(kind, handle, item) {
    const value = _serializeLanguageValue(item);
    const key = _resolvableLanguageItemHandleKey(kind);
    if (key && value && typeof value === 'object' && !Array.isArray(value)) {
        value[key] = String(handle || '');
    }
    return value;
}

function _cacheResolvableLanguageItem(kind, provider, item, resolved = false) {
    const baseKind = _resolvableLanguageItemKind(kind) || kind;
    const handle = String(_nextResolvableLanguageItemHandle++);
    _resolvableLanguageItemCache.set(handle, {
        provider,
        item,
        kind: baseKind,
        resolved: !!resolved,
    });
    while (_resolvableLanguageItemCache.size > 3000) {
        const first = _resolvableLanguageItemCache.keys().next().value;
        if (first === undefined) break;
        _resolvableLanguageItemCache.delete(first);
    }
    return _serializeResolvableLanguageItem(baseKind, handle, item);
}

function _resolvableLanguageItemFromMessage(msg, kind) {
    const payloadKey = _resolvableLanguageItemPayloadKey(kind);
    return msg[payloadKey] || msg.item || msg.value || null;
}

function _resolvableLanguageItemHandleFromMessage(msg, kind) {
    const item = _resolvableLanguageItemFromMessage(msg, kind);
    const handleKey = _resolvableLanguageItemHandleKey(kind);
    return String(
        (item && handleKey ? item[handleKey] : '')
        || msg[handleKey]
        || msg.handle
        || '');
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
    const cts = new CancellationTokenSource();
    const token = cts.token;
    const providerErrors = [];
    let matchedProviderCount = 0;
    if (requestId) _languageProviderRequests.set(requestId, cts);
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
        const trigger = msg.triggerCharacter === undefined || msg.triggerCharacter === null
            ? ''
            : String(msg.triggerCharacter);
        const context = msg.context && typeof msg.context === 'object'
            ? Object.assign({}, msg.context)
            : {};
        if (kind === 'completion') {
            if (context.triggerKind === undefined || context.triggerKind === null) {
                context.triggerKind = trigger ? 1 : 0;
            }
            context.triggerCharacter = trigger || undefined;
        } else if (kind === 'signatureHelp') {
            if (trigger) {
                context.triggerKind = context.triggerKind || 2;
                context.triggerCharacter = trigger;
            } else {
                context.triggerKind = context.triggerKind || 1;
            }
        }
        const matchingProvidersForKind = (targetKind) => {
            if (workspaceSymbolKind) {
                return _languageProviders.filter(entry => entry.kind === 'workspaceSymbol');
            }
            return _languageProviders
                .filter(entry => entry.kind === targetKind)
                .map(entry => ({ entry, score: _matchDocumentSelector(entry.selector, document) }))
                .filter(item => item.score > 0)
                .sort((a, b) => b.score - a.score)
                .map(item => item.entry);
        };
        const providerIdentity = (entry) => {
            if (!entry) return {};
            return {
                handle: entry.handle,
                providerId: String(entry.providerId || entry.id || entry.handle || ''),
                extensionId: String(entry.extensionId || ''),
                displayName: String(entry.displayName || entry.providerId || entry.id || entry.handle || ''),
                kind: String(entry.kind || kind || ''),
            };
        };
        const recordProviderError = (entry, err) => {
            const error = err && err.message ? String(err.message) : String(err || '');
            const payload = Object.assign(providerIdentity(entry), {
                error,
                name: err && err.name ? String(err.name) : '',
            });
            providerErrors.push(payload);
            send(Object.assign({
                type: 'language_provider_error',
                requestId,
                providerCount: matchedProviderCount,
            }, payload));
            log(`language provider ${kind} error: ${error}`);
        };
        const respondCancelled = () => {
            send({
                type: 'language_provider_response',
                requestId,
                ok: false,
                kind,
                cancelled: true,
                error: 'Language provider request cancelled',
                providerCount: matchedProviderCount,
                providerErrors: providerErrors.slice(),
            });
        };
        if (token.isCancellationRequested) {
            respondCancelled();
            return;
        }

        if (kind === 'diagnostics') {
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: _serializeLanguageValue(_diagnosticsForUri(document.uri)),
            });
            return;
        }

        if (kind === 'formattingProviders') {
            const values = [];
            const index = new Map();
            for (const entry of matchingProvidersForKind('formatting')) {
                _addFormattingProviderPayload(
                    values, index, entry, 'documentFormatting');
            }
            for (const entry of matchingProvidersForKind('rangeFormatting')) {
                _addFormattingProviderPayload(
                    values, index, entry, 'rangeFormatting');
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

        if (kind === 'completionResolve') {
            const handle = String(
                msg.item?._nodeCompletionHandle
                || msg._nodeCompletionHandle
                || msg.handle
                || '');
            const cached = handle ? _completionItemCache.get(handle) : null;
            if (cached) {
                try {
                    if (!cached.resolved && typeof cached.provider?.resolveCompletionItem === 'function') {
                        const resolved = await cached.provider.resolveCompletionItem.call(
                            cached.provider, cached.item, token);
                        if (resolved !== undefined && resolved !== null) cached.item = resolved;
                        cached.resolved = true;
                        _completionItemCache.set(handle, cached);
                    }
                    send({
                        type: 'language_provider_response',
                        requestId,
                        ok: true,
                        kind,
                        value: _serializeCompletionItemHandle(handle, cached.item),
                    });
                    return;
                } catch (err) {
                    recordProviderError(entry, err);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: msg.item || null,
            });
            return;
        }
        const resolvableKind = _resolvableLanguageItemKind(kind);
        if (resolvableKind && kind.endsWith('Resolve')) {
            const handle = _resolvableLanguageItemHandleFromMessage(msg, kind);
            const cached = handle ? _resolvableLanguageItemCache.get(handle) : null;
            if (cached && cached.kind === resolvableKind) {
                try {
                    const resolveFn = cached.provider && cached.provider[methodName];
                    if (!cached.resolved && typeof resolveFn === 'function') {
                        const resolved = await resolveFn.call(
                            cached.provider, cached.item, token);
                        if (resolved !== undefined && resolved !== null) {
                            cached.item = resolved;
                        }
                        cached.resolved = true;
                        _resolvableLanguageItemCache.set(handle, cached);
                    }
                    send({
                        type: 'language_provider_response',
                        requestId,
                        ok: true,
                        kind,
                        value: _serializeResolvableLanguageItem(
                            resolvableKind, handle, cached.item),
                    });
                    return;
                } catch (err) {
                    recordProviderError(entry, err);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: _resolvableLanguageItemFromMessage(msg, kind),
            });
            return;
        }
        const providerKind = kind === 'semanticTokensLegend'
            ? 'semanticTokens'
            : kind === 'semanticTokensEdits'
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
        const providers = matchingProvidersForKind(providerKind);
        matchedProviderCount = providers.length;
        send({
            type: 'language_provider_status',
            requestId,
            kind,
            providerCount: matchedProviderCount,
        });

        if (kind === 'workspaceSymbol') {
            const values = [];
            const query = String(msg.query || msg.search || '');
            for (const entry of providers) {
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawSymbols = _normalizeProviderItems(await fn.call(provider, query, token));
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    for (const symbol of rawSymbols) {
                        if (symbol && symbol.name) values.push(_cacheWorkspaceSymbol(provider, symbol));
                    }
                } catch (err) {
                    recordProviderError(entry, err);
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
                    recordProviderError(entry, err);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const triggers = (entry.triggers || []).map(item => String(item));
                if (trigger && !triggers.includes(trigger)) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token, context);
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    const normalized = _completionListFromProviderValue(value);
                    for (let item of normalized.items) {
                        if (token.isCancellationRequested) { respondCancelled(); return; }
                        let didResolve = false;
                        if (remainingResolves > 0 && typeof provider.resolveCompletionItem === 'function') {
                            const resolved = await provider.resolveCompletionItem.call(provider, item, token);
                            if (resolved !== undefined && resolved !== null) item = resolved;
                            remainingResolves -= 1;
                            didResolve = true;
                        }
                        items.push(_cacheCompletionItem(provider, item, didResolve));
                    }
                    isIncomplete = isIncomplete || normalized.isIncomplete;
                } catch (err) {
                    recordProviderError(entry, err);
                }
            }
            send({
                type: 'language_provider_response',
                requestId,
                ok: true,
                kind,
                value: {
                    items,
                    isIncomplete,
                },
            });
            return;
        }

        if (kind === 'signatureHelp') {
            for (const entry of providers) {
                if (token.isCancellationRequested) { respondCancelled(); return; }
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
                    if (token.isCancellationRequested) { respondCancelled(); return; }
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
                    recordProviderError(entry, err);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, position, token);
                    if (token.isCancellationRequested) { respondCancelled(); return; }
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
                    recordProviderError(entry, err);
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
                    recordProviderError(entry, err);
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
                    recordProviderError(entry, err);
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
                    recordProviderError(entry, err);
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

        if (kind === 'evaluatableExpression') {
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
                    recordProviderError(entry, err);
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

        if (kind === 'inlineValue') {
            const values = [];
            const contextPayload = msg.context && typeof msg.context === 'object'
                ? msg.context
                : {};
            const stoppedLocation = _rangeFromPayload(
                contextPayload.stoppedLocation || msg.stoppedLocation || msg.range || range);
            const frameId = Number.isFinite(Number(contextPayload.frameId))
                ? Number(contextPayload.frameId)
                : 0;
            const inlineContext = new InlineValueContext(frameId, stoppedLocation);
            for (const entry of providers) {
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, range, inlineContext, token);
                    values.push(..._normalizeProviderItems(value));
                } catch (err) {
                    recordProviderError(entry, err);
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
                    recordProviderError(entry, err);
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
                        let didResolve = false;
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveDocumentLink === 'function') {
                                const resolved = await provider.resolveDocumentLink.call(provider, link, token);
                                if (resolved !== undefined && resolved !== null) link = resolved;
                                didResolve = true;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(_cacheResolvableLanguageItem('documentLink', provider, link, didResolve));
                    }
                } catch (err) {
                    recordProviderError(entry, err);
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
                        let didResolve = false;
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveInlayHint === 'function') {
                                const resolved = await provider.resolveInlayHint.call(provider, hint, token);
                                if (resolved !== undefined && resolved !== null) hint = resolved;
                                didResolve = true;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(_cacheResolvableLanguageItem('inlayHint', provider, hint, didResolve));
                    }
                } catch (err) {
                    recordProviderError(entry, err);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawLenses = _normalizeProviderItems(await fn.call(provider, document, token));
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    for (let lens of rawLenses) {
                        if (token.isCancellationRequested) { respondCancelled(); return; }
                        let didResolve = false;
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveCodeLens === 'function') {
                                const resolved = await provider.resolveCodeLens.call(provider, lens, token);
                                if (resolved !== undefined && resolved !== null) lens = resolved;
                                didResolve = true;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(_cacheResolvableLanguageItem('codeLens', provider, lens, didResolve));
                    }
                } catch (err) {
                    recordProviderError(entry, err);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    await fn.call(provider, document, pasteRanges, dataTransfer, token);
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                } catch (err) {
                    recordProviderError(entry, err);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                if (!_dataTransferMatchesMetadata(dataTransfer, entry.metadata, 'pasteMimeTypes')) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawEdits = _normalizeProviderItems(
                        await fn.call(provider, document, pasteRanges, dataTransfer, pasteContext, token));
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    for (let edit of rawEdits) {
                        if (token.isCancellationRequested) { respondCancelled(); return; }
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
                    recordProviderError(entry, err);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                if (!_dataTransferMatchesMetadata(dataTransfer, entry.metadata, 'dropMimeTypes')) continue;
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawEdits = _normalizeProviderItems(
                        await fn.call(provider, document, position, dataTransfer, token));
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    for (let edit of rawEdits) {
                        if (token.isCancellationRequested) { respondCancelled(); return; }
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
                    recordProviderError(entry, err);
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

        if (kind === 'semanticTokensEdits') {
            const previousResultId = msg.previousResultId === undefined || msg.previousResultId === null
                ? ''
                : String(msg.previousResultId);
            for (const entry of providers) {
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = await fn.call(provider, document, previousResultId, token);
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    if (value !== undefined && value !== null) {
                        send({
                            type: 'language_provider_response',
                            requestId,
                            ok: true,
                            kind,
                            value: {
                                edits: _serializeLanguageValue(value),
                                legend: _serializeLanguageValue(entry.metadata || null),
                            },
                        });
                        return;
                    }
                } catch (err) {
                    recordProviderError(entry, err);
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

        if (kind === 'semanticTokens' || kind === 'semanticTokensRange') {
            for (const entry of providers) {
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const value = kind === 'semanticTokensRange'
                        ? await fn.call(provider, document, range, token)
                        : await fn.call(provider, document, token);
                    if (token.isCancellationRequested) { respondCancelled(); return; }
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
                    recordProviderError(entry, err);
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
            const onlyKind = msg.only === undefined || msg.only === null
                ? undefined
                : _codeActionKindFromPayload(msg.only);
            const rawTriggerKind = Number(msg.triggerKind);
            const triggerKind = (
                rawTriggerKind === CodeActionTriggerKind.Invoke
                || rawTriggerKind === CodeActionTriggerKind.Automatic)
                ? rawTriggerKind
                : CodeActionTriggerKind.Invoke;
            const codeActionContext = {
                diagnostics: _codeActionDiagnostics(document, msg),
                only: onlyKind,
                triggerKind,
            };
            for (const entry of providers) {
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    const rawActions = _normalizeProviderItems(await fn.call(
                        provider, document, range, codeActionContext, token));
                    if (token.isCancellationRequested) { respondCancelled(); return; }
                    for (let action of rawActions) {
                        if (token.isCancellationRequested) { respondCancelled(); return; }
                        if (!_codeActionMatchesKind(action, msg.only)) continue;
                        let didResolve = false;
                        if (remainingResolves > 0) {
                            if (typeof provider.resolveCodeAction === 'function') {
                                const resolved = await provider.resolveCodeAction.call(provider, action, token);
                                if (resolved !== undefined && resolved !== null) action = resolved;
                                didResolve = true;
                            }
                            remainingResolves -= 1;
                        }
                        values.push(_cacheResolvableLanguageItem('codeActions', provider, action, didResolve));
                    }
                } catch (err) {
                    recordProviderError(entry, err);
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
            if (token.isCancellationRequested) { respondCancelled(); return; }
            if ((kind === 'formatting' || kind === 'rangeFormatting')
                && !_languageProviderMatchesProviderId(entry, msg.providerId)) {
                continue;
            }
            const provider = entry.provider;
            const fn = provider && provider[methodName];
            if (typeof fn !== 'function') continue;
            try {
                let value;
                if (kind === 'codeActions') {
                    value = await fn.call(provider, document, range, {
                        diagnostics: _codeActionDiagnostics(document, msg),
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                values.push(..._normalizeProviderItems(value));
            } catch (err) {
                recordProviderError(entry, err);
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
    } finally {
        _languageProviderRequests.delete(requestId);
        cts.dispose();
    }
}

function handleLanguageProviderCancel(msg) {
    const requestId = String(msg.requestId || '');
    const cts = _languageProviderRequests.get(requestId);
    if (!cts) return;
    cts.cancel();
}

async function handleFileDecorationRequest(msg) {
    const requestId = String(msg.requestId || '');
    const uri = _uriFromPayload(msg.uri || msg);
    const cts = new CancellationTokenSource();
    const token = cts.token;
    const values = [];
    const errors = [];
    if (requestId) _fileDecorationRequests.set(requestId, cts);
    try {
        if (!requestId) throw new Error('Missing file decoration requestId');
        for (const entry of [..._fileDecorationProviders]) {
            if (token.isCancellationRequested) break;
            const provider = entry.provider;
            const fn = provider && provider.provideFileDecoration;
            if (typeof fn !== 'function') continue;
            try {
                const value = await fn.call(provider, uri, token);
                if (token.isCancellationRequested) break;
                if (value !== undefined && value !== null) {
                    values.push(_serializeLanguageValue(value));
                }
            } catch (err) {
                const message = err?.message || String(err);
                log(`file decoration provider error: ${message}`);
                errors.push({
                    handle: entry.handle,
                    extensionId: entry.extensionId,
                    error: message,
                });
            }
        }
        if (token.isCancellationRequested) {
            send({
                type: 'file_decoration_response',
                requestId,
                ok: false,
                cancelled: true,
                error: 'File decoration request cancelled',
                value: values,
                errors,
            });
            return;
        }
        send({
            type: 'file_decoration_response',
            requestId,
            ok: true,
            value: values,
            errors,
        });
    } catch (err) {
        send({
            type: 'file_decoration_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
            value: [],
            errors,
        });
    } finally {
        _fileDecorationRequests.delete(requestId);
        cts.dispose();
    }
}

function handleFileDecorationCancel(msg) {
    const requestId = String(msg.requestId || '');
    const cts = _fileDecorationRequests.get(requestId);
    if (!cts) return;
    cts.cancel();
}

async function handleScmQuickDiffRequest(msg) {
    const requestId = String(msg.requestId || '');
    const providerId = String(msg.providerId || '');
    const provider = _scmProviders.get(providerId);
    const quickDiffProvider = provider && provider.quickDiffProvider;
    const fn = quickDiffProvider && quickDiffProvider.provideOriginalResource;
    const cts = new CancellationTokenSource();
    const token = cts.token;
    if (requestId) _scmQuickDiffRequests.set(requestId, cts);
    try {
        if (!requestId) throw new Error('Missing SCM quick diff requestId');
        if (!provider) throw new Error(`SCM provider not found: ${providerId}`);
        if (typeof fn !== 'function') {
            send({
                type: 'scm_quick_diff_response',
                requestId,
                ok: false,
                noProvider: true,
                error: 'SCM provider has no quick diff provider',
            });
            return;
        }
        const uri = _uriFromPayload(msg.resourceUri || msg.uri || msg);
        const result = await fn.call(quickDiffProvider, uri, token);
        if (token.isCancellationRequested) {
            send({
                type: 'scm_quick_diff_response',
                requestId,
                ok: false,
                cancelled: true,
                error: 'SCM quick diff request cancelled',
            });
            return;
        }
        send({
            type: 'scm_quick_diff_response',
            requestId,
            ok: true,
            providerId,
            resourceUri: uri.toString(),
            originalResourceUri: result ? _serializeLanguageUri(result) : '',
        });
    } catch (err) {
        send({
            type: 'scm_quick_diff_response',
            requestId,
            ok: false,
            providerId,
            error: err?.message || String(err),
        });
    } finally {
        _scmQuickDiffRequests.delete(requestId);
        cts.dispose();
    }
}

function handleScmQuickDiffCancel(msg) {
    const requestId = String(msg.requestId || '');
    const cts = _scmQuickDiffRequests.get(requestId);
    if (!cts) return;
    cts.cancel();
}

function _scmValidationType(value) {
    if (typeof value === 'string') {
        const key = value.trim().toLowerCase();
        if (key === 'info' || key === 'information') return 1;
        if (key === 'warning' || key === 'warn') return 2;
        if (key === 'error') return 3;
    }
    const numeric = Number(value);
    return Number.isFinite(numeric) ? numeric : 0;
}

function _scmValidationMessage(value) {
    if (value == null) return '';
    if (typeof value === 'string') return value;
    if (typeof value === 'object') {
        if (value.message != null) return _scmValidationMessage(value.message);
        if (value.value != null) return String(value.value);
        if (value.text != null) return String(value.text);
    }
    return String(value);
}

function _normalizeScmInputValidation(value) {
    if (!value) return null;
    const raw = _plainBridgeValue(value);
    if (Array.isArray(raw)) {
        if (!raw.length) return null;
        const message = _scmValidationMessage(raw[0]);
        if (!message) return null;
        const type = _scmValidationType(raw.length > 1 ? raw[1] : 0);
        return { message, type, severity: type };
    }
    if (raw && typeof raw === 'object') {
        const message = _scmValidationMessage(raw.message ?? raw);
        if (!message) return null;
        const type = _scmValidationType(raw.type ?? raw.severity ?? 0);
        return { message, type, severity: type };
    }
    const message = _scmValidationMessage(raw);
    if (!message) return null;
    return { message, type: 0, severity: 0 };
}

function _serializeScmHistoryRef(ref) {
    const raw = _plainBridgeValue(ref);
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
    const id = String(raw.id || raw.name || raw.label || '').trim();
    const label = String(raw.label || raw.name || id).trim();
    if (!id && !label) return null;
    raw.id = id || label;
    raw.label = label || id;
    return raw;
}

function _serializeScmHistoryItem(item) {
    let raw = _plainBridgeValue(item);
    if (!raw || typeof raw !== 'object' || Array.isArray(raw)) {
        raw = { id: String(item || ''), message: String(item || '') };
    }
    const id = String(raw.id || raw.revision || '').trim();
    if (id) raw.id = id;
    if (Array.isArray(raw.references)) {
        raw.references = raw.references
            .map(_serializeScmHistoryRef)
            .filter(Boolean);
    }
    return raw;
}

function _serializeScmHistoryChanges(value) {
    const raw = _plainBridgeValue(value);
    const items = Array.isArray(raw) ? raw : (raw == null ? [] : [raw]);
    return items.filter(item => item && typeof item === 'object' && !Array.isArray(item));
}

function _scmHistoryCurrentRefs(historyProvider) {
    return {
        historyItemRef: _serializeScmHistoryRef(
            historyProvider && historyProvider.currentHistoryItemRef),
        historyItemRemoteRef: _serializeScmHistoryRef(
            historyProvider && historyProvider.currentHistoryItemRemoteRef),
        historyItemBaseRef: _serializeScmHistoryRef(
            historyProvider && historyProvider.currentHistoryItemBaseRef),
    };
}

async function handleScmHistoryRequest(msg) {
    const requestId = String(msg.requestId || '');
    const providerId = String(msg.providerId || msg.id || '');
    const provider = _scmProviders.get(providerId);
    const historyProvider = provider && provider.historyProvider;
    const operation = String(msg.operation || msg.op || '');
    const payload = msg.payload && typeof msg.payload === 'object' ? msg.payload : {};
    const cts = new CancellationTokenSource();
    const token = cts.token;
    if (requestId) _scmHistoryRequests.set(requestId, cts);
    try {
        if (!requestId) throw new Error('Missing SCM history requestId');
        if (!provider) throw new Error(`SCM provider not found: ${providerId}`);
        if (!historyProvider) {
            send({
                type: 'scm_history_response',
                requestId,
                ok: false,
                noProvider: true,
                error: 'SCM provider has no history provider',
            });
            return;
        }
        let value;
        if (operation === 'provideRefs') {
            if (typeof historyProvider.provideHistoryItemRefs !== 'function') {
                throw new Error('SCM history provider has no provideHistoryItemRefs');
            }
            const refs = await historyProvider.provideHistoryItemRefs(
                payload.historyItemRefs, token);
            value = Array.isArray(refs)
                ? refs.map(_serializeScmHistoryRef).filter(Boolean) : [];
        } else if (operation === 'provideItems') {
            if (typeof historyProvider.provideHistoryItems !== 'function') {
                throw new Error('SCM history provider has no provideHistoryItems');
            }
            const items = await historyProvider.provideHistoryItems(
                payload.options || {}, token);
            value = Array.isArray(items) ? items.map(_serializeScmHistoryItem) : [];
        } else if (operation === 'provideChanges') {
            if (typeof historyProvider.provideHistoryItemChanges !== 'function') {
                throw new Error('SCM history provider has no provideHistoryItemChanges');
            }
            value = _serializeScmHistoryChanges(
                await historyProvider.provideHistoryItemChanges(
                    payload.historyItemId || payload.id || '',
                    payload.historyItemParentId || payload.parentId,
                    token));
        } else if (operation === 'resolveItem') {
            if (typeof historyProvider.resolveHistoryItem !== 'function') {
                throw new Error('SCM history provider has no resolveHistoryItem');
            }
            const item = await historyProvider.resolveHistoryItem(
                payload.historyItemId || payload.id || '', token);
            value = item ? _serializeScmHistoryItem(item) : null;
        } else if (operation === 'resolveChatContext') {
            if (typeof historyProvider.resolveHistoryItemChatContext !== 'function') {
                throw new Error('SCM history provider has no resolveHistoryItemChatContext');
            }
            value = _plainBridgeValue(await historyProvider.resolveHistoryItemChatContext(
                payload.historyItemId || payload.id || '', token));
        } else if (operation === 'resolveChangeRangeChatContext') {
            if (typeof historyProvider.resolveHistoryItemChangeRangeChatContext !== 'function') {
                throw new Error('SCM history provider has no resolveHistoryItemChangeRangeChatContext');
            }
            value = _plainBridgeValue(await historyProvider.resolveHistoryItemChangeRangeChatContext(
                payload.historyItemId || payload.id || '',
                payload.historyItemParentId || payload.parentId || '',
                payload.path || '',
                token));
        } else if (operation === 'resolveCommonAncestor') {
            if (typeof historyProvider.resolveHistoryItemRefsCommonAncestor !== 'function') {
                throw new Error('SCM history provider has no resolveHistoryItemRefsCommonAncestor');
            }
            value = _plainBridgeValue(await historyProvider.resolveHistoryItemRefsCommonAncestor(
                payload.historyItemRefs || payload.refs || [], token));
        } else {
            throw new Error(`Unknown SCM history operation: ${operation}`);
        }
        if (token.isCancellationRequested) {
            send({
                type: 'scm_history_response',
                requestId,
                ok: false,
                cancelled: true,
                error: 'SCM history request cancelled',
            });
            return;
        }
        send({
            type: 'scm_history_response',
            requestId,
            ok: true,
            providerId,
            operation,
            value,
        });
    } catch (err) {
        send({
            type: 'scm_history_response',
            requestId,
            ok: false,
            providerId,
            operation,
            error: err?.message || String(err),
        });
    } finally {
        _scmHistoryRequests.delete(requestId);
        cts.dispose();
    }
}

function handleScmHistoryCancel(msg) {
    const requestId = String(msg.requestId || '');
    const cts = _scmHistoryRequests.get(requestId);
    if (!cts) return;
    cts.cancel();
}

async function handleScmValidateInput(msg) {
    const requestId = String(msg.requestId || '');
    const providerId = String(msg.providerId || msg.id || '');
    const provider = _scmProviders.get(providerId);
    try {
        if (!requestId) throw new Error('Missing SCM input validation requestId');
        if (!provider || !provider.inputBox) {
            throw new Error(`SCM provider not found: ${providerId}`);
        }
        const inputBox = provider.inputBox;
        let validation = null;
        if (typeof inputBox._validateInput === 'function') {
            validation = await inputBox._validateInput(
                String(msg.value || ''),
                Number.isFinite(Number(msg.cursorPosition))
                    ? Number(msg.cursorPosition) : 0);
        }
        send({
            type: 'scm_validate_input_response',
            requestId,
            ok: true,
            providerId,
            value: String(msg.value || ''),
            validation,
            validationProvider: typeof inputBox.validateInput === 'function',
        });
    } catch (err) {
        send({
            type: 'scm_validate_input_response',
            requestId,
            ok: false,
            providerId,
            value: String(msg.value || ''),
            validation: {
                message: err?.message || String(err),
                type: 3,
                severity: 3,
            },
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
        } else if (op === 'resolveTreeItem') {
            const raw = await _resolveTreeItemForProvider(provider, viewId, element);
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

async function handleTreeDragDropRequest(msg) {
    const requestId = String(msg.requestId || '');
    const viewId = String(msg.viewId || '');
    const op = String(msg.op || '');
    const view = _treeViews.get(viewId);
    const controller = view?._dragAndDropController;
    try {
        if (!controller) throw new Error(`Tree drag/drop controller not found: ${viewId}`);
        const token = {
            isCancellationRequested: false,
            onCancellationRequested: new EventEmitter().event,
        };
        const dataTransfer = _dataTransferFromPayload(msg.dataTransfer || {});
        if (op === 'handleDrag') {
            const sourceHandles = Array.isArray(msg.sourceHandles)
                ? msg.sourceHandles.map(handle => String(handle || '')).filter(Boolean)
                : [];
            const source = sourceHandles.map(handle => _treeElementForHandle(viewId, handle));
            if (source.some(item => item === undefined)) {
                throw new Error('Tree drag source handle is stale or unknown');
            }
            if (typeof controller.handleDrag === 'function') {
                await controller.handleDrag(source, dataTransfer, token);
            }
        } else if (op === 'handleDrop') {
            const targetHandle = String(msg.targetHandle || '');
            const target = targetHandle
                ? _treeElementForHandle(viewId, targetHandle)
                : undefined;
            if (targetHandle && target === undefined) {
                throw new Error('Tree drop target handle is stale or unknown');
            }
            if (typeof controller.handleDrop !== 'function') {
                throw new Error('Tree drag/drop controller does not implement handleDrop');
            }
            await controller.handleDrop(target, dataTransfer, token);
        } else {
            throw new Error(`Unsupported tree drag/drop op: ${op}`);
        }
        send({
            type: 'tree_drag_drop_response',
            requestId,
            ok: true,
            value: { dataTransfer: _dataTransferToPayload(dataTransfer) },
        });
    } catch (err) {
        send({
            type: 'tree_drag_drop_response',
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
    } else if (event === 'checkbox' && element !== undefined) {
        const state = Number(msg.checkboxState) === 1 ? 1 : 0;
        view._onDidChangeCheckboxState.fire({ items: [[element, state]] });
    } else if (event === 'visibility') {
        view._setVisibleFromHost?.(!!msg.visible);
    }
}

async function handleTerminalProfileRequest(msg) {
    const requestId = msg.requestId;
    try {
        const value = await _createTerminalFromProfileProvider(
            msg.id || msg.profileId,
            msg.options || {});
        send({
            type: 'terminal_profile_response',
            requestId,
            ok: true,
            profile: value.profile || null,
            terminal: value.terminal || null,
            cancelled: value.cancelled === true,
        });
    } catch (err) {
        send({
            type: 'terminal_profile_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

async function handleTerminalLinkRequest(msg) {
    const requestId = msg.requestId;
    try {
        const terminal = _terminalForLinkRequest(msg);
        if (!terminal) {
            throw new Error('Terminal not found for link request');
        }
        const line = String(msg.line || '');
        _terminalLinkCache.set(terminal._handle, new Map());
        const tokenSource = new CancellationTokenSource();
        const context = { terminal, line };
        const results = [];
        for (const entry of _terminalLinkProviders) {
            try {
                const raw = await entry.provider.provideTerminalLinks(
                    context,
                    tokenSource.token);
                const links = Array.isArray(raw) ? raw : [];
                for (const link of links) {
                    const payload = _terminalLinkPayload(
                        entry.provider,
                        link,
                        terminal._handle);
                    if (payload) results.push(payload);
                }
            } catch (err) {
                log(`terminal link provider failed: ${err?.message || err}`);
            }
        }
        send({
            type: 'terminal_link_response',
            requestId,
            ok: true,
            terminalId: terminal._handle,
            line,
            value: results,
        });
    } catch (err) {
        send({
            type: 'terminal_link_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
            value: [],
        });
    }
}

async function handleTerminalLinkActivate(msg) {
    const requestId = msg.requestId;
    try {
        const terminal = _terminalForLinkRequest(msg);
        if (!terminal) {
            throw new Error('Terminal not found for link activation');
        }
        const linkId = Number(msg.linkId ?? msg.id);
        const cached = _terminalLinkCache.get(terminal._handle)?.get(linkId);
        if (!cached) {
            throw new Error(`Terminal link not found: ${linkId}`);
        }
        if (cached.provider && typeof cached.provider.handleTerminalLink === 'function') {
            await cached.provider.handleTerminalLink(cached.link);
        }
        send({
            type: 'terminal_link_activate_response',
            requestId,
            ok: true,
            terminalId: terminal._handle,
            linkId,
        });
    } catch (err) {
        send({
            type: 'terminal_link_activate_response',
            requestId,
            ok: false,
            error: err?.message || String(err),
        });
    }
}

function handleScmSetInputValue(msg) {
    const providerId = String(msg.providerId || msg.id || '');
    const provider = _scmProviders.get(providerId);
    if (!provider || !provider.inputBox) return;
    provider.inputBox.value = String(msg.value || '');
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
        case 'webview_panel_view_state':
            handleWebviewPanelViewState(msg);
            break;
        case 'webview_state':
            handleWebviewState(msg);
            break;
        case 'dispose_webview_panel':
            handleDisposeWebviewPanel(msg);
            break;
        case 'resolve_webview_view':
            await resolveWebviewView(msg.viewType, msg.state);
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
        case 'window_message_response':
            _handleWindowMessageResponse(msg);
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
        case 'tree_drag_drop_request':
            await handleTreeDragDropRequest(msg);
            break;
        case 'language_provider_request':
            await handleLanguageProviderRequest(msg);
            break;
        case 'language_provider_cancel':
            handleLanguageProviderCancel(msg);
            break;
        case 'file_decoration_request':
            await handleFileDecorationRequest(msg);
            break;
        case 'file_decoration_cancel':
            handleFileDecorationCancel(msg);
            break;
        case 'scm_quick_diff_request':
            await handleScmQuickDiffRequest(msg);
            break;
        case 'scm_quick_diff_cancel':
            handleScmQuickDiffCancel(msg);
            break;
        case 'scm_history_request':
            await handleScmHistoryRequest(msg);
            break;
        case 'scm_history_cancel':
            handleScmHistoryCancel(msg);
            break;
        case 'scm_validate_input':
            await handleScmValidateInput(msg);
            break;
        case 'terminal_profile_request':
            await handleTerminalProfileRequest(msg);
            break;
        case 'terminal_dispose_request':
            _disposeTerminalById(msg.id ?? msg.handle);
            break;
        case 'terminal_link_request':
            await handleTerminalLinkRequest(msg);
            break;
        case 'terminal_link_activate':
            await handleTerminalLinkActivate(msg);
            break;
        case 'scm_set_input_value':
            handleScmSetInputValue(msg);
            break;
        case 'notebook_deserialize_request':
            await _handleNotebookDeserializeRequest(msg);
            break;
        case 'notebook_serialize_request':
            await _handleNotebookSerializeRequest(msg);
            break;
        case 'notebook_cell_status_bar_request':
            await _handleNotebookCellStatusBarRequest(msg);
            break;
        case 'notebook_controllers_request':
            await _handleNotebookControllersRequest(msg);
            break;
        case 'notebook_controller_select_request':
            await _handleNotebookControllerSelectRequest(msg);
            break;
        case 'notebook_controller_execute_request':
            await _handleNotebookControllerExecuteRequest(msg);
            break;
        case 'extension_task_execute_request':
            await handleExtensionTaskExecuteRequest(msg);
            break;
        case 'extension_debug_start_request':
            await handleExtensionDebugStartRequest(msg);
            break;
        case 'lm_tool_request':
            await handleLmToolRequest(msg);
            break;
        case 'mcp_server_definitions_request':
            await handleMcpServerDefinitionsRequest(msg);
            break;
        case 'mcp_server_resolve_request':
            await handleMcpServerResolveRequest(msg);
            break;
        case 'chat_participant_request':
            await handleChatParticipantRequest(msg);
            break;
        case 'chat_context_provider_request':
            await handleChatContextProviderRequest(msg);
            break;
        case 'chat_context_resolve_request':
            await handleChatContextResolveRequest(msg);
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
                _rebuildConfigurationTargetsFromSettings();
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
                    const targetName = String(msg.target || '') || 'workspace';
                    _configDeleteTargetValue(changedPath, targetName);
                    if (!msg.target || msg.target === 'workspace') {
                        _configDelete(_settings, changedPath);
                        _configMirrorAiEditorAlias(changedPath, undefined, true);
                    }
                } else if (msg.key !== undefined) {
                    const targetName = String(msg.target || '') || 'workspace';
                    _configSetTargetValue(changedPath, targetName, msg.value);
                    if (!msg.target || msg.target === 'workspace') {
                        _configSet(_settings, changedPath, msg.value);
                        _configMirrorAiEditorAlias(changedPath, msg.value, false);
                    }
                } else if (msg.value !== undefined && typeof msg.value === 'object') {
                    const sectionPath = _configPath(changedSection);
                    _configSet(_settings, sectionPath, msg.value);
                    _configMirrorAiEditorAlias(sectionPath, msg.value, false);
                    _rebuildConfigurationTargetsFromSettings();
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
