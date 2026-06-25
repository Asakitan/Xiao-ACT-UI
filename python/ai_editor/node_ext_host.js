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

const NotebookCellKind = Object.freeze({ Markup: 1, Code: 2 });

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
    _notebookDocuments.set(uri.toString(), notebook);
    _onDidOpenNotebookDocumentEmitter.fire(notebook);
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
        this._state = null;
        this._viewType = '';
        this._title = '';
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
        send({ type: 'webview_post_message', viewId: this._viewId, message });
        return Promise.resolve(true);
    }
    asWebviewUri(localUri) {
        this._assertAlive();
        return _asWebviewResourceUri(localUri);
    }
    _setState(state) {
        this._state = state === undefined ? null : state;
    }
    _setPanelMetadata(viewType, title) {
        this._viewType = String(viewType || '');
        this._title = String(title || '');
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
        this._viewId = viewId;
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
    set title(v) {
        const next = String(v || '');
        if (this._title === next) return;
        this._title = next;
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
    constructor(name, languageId = '') {
        this.name = name;
        this.languageId = languageId || '';
        this._lines = [];
        this._disposed = false;
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

class ProcessExecution {
    constructor(processValue, argsOrOptions, options) {
        this.process = String(processValue || '');
        if (Array.isArray(argsOrOptions)) {
            this.args = argsOrOptions.map(item => String(item));
            this.options = options || {};
        } else {
            this.args = [];
            this.options = argsOrOptions || {};
        }
    }
}

class ShellExecution {
    constructor(commandLineOrCommand, argsOrOptions, options) {
        if (Array.isArray(argsOrOptions)) {
            this.command = String(commandLineOrCommand || '');
            this.args = argsOrOptions.map(item => String(item));
            this.options = options || {};
            this.commandLine = undefined;
        } else {
            this.commandLine = String(commandLineOrCommand || '');
            this.command = undefined;
            this.args = [];
            this.options = argsOrOptions || {};
        }
    }
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
            this.problemMatchers = executionOrProblemMatchers || [];
        } else {
            this.scope = scopeOrName;
            this.name = String(nameOrSource || this.definition.type || 'task');
            this.source = String(sourceOrExecution || '');
            this.execution = executionOrProblemMatchers;
            this.problemMatchers = problemMatchers || [];
        }
        this.isBackground = false;
        this.group = undefined;
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
const _commands = new Map();             // commandId -> handler
const _pythonCommandRequests = new Map(); // requestId -> { resolve, reject, timer }
const _pythonLmRequests = new Map();      // requestId -> { resolve, reject, timer }
const _windowDialogRequests = new Map();  // requestId -> { resolve, timer, cleanup, kind }
const _envClipboardRequests = new Map();  // requestId -> { resolve, timer, cleanup, action }
const _webviewViewProviders = new Map(); // viewType -> { provider, options }
const _webviewViews = new Map();         // viewId -> WebviewView
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
const _chatParticipants = new Map();     // id -> { handle, handler, extensionId }
let _nextLanguageProviderHandle = 1;
let _nextLanguageStatusHandle = 1;
let _nextFileDecorationProviderHandle = 1;
let _nextLanguageConfigurationHandle = 1;
let _nextLmToolHandle = 1;
let _nextChatParticipantHandle = 1;
let _nextPythonCommandRequestHandle = 1;
let _nextPythonLmRequestHandle = 1;
let _nextExtensionActivationRequestHandle = 1;
let _nextWindowDialogRequestHandle = 1;
let _nextEnvClipboardRequestHandle = 1;
let _nextTaskExecutionHandle = 1;
let _nextDebugSessionHandle = 1;
let _activeDebugSession = null;
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
const _onDidChangeWorkspaceFoldersEmitter = new EventEmitter();
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
        return Object.assign({}, nameOrOptions);
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
    return options;
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
    return !filterType || _taskType(task) === filterType;
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
        presentationOptions: _plainBridgeValue(task.presentationOptions || {}),
        runOptions: _plainBridgeValue(task.runOptions || {}),
    };
}

function _quoteCommandToken(value) {
    const text = String(value ?? '');
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
        configuration: _plainBridgeValue(session.configuration || {}),
        adapterDescriptor: _debugAdapterDescriptorPayload(
            session.adapterDescriptor),
    };
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
    return _plainBridgeValue(descriptor);
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

function _createNotebookController(extDesc, id, notebookType, label, handler) {
    const normalizedId = String(id || '').trim();
    const normalizedType = String(notebookType || '').trim();
    if (!normalizedId || !normalizedType) {
        throw new Error('NotebookController id and notebookType are required');
    }
    const handle = _nextNotebookControllerHandle++;
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
        createNotebookCellExecution(cell) {
            return _createNotebookCellExecution(controller, cell);
        },
        dispose() {
            _notebookControllers.delete(handle);
        },
    };
    _notebookControllers.set(handle, controller);
    send({
        type: 'notebook_controller_registered',
        handle,
        id: normalizedId,
        notebookType: normalizedType,
        label: controller.label,
        extensionId: controller.extensionId,
    });
    return controller;
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
        this._handle = _nextTerminalHandle++;
        this._disposed = false;
        this._ptyOpened = false;
        this._ptyDisposables = [];
        this.creationOptions = Object.assign({}, options || {});
        this.name = String(options?.name || `Terminal ${this._handle}`);
        this.processId = Promise.resolve(undefined);
        this.exitStatus = undefined;
        this.state = { isInteractedWith: false };
        this.shellIntegration = undefined;
        this._shellIntegrationObject = null;
        this.dimensions = undefined;
        this._pty = options?.pty && typeof options.pty.open === 'function'
            ? options.pty
            : null;
        this._bindPseudoterminal();
    }
    _bindPseudoterminal() {
        const pty = this._pty;
        if (!pty) return;
        const writeDisposable = _terminalSubscribeEvent(pty.onDidWrite, data => {
            if (!this._ptyOpened || this._disposed) return;
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
    sendText(text, shouldExecute = true) {
        if (this._disposed) return;
        this.state.isInteractedWith = true;
        this._ensureShellIntegration();
        _onDidChangeTerminalStateEmitter.fire(this);
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
            isTransient: options.isTransient === true,
            isPseudoterminal: !!this._pty,
            shellIntegration: !!this.shellIntegration,
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
        const envValue = _terminalOptionValue(terminal.creationOptions?.env);
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
        return _plainBridgeValue(this.env);
    }
    executeCommand(commandLineOrExecutable, args) {
        const commandLine = _terminalShellCommandLine(commandLineOrExecutable, args);
        const execution = new TerminalShellExecution(this._terminal, commandLine);
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
    const terminalOptions = Object.assign(
        {},
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
        NotebookData,
        NotebookEdit,
        NotebookRange,
        Disposable,
        EventEmitter,
        CancellationTokenSource,
        TerminalProfile,
        TerminalLink,

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
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
        LanguageStatusSeverity: { Information: 0, Warning: 1, Error: 2 },
        NotebookCellKind,
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
                });
                setImmediate(() => resolveWebviewView(viewType));
                log(`registered WebviewViewProvider: ${viewType}`);
                return new Disposable(() => {
                    _webviewViewProviders.delete(viewType);
                    send({
                        type: 'webview_view_provider_disposed',
                        viewType,
                        extensionId: extDesc.extensionId || '',
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
                const ch = new OutputChannel(name, languageId);
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
            onDidChangeWorkspaceFolders: _onDidChangeWorkspaceFoldersEmitter.event,
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
                async startDebugging(folder, config, options) {
                    log('debug: startDebugging');
                    if (!config || typeof config !== 'object') return false;
                    const debugType = String(config.type || '');
                    const provider = _debugConfigProviders.get(debugType);
                    if (provider && typeof provider.resolveDebugConfiguration === 'function') {
                        const resolved = await Promise.resolve(
                            provider.resolveDebugConfiguration(folder, { ...config }));
                        if (!resolved) return false;
                        config = resolved;
                    }
                    const session = {
                        id: `debug-${_nextDebugSessionHandle++}`,
                        type: String(config.type || debugType || 'debug'),
                        name: String(config.name || config.type || 'Debug'),
                        workspaceFolder: folder || undefined,
                        configuration: { ...config },
                        parentSession: options && options.parentSession,
                    };
                    const descriptorFactory = _debugAdapterFactories.get(session.type);
                    if (descriptorFactory
                            && typeof descriptorFactory.createDebugAdapterDescriptor === 'function') {
                        session.adapterDescriptor = await Promise.resolve(
                            descriptorFactory.createDebugAdapterDescriptor(
                                session,
                                undefined));
                    }
                    _debugUpdateActive(session, _onDidChangeActiveDebugSession);
                    _onDidStartDebugSession.fire(session);
                    send({
                        type: 'debug_start',
                        session: _debugSessionPayload(session),
                        config: _plainBridgeValue(config || {}),
                    });
                    return true;
                },
                stopDebugging(session) {
                    const target = session || _activeDebugSession;
                    if (!target) return Promise.resolve();
                    if (_activeDebugSession && _activeDebugSession.id === target.id) {
                        _debugUpdateActive(null, _onDidChangeActiveDebugSession);
                    }
                    _onDidTerminateDebugSession.fire(target);
                    send({
                        type: 'debug_stop',
                        session: _debugSessionPayload(target),
                    });
                    return Promise.resolve();
                },
                get activeDebugSession() { return _activeDebugSession; },
                get breakpoints() { return []; },
                onDidChangeActiveDebugSession: _onDidChangeActiveDebugSession.event,
                onDidStartDebugSession: _onDidStartDebugSession.event,
                onDidTerminateDebugSession: _onDidTerminateDebugSession.event,
                onDidChangeBreakpoints: new EventEmitter().event,
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
                const index = _taskExecutions.indexOf(execution);
                if (index >= 0) _taskExecutions.splice(index, 1);
                _onDidEndTask.fire({ execution, task: execution.task });
                _onDidEndTaskProcess.fire({ execution, exitCode });
            };
            return {
                registerTaskProvider(type, provider) {
                    _taskProviders.set(type, provider);
                    log(`tasks: registered provider for "${type}"`);
                    return new Disposable(() => _taskProviders.delete(type));
                },
                async fetchTasks(filter) {
                    log('tasks: fetchTasks');
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
                            send({
                                type: 'task_terminate',
                                executionId: execution.id,
                                task: _serializeTask(resolved),
                            });
                            endExecution(execution, undefined);
                            return Promise.resolve();
                        },
                    };
                    const executionSpec = _taskExecutionSpec(resolved);
                    _taskExecutions.push(execution);
                    _onDidStartTask.fire({ execution, task: resolved });
                    _onDidStartTaskProcess.fire({ execution, processId: 0 });
                    send({
                        type: 'task_execute',
                        executionId: execution.id,
                        task: _serializeTask(resolved),
                        commandLine: executionSpec.commandLine || '',
                        metadata: {
                            cwd: executionSpec.cwd,
                            env: executionSpec.env,
                            kind: executionSpec.kind,
                        },
                    });
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

        // --- Types used by some extensions ---
        ThemeIcon,
        ThemeColor,
        ProcessExecution,
        ShellExecution,
        Task,
        DebugAdapterExecutable,
        DebugAdapterServer,
        DebugAdapterNamedPipeServer,
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
        Hover: class { constructor(contents, range) { this.contents = Array.isArray(contents) ? contents : [contents]; this.range = range; } },
        DocumentLink: class { constructor(range, target) { this.range = range; this.target = target; } },
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
            if (globalValue === _CONFIG_MISSING
                    && workspaceValue === _CONFIG_MISSING
                    && workspaceFolderValue === _CONFIG_MISSING
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
                target: _configurationUpdateTargets[fullKey] || undefined,
            };
        },
        update(key, value, configTarget, overrideInLanguage) {
            const pathParts = _configFullPath(section, key);
            const fullKey = pathParts.join('.');
            const targetName = _configurationTargetName(configTarget);
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
        const respondCancelled = () => {
            send({
                type: 'language_provider_response',
                requestId,
                ok: false,
                kind,
                cancelled: true,
                error: 'Language provider request cancelled',
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
                    log(`language provider ${kind} error: ${err.message}`);
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
                    log(`language provider ${kind} error: ${err.message}`);
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
                    log(`language provider ${kind} error: ${err.message}`);
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
                if (token.isCancellationRequested) { respondCancelled(); return; }
                const provider = entry.provider;
                const fn = provider && provider[methodName];
                if (typeof fn !== 'function') continue;
                try {
                    await fn.call(provider, document, pasteRanges, dataTransfer, token);
                    if (token.isCancellationRequested) { respondCancelled(); return; }
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
            const codeActionContext = {
                diagnostics: _codeActionDiagnostics(document, msg),
                only: msg.only,
                triggerKind: msg.triggerKind,
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
