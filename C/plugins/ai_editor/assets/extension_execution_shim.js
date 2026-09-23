'use strict';

module.exports = function installExecutionApis(host) {
    const {
        state, EventEmitter, disposable, disposalBarrier, requireActivationScope,
        activationScopeCurrent, runWithActivation, runWithDeadline,
        surfaceOwner, observeRegistration, boundedSurfaceSnapshot, callHost, callHostObserved,
        reportAsyncFailure, neverCancellationToken, registerProcessRoute,
        queueEarlyProcessEvent, processEventId, terminalRecordById,
        newTerminalRecord, reviveTerminal, finalizeTerminal, markTerminalOpen,
        terminalEvents, Uri,
    } = host;

    const providerTimeoutMs = 10000;
    const dapTimeoutMs = 15000;
    const launchTimeoutMs = 30000;
    const maximumTasks = 512;
    const maximumBreakpoints = 4096;
    const maximumPendingDapRequests = 256;
    const maximumPayloadBytes = 1024 * 1024;

    state.terminalProfileProviders ||= new Map();
    state.debugInlineAdapters ||= new Map();

    const taskEvents = {
        start: new EventEmitter(),
        end: new EventEmitter(),
        processStart: new EventEmitter(),
        processEnd: new EventEmitter(),
    };
    const debugEvents = {
        start: new EventEmitter(),
        terminate: new EventEmitter(),
        active: new EventEmitter(),
        custom: new EventEmitter(),
        breakpoints: new EventEmitter(),
        console: new EventEmitter(),
    };

    function ownerCurrent(record) {
        return record && record.active && activationScopeCurrent(record.scope);
    }

    function executionScope(apiName, preferredType, providerKind, category,
        preferredKey) {
        try { return requireActivationScope(apiName); }
        catch (_) {
            if (preferredKey) {
                const preferred = category === 'debug'
                    ? state.debugProviders.get(String(preferredKey))
                    : state.taskProviders.get(String(preferredKey));
                if (ownerCurrent(preferred) &&
                    (!preferredType || preferred.type === preferredType) &&
                    (!providerKind || preferred.kind === providerKind)) {
                    return preferred.scope;
                }
            }
            if (category !== 'task') {
                for (const provider of state.debugProviders.values()) {
                    if (ownerCurrent(provider) &&
                        (!preferredType || provider.type === preferredType) &&
                        (!providerKind || provider.kind === providerKind)) return provider.scope;
                }
            }
            if (category !== 'debug') {
                for (const provider of state.taskProviders.values())
                    if (ownerCurrent(provider) &&
                        (!preferredType || provider.type === preferredType)) return provider.scope;
            }
            return { extensionId: '', generation: 0, hostOwned: true,
                phase: 'active', journal: [], pendingRegistrations: [],
                seen: new WeakSet(), subscriptions: [] };
        }
    }

    function bounded(value, label) {
        return boundedSurfaceSnapshot(value, label, maximumPayloadBytes);
    }

    function folderSnapshot(folder) {
        if (!folder) return null;
        if (typeof folder === 'string') return { uri: folder };
        const uri = folder.uri && typeof folder.uri.toString === 'function'
            ? folder.uri.toString() : folder.uri;
        return bounded({ uri: uri || '', name: folder.name || '', index: folder.index ?? 0 },
            'Workspace folder');
    }

    class ProcessExecution {
        constructor(process, args = [], options = {}) {
            this.process = String(process || '');
            this.args = Array.isArray(args) ? args.map(value => String(value)) : [];
            this.options = options && typeof options === 'object' ? options : {};
            if (!this.process) throw new TypeError('ProcessExecution requires a process');
        }
    }

    class ShellExecution {
        constructor(commandLineOrCommand, argsOrOptions, options) {
            if (Array.isArray(argsOrOptions)) {
                this.command = String(commandLineOrCommand || '');
                this.args = argsOrOptions.map(value => String(value));
                this.options = options && typeof options === 'object' ? options : {};
            } else {
                this.commandLine = String(commandLineOrCommand || '');
                this.options = argsOrOptions && typeof argsOrOptions === 'object'
                    ? argsOrOptions : {};
            }
            if (!this.command && !this.commandLine)
                throw new TypeError('ShellExecution requires a command');
        }
    }

    class CustomExecution {
        constructor(callback) {
            if (typeof callback !== 'function')
                throw new TypeError('CustomExecution requires a callback');
            this.callback = callback;
        }
    }

    class Task {
        constructor(definition, scope, name, source, execution, problemMatchers = []) {
            if (!definition || typeof definition !== 'object' || Array.isArray(definition))
                throw new TypeError('Task definition must be an object');
            this.definition = bounded(definition, 'Task definition');
            this.scope = scope;
            this.name = String(name || 'Task');
            this.source = String(source || 'Extension');
            this.execution = execution;
            this.problemMatchers = Array.isArray(problemMatchers)
                ? problemMatchers.map(value => String(value))
                : problemMatchers ? [String(problemMatchers)] : [];
            this.isBackground = false;
            this.presentationOptions = {};
            this.runOptions = {};
            this.group = undefined;
            this.detail = undefined;
        }
    }

    function serializeExecution(execution) {
        if (execution instanceof ProcessExecution) {
            return bounded({ kind: 'process', command: execution.process,
                args: execution.args, options: execution.options }, 'ProcessExecution');
        }
        if (execution instanceof ShellExecution) {
            return bounded({ kind: 'shell', command: execution.command || '',
                commandLine: execution.commandLine || '', args: execution.args || [],
                options: execution.options }, 'ShellExecution');
        }
        if (execution instanceof CustomExecution) return { kind: 'custom' };
        if (execution && typeof execution === 'object') return bounded(execution, 'Task execution');
        return null;
    }

    function serializeTask(task) {
        if (!(task instanceof Task) && (!task || typeof task !== 'object'))
            throw new TypeError('Expected Task');
        const value = {
            definition: task.definition || {}, scope: typeof task.scope === 'number'
                ? task.scope : folderSnapshot(task.scope), name: String(task.name || 'Task'),
            source: String(task.source || 'Extension'), execution: serializeExecution(task.execution),
            problemMatchers: Array.isArray(task.problemMatchers) ? task.problemMatchers : [],
            isBackground: task.isBackground === true,
            presentationOptions: task.presentationOptions || {}, runOptions: task.runOptions || {},
            group: task.group && (task.group.id || task.group), detail: task.detail,
            providerKey: task._providerKey,
        };
        return bounded(value, 'Task');
    }

    function reviveExecution(value) {
        if (!value || typeof value !== 'object') return undefined;
        if (value.kind === 'process') return new ProcessExecution(value.command, value.args, value.options);
        if (value.kind === 'shell') return value.commandLine
            ? new ShellExecution(value.commandLine, value.options)
            : new ShellExecution(value.command, value.args, value.options);
        return value;
    }

    function reviveTask(value, providerKey) {
        if (value instanceof Task) return value;
        const task = new Task(value.definition || { type: value.type || 'shell' }, value.scope,
            value.name || value.label || 'Task', value.source || 'Extension',
            reviveExecution(value.execution), value.problemMatchers || []);
        task.isBackground = value.isBackground === true;
        task.presentationOptions = value.presentationOptions || {};
        task.runOptions = value.runOptions || {};
        task.group = value.group;
        task.detail = value.detail;
        task._providerKey = providerKey || value.providerKey;
        return task;
    }

    class TaskExecution {
        constructor(record) {
            this._record = record;
            this.task = record.task;
        }
        get id() { return this._record.id; }
        terminate() {
            if (!this._record.active) return Promise.resolve();
            return callHost('vscode.tasks.terminateTask', {
                ...this._record.owner, executionId: this._record.id,
            }).catch(error => reportAsyncFailure('task termination failed', error));
        }
    }

    function registerTaskProvider(type, provider) {
        const scope = requireActivationScope('registerTaskProvider');
        const taskType = String(type || '');
        if (!taskType || taskType.length > 128 || !provider ||
            typeof provider.provideTasks !== 'function') {
            throw new TypeError('registerTaskProvider requires a type and provider');
        }
        if (state.taskProviders.size >= maximumTasks)
            throw new Error('TaskProvider limit reached');
        const owner = surfaceOwner(scope, 'task-provider');
        const record = { type: taskType, provider, scope, owner, active: true,
            key: owner.registrationId };
        state.taskProviders.set(record.key, record);
        const ready = observeRegistration(scope, callHost('vscode.tasks.registerTaskProvider', {
            ...owner, type: taskType,
        }).then(result => {
            if (!ownerCurrent(record)) throw new Error('TaskProvider generation retired');
            const key = String(result.key || record.key);
            state.taskProviders.delete(record.key);
            record.key = key;
            state.taskProviders.set(key, record);
            return record;
        }).catch(error => {
            record.active = false;
            state.taskProviders.delete(record.key);
            throw error;
        }));
        record.ready = ready;
        const value = disposable(() => {
            record.active = false;
            state.taskProviders.delete(record.key);
            return value[disposalBarrier] = ready.then(() => callHost(
                'vscode.tasks.unregisterTaskProvider', {
                    ...owner, key: record.key,
                }), () => undefined);
        });
        value.ready = ready;
        return value;
    }

    async function invokeTaskProvider(request) {
        const record = state.taskProviders.get(String(request.key || ''));
        if (!ownerCurrent(record) || record.owner.extensionId !== request.extensionId ||
            record.owner.generation !== request.generation) {
            throw new Error('Stale task provider generation');
        }
        if (request.invoke === 'tasks.fetch') {
            const supplied = await runWithDeadline(
                () => runWithActivation(record.scope,
                    () => record.provider.provideTasks(neverCancellationToken)),
                Date.now() + providerTimeoutMs, 'TaskProvider timed out');
            const values = await Promise.resolve(supplied);
            if (!ownerCurrent(record))
                throw new Error('TaskProvider generation retired during callback');
            if (!Array.isArray(values)) return { tasks: [] };
            return { tasks: values.slice(0, maximumTasks).map(task => {
                const serialized = serializeTask(task);
                serialized.providerKey = record.key;
                return serialized;
            }) };
        }
        if (request.invoke === 'tasks.resolve') {
            if (typeof record.provider.resolveTask !== 'function')
                return { task: request.task || null };
            const candidate = reviveTask(request.task || {}, record.key);
            const resolved = await runWithDeadline(
                () => runWithActivation(record.scope,
                    () => record.provider.resolveTask(candidate, neverCancellationToken)),
                Date.now() + providerTimeoutMs, 'TaskProvider resolve timed out');
            const value = await Promise.resolve(resolved);
            if (!ownerCurrent(record))
                throw new Error('TaskProvider generation retired during callback');
            return { task: value ? serializeTask(value) : null };
        }
        throw new Error('Unsupported task provider callback');
    }

    async function fetchTasks(filter) {
        const type = filter && filter.type ? String(filter.type) : '';
        const items = [];
        for (const record of Array.from(state.taskProviders.values())) {
            if (!ownerCurrent(record) || (type && record.type !== type)) continue;
            await record.ready;
            const result = await invokeTaskProvider({ invoke: 'tasks.fetch',
                key: record.key, extensionId: record.owner.extensionId,
                generation: record.owner.generation, type: record.type });
            for (const value of result.tasks || []) {
                if (items.length >= maximumTasks) break;
                items.push(reviveTask(value, record.key));
            }
        }
        return items;
    }

    function makeTaskRoute(record) {
        return payload => handleTaskEvent(payload, record);
    }

    async function executeTask(task) {
        task = reviveTask(task || {}, task?._providerKey || task?.providerKey);
        const providerKey = task._providerKey || task.providerKey;
        if (!task.execution && providerKey) {
            const provider = state.taskProviders.get(String(providerKey));
            if (!ownerCurrent(provider))
                throw new Error('Task provider generation is no longer active');
            await provider.ready;
            const resolved = await invokeTaskProvider({ invoke: 'tasks.resolve',
                key: provider.key, extensionId: provider.owner.extensionId,
                generation: provider.owner.generation,
                task: serializeTask(task) });
            if (!resolved.task)
                throw new Error('TaskProvider did not resolve the task');
            task = reviveTask(resolved.task, provider.key);
        }
        if (!task.execution)
            throw new Error('Task has no executable definition');
        const scope = executionScope('executeTask', task.definition?.type,
            undefined, 'task', providerKey);
        const owner = surfaceOwner(scope, 'task-execution');
        const id = owner.registrationId;
        const terminalId = `${id}-terminal`;
        const serialized = serializeTask(task);
        let terminalRecord;
        if (task.execution instanceof CustomExecution) {
            const pty = await Promise.resolve(runWithActivation(scope,
                () => task.execution.callback()));
            if (!pty || typeof pty.open !== 'function' || typeof pty.close !== 'function')
                throw new TypeError('CustomExecution callback must return a Pseudoterminal');
            terminalRecord = newTerminalRecord(terminalId, task.name, task.execution,
                pty, scope, owner);
        }
        const record = { id, task, owner, scope, active: true, terminalId,
            execution: undefined, routeDispose: undefined };
        record.execution = new TaskExecution(record);
        state.taskExecutions.set(id, record);
        record.routeDispose = registerProcessRoute(id, makeTaskRoute(record));
        try {
            const result = await callHost('vscode.tasks.executeTask', {
                ...owner, executionId: id, terminalId, task: serialized,
            });
            if (!ownerCurrent({ active: record.active, scope }))
                throw new Error('Task execution generation retired');
            const snapshot = result.snapshot || result;
            if (terminalRecord) markTerminalOpen(terminalRecord, snapshot);
            else reviveTerminal(snapshot);
            return record.execution;
        } catch (error) {
            record.active = false;
            state.taskExecutions.delete(id);
            record.routeDispose?.();
            if (terminalRecord) finalizeTerminal(terminalRecord, { code: undefined, reason: 4 });
            throw error;
        }
    }

    function handleTaskEvent(payload, existing) {
        const id = String(payload.executionId || processEventId(payload));
        const record = existing || state.taskExecutions.get(id);
        if (!record) {
            queueEarlyProcessEvent(payload);
            return false;
        }
        const op = String(payload.op || '');
        if (op === 'start') taskEvents.start.fire({ execution: record.execution });
        else if (op === 'processStart') taskEvents.processStart.fire({
            execution: record.execution, processId: Number(payload.processId) || 0,
        });
        else if (op === 'processEnd') taskEvents.processEnd.fire({
            execution: record.execution, exitCode: Number.isInteger(payload.exitCode)
                ? payload.exitCode : undefined,
        });
        else if (op === 'end') {
            record.active = false;
            taskEvents.end.fire({ execution: record.execution });
            callHost('vscode.tasks.terminateTask', {
                ...record.owner, executionId: record.id,
            }).catch(() => undefined);
            state.taskExecutions.delete(id);
            record.routeDispose?.();
        }
        return true;
    }

    const tasks = {
        registerTaskProvider,
        fetchTasks,
        executeTask,
        get taskExecutions() {
            return Array.from(state.taskExecutions.values())
                .filter(record => record.active).map(record => record.execution);
        },
        onDidStartTask: taskEvents.start.event,
        onDidEndTask: taskEvents.end.event,
        onDidStartTaskProcess: taskEvents.processStart.event,
        onDidEndTaskProcess: taskEvents.processEnd.event,
    };

    class DebugAdapterExecutable {
        constructor(command, args = [], options = {}) {
            this.command = String(command || '');
            this.args = Array.isArray(args) ? args.map(value => String(value)) : [];
            this.options = options && typeof options === 'object' ? options : {};
            if (!this.command) throw new TypeError('DebugAdapterExecutable requires a command');
        }
    }
    class DebugAdapterServer {
        constructor(port, hostName = '127.0.0.1') {
            this.port = Number(port);
            this.host = String(hostName || '127.0.0.1');
        }
    }
    class DebugAdapterNamedPipeServer {
        constructor(path) { this.path = String(path || ''); }
    }
    class DebugAdapterInlineImplementation {
        constructor(implementation) {
            if (!implementation || typeof implementation.handleMessage !== 'function')
                throw new TypeError('Inline debug adapter requires handleMessage');
            this.implementation = implementation;
        }
    }

    class DebugSession {
        constructor(record) {
            this._record = record;
            this.id = record.id;
            this.type = String(record.configuration.type || '');
            this.name = String(record.configuration.name || 'Debug');
            this.configuration = record.configuration;
            this.workspaceFolder = record.folder || undefined;
            this.parentSession = record.parentSession || undefined;
        }
        customRequest(command, args) {
            return sendDapRequest(this._record, String(command), args || {});
        }
        getDebugProtocolBreakpoint(breakpoint) {
            return this._record.protocolBreakpoints.get(breakpoint?.id);
        }
    }

    let nextBreakpointId = 1;
    class SourceBreakpoint {
        constructor(location, enabled = true, condition, hitCondition, logMessage) {
            this.id = `bp-${nextBreakpointId++}`;
            this.location = location;
            this.enabled = enabled !== false;
            this.condition = condition;
            this.hitCondition = hitCondition;
            this.logMessage = logMessage;
        }
    }
    class FunctionBreakpoint {
        constructor(functionName, enabled = true, condition, hitCondition, logMessage) {
            this.id = `bp-${nextBreakpointId++}`;
            this.functionName = String(functionName || '');
            this.enabled = enabled !== false;
            this.condition = condition;
            this.hitCondition = hitCondition;
            this.logMessage = logMessage;
        }
    }
    class DataBreakpoint {
        constructor(label, dataId, canPersist, enabled = true, hitCondition, condition, accessType) {
            this.id = `bp-${nextBreakpointId++}`;
            Object.assign(this, { label, dataId, canPersist, enabled, hitCondition, condition, accessType });
        }
    }

    function descriptorSnapshot(descriptor, scope) {
        if (descriptor instanceof DebugAdapterExecutable) return bounded({
            kind: 'executable', command: descriptor.command,
            args: descriptor.args, options: descriptor.options,
        }, 'Debug adapter executable');
        if (descriptor instanceof DebugAdapterServer) return {
            kind: 'server', port: descriptor.port, host: descriptor.host,
        };
        if (descriptor instanceof DebugAdapterNamedPipeServer) return {
            kind: 'namedPipe', path: descriptor.path,
        };
        if (descriptor instanceof DebugAdapterInlineImplementation) {
            if (state.debugInlineAdapters.size >= maximumTasks)
                throw new Error('Inline debug adapter limit reached');
            const id = surfaceOwner(scope, 'debug-inline').registrationId;
            state.debugInlineAdapters.set(id, {
                implementation: descriptor.implementation, scope,
            });
            return { kind: 'inline', inlineId: id };
        }
        if (descriptor && typeof descriptor === 'object')
            return bounded(descriptor, 'Debug adapter descriptor');
        return null;
    }

    function registerDebugProvider(type, provider, kind, triggerKind = 0) {
        const scope = requireActivationScope(`registerDebug${kind}`);
        const debugType = String(type || '');
        if (!debugType || debugType.length > 128 || !provider)
            throw new TypeError('Debug provider requires a type and provider');
        if (state.debugProviders.size >= maximumTasks)
            throw new Error('Debug provider limit reached');
        const owner = surfaceOwner(scope, 'debug-provider');
        const record = { type: debugType, provider, kind, triggerKind,
            owner, scope, active: true, key: owner.registrationId };
        state.debugProviders.set(record.key, record);
        const ready = observeRegistration(scope, callHost('vscode.debug.registerProvider', {
            ...owner, type: debugType, kind, triggerKind,
        }).then(result => {
            if (!ownerCurrent(record)) throw new Error('Debug provider generation retired');
            const key = String(result.key || record.key);
            state.debugProviders.delete(record.key);
            record.key = key;
            state.debugProviders.set(key, record);
            return record;
        }).catch(error => {
            record.active = false;
            state.debugProviders.delete(record.key);
            throw error;
        }));
        record.ready = ready;
        const value = disposable(() => {
            record.active = false;
            state.debugProviders.delete(record.key);
            for (const [id, inline] of state.debugInlineAdapters) {
                if (inline.scope === record.scope) state.debugInlineAdapters.delete(id);
            }
            return value[disposalBarrier] = ready.then(() => callHost(
                'vscode.debug.unregisterProvider', { ...owner, key: record.key }),
                () => undefined);
        });
        value.ready = ready;
        return value;
    }

    function providerFolder(value) {
        if (!value) return undefined;
        const snapshot = value.uri ? value : { uri: value };
        return { uri: typeof snapshot.uri === 'string' ? Uri.parse(snapshot.uri) : snapshot.uri,
            name: snapshot.name || '', index: snapshot.index ?? 0 };
    }

    function providerSession(configuration) {
        const record = { id: 'preparing', configuration, folder: undefined,
            protocolBreakpoints: new Map() };
        return new DebugSession(record);
    }

    async function invokeDebugProvider(request) {
        const record = state.debugProviders.get(String(request.key || ''));
        if (!ownerCurrent(record) || record.owner.extensionId !== request.extensionId ||
            record.owner.generation !== request.generation)
            throw new Error('Stale debug provider generation');
        const folder = providerFolder(request.folder);
        if (request.invoke === 'debug.provideConfigurations') {
            if (typeof record.provider.provideDebugConfigurations !== 'function')
                return { configurations: [] };
            const values = await runWithDeadline(() => runWithActivation(record.scope,
                () => record.provider.provideDebugConfigurations(folder, neverCancellationToken)),
                Date.now() + providerTimeoutMs, 'Debug configuration provider timed out');
            const configurations = bounded(await Promise.resolve(values) || [],
                'Debug configurations');
            if (!ownerCurrent(record))
                throw new Error('Debug provider generation retired during callback');
            return { configurations: bounded(Array.isArray(configurations)
                ? configurations.map(value => value && typeof value === 'object'
                    ? { ...value, _saoDebugProviderKey: record.key } : value)
                : [], 'Debug configurations') };
        }
        if (request.invoke === 'debug.resolveConfiguration') {
            const method = request.substituted
                ? 'resolveDebugConfigurationWithSubstitutedVariables'
                : 'resolveDebugConfiguration';
            if (typeof record.provider[method] !== 'function')
                return { configuration: request.configuration };
            const value = await runWithDeadline(() => runWithActivation(record.scope,
                () => record.provider[method](folder, request.configuration,
                    neverCancellationToken)), Date.now() + providerTimeoutMs,
                'Debug configuration resolution timed out');
            const resolved = await Promise.resolve(value);
            if (!ownerCurrent(record))
                throw new Error('Debug provider generation retired during callback');
            return resolved === null || resolved === undefined
                ? { cancelled: true } : { configuration: bounded(resolved, 'Debug configuration') };
        }
        if (request.invoke === 'debug.createDescriptor') {
            const method = record.provider.createDebugAdapterDescriptor ||
                record.provider.createDebugAdapter;
            if (typeof method !== 'function') return { descriptor: request.executable || null };
            const value = await runWithDeadline(() => runWithActivation(record.scope,
                () => method.call(record.provider,
                    providerSession(request.configuration || {}), request.executable)),
                Date.now() + providerTimeoutMs, 'Debug adapter factory timed out');
            const descriptor = descriptorSnapshot(await Promise.resolve(value), record.scope);
            if (!ownerCurrent(record))
                throw new Error('Debug provider generation retired during callback');
            return { descriptor };
        }
        throw new Error('Unsupported debug provider callback');
    }

    function trackerCall(record, name, ...args) {
        for (const tracker of record.trackers) {
            if (tracker && typeof tracker[name] === 'function') {
                try { runWithActivation(record.scope, () => tracker[name](...args)); }
                catch (error) { reportAsyncFailure(`debug tracker ${name} failed`, error); }
            }
        }
    }

    function deliverClientDap(record, message) {
        trackerCall(record, 'onWillReceiveMessage', message);
        if (record.inline) {
            return Promise.resolve(runWithActivation(record.scope,
                () => record.inline.handleMessage(message)));
        }
        return callHost('vscode.debug.sendMessage', {
            ...record.owner, sessionId: record.id, message,
        });
    }

    function sendDapRequest(record, command, args = {}, timeoutMs = dapTimeoutMs) {
        if (!record.active) return Promise.reject(new Error('Debug session is not active'));
        if (!command || Buffer.byteLength(String(command), 'utf8') > 256)
            return Promise.reject(new Error('Debug request command is invalid'));
        if (record.pending.size >= maximumPendingDapRequests)
            return Promise.reject(new Error('Too many pending debug requests'));
        if (!Number.isSafeInteger(record.nextSeq) || record.nextSeq >= Number.MAX_SAFE_INTEGER)
            return Promise.reject(new Error('Debug request identifiers are exhausted'));
        const seq = ++record.nextSeq;
        const message = { seq, type: 'request', command, arguments: bounded(args, 'DAP request') };
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                record.pending.delete(seq);
                reject(new Error(`Debug request timed out: ${command}`));
            }, timeoutMs);
            record.pending.set(seq, { resolve, reject, timer, command });
            Promise.resolve(deliverClientDap(record, message)).catch(error => {
                const pending = record.pending.get(seq);
                if (!pending) return;
                clearTimeout(pending.timer);
                record.pending.delete(seq);
                reject(error);
            });
        });
    }

    function sendDapResponse(record, request, success, body, message) {
        const response = { seq: ++record.nextSeq, type: 'response',
            request_seq: request.seq, success: success !== false,
            command: request.command, body: body || {} };
        if (message) response.message = String(message);
        return deliverClientDap(record, response);
    }

    function breakpointSourcePath(breakpoint) {
        const uri = breakpoint?.location?.uri;
        return uri && (uri.fsPath || uri.path || uri.toString?.());
    }

    async function configureBreakpoints(record, completeConfiguration = true) {
        record.protocolBreakpoints.clear();
        const grouped = new Map();
        const functions = [];
        const data = [];
        for (const breakpoint of state.breakpoints.values()) {
            if (!breakpoint.enabled) continue;
            if (breakpoint instanceof SourceBreakpoint) {
                const source = breakpointSourcePath(breakpoint);
                if (!source) continue;
                const rows = grouped.get(source) || [];
                rows.push(breakpoint);
                grouped.set(source, rows);
            } else if (breakpoint instanceof FunctionBreakpoint) {
                functions.push(breakpoint);
            } else if (breakpoint instanceof DataBreakpoint) {
                data.push(breakpoint);
            }
        }
        for (const [source, rows] of grouped) {
            try {
                const response = await sendDapRequest(record, 'setBreakpoints', {
                    source: { path: source }, breakpoints: rows.map(value => ({
                        line: (value.location.range?.start?.line ?? value.location.range?.line ?? 0) + 1,
                        column: (value.location.range?.start?.character ?? 0) + 1,
                        condition: value.condition, hitCondition: value.hitCondition,
                        logMessage: value.logMessage,
                    })), sourceModified: false,
                });
                const protocol = response?.breakpoints || [];
                rows.forEach((value, index) => record.protocolBreakpoints.set(value.id, protocol[index]));
            } catch (error) {
                reportAsyncFailure('setBreakpoints failed', error);
            }
        }
        if (functions.length) {
            try {
                const response = await sendDapRequest(record, 'setFunctionBreakpoints', {
                    breakpoints: functions.map(value => ({ name: value.functionName,
                        condition: value.condition, hitCondition: value.hitCondition })),
                });
                const protocol = response?.breakpoints || [];
                functions.forEach((value, index) =>
                    record.protocolBreakpoints.set(value.id, protocol[index]));
            } catch (error) {
                reportAsyncFailure('setFunctionBreakpoints failed', error);
            }
        }
        if (data.length) {
            try {
                const response = await sendDapRequest(record, 'setDataBreakpoints', {
                    breakpoints: data.map(value => ({ dataId: value.dataId,
                        accessType: value.accessType, condition: value.condition,
                        hitCondition: value.hitCondition })),
                });
                const protocol = response?.breakpoints || [];
                data.forEach((value, index) =>
                    record.protocolBreakpoints.set(value.id, protocol[index]));
            } catch (error) {
                reportAsyncFailure('setDataBreakpoints failed', error);
            }
        }
        if (completeConfiguration) {
            try { await sendDapRequest(record, 'configurationDone', {}); }
            catch (error) { reportAsyncFailure('configurationDone failed', error); }
        }
    }

    function refreshActiveBreakpoints() {
        for (const record of state.debugSessions.values()) {
            if (!record.active || !record.initialized) continue;
            configureBreakpoints(record, false).catch(error =>
                reportAsyncFailure('breakpoint refresh failed', error));
        }
    }

    function finishDebugRecord(record, payload = {}) {
        if (!record || !record.active) return;
        record.active = false;
        for (const pending of record.pending.values()) {
            clearTimeout(pending.timer);
            pending.reject(new Error('Debug session ended'));
        }
        record.pending.clear();
        record.routeDispose?.();
        record.inlineSubscription?.dispose?.();
        try { record.inline?.dispose?.(); } catch (_) { /* best effort */ }
        trackerCall(record, 'onWillStopSession');
        trackerCall(record, 'onExit', payload.exitCode, payload.signal);
        state.debugSessions.delete(record.id);
        if (state.activeDebugSessionId === record.id) {
            state.activeDebugSessionId = undefined;
            debugEvents.active.fire(undefined);
        }
        debugEvents.terminate.fire(record.session);
    }

    async function handleAdapterRequest(record, message) {
        if (message.command === 'runInTerminal') {
            try {
                const args = message.arguments || {};
                const shell = Array.isArray(args.args) ? args.args : [];
                const terminal = runWithActivation(record.scope,
                    () => host.createTerminalApi({ name: args.title || 'Debug',
                        shellPath: shell[0], shellArgs: shell.slice(1), cwd: args.cwd,
                        env: args.env, strictEnv: false }));
                const processId = await terminal.processId;
                await sendDapResponse(record, message, true, { processId });
            } catch (error) {
                await sendDapResponse(record, message, false, {}, error.message);
            }
            return;
        }
        if (message.command === 'startDebugging') {
            try {
                const ok = await runWithActivation(record.scope, () =>
                    startDebugging(record.folder,
                        message.arguments?.configuration || {}, record.session));
                await sendDapResponse(record, message, ok, {});
            } catch (error) {
                await sendDapResponse(record, message, false, {}, error.message);
            }
            return;
        }
        await sendDapResponse(record, message, false, {},
            `Unsupported reverse request: ${message.command}`);
    }

    function handleDapMessage(record, message) {
        trackerCall(record, 'onDidSendMessage', message);
        if (message.type === 'response') {
            const pending = record.pending.get(Number(message.request_seq));
            if (pending) {
                clearTimeout(pending.timer);
                record.pending.delete(Number(message.request_seq));
                if (message.success === false) pending.reject(new Error(
                    message.message || `Debug request failed: ${pending.command}`));
                else pending.resolve(message.body || {});
            }
            return;
        }
        if (message.type === 'request') {
            handleAdapterRequest(record, message).catch(error =>
                reportAsyncFailure('debug reverse request failed', error));
            return;
        }
        if (message.type !== 'event') return;
        if (message.event === 'initialized') {
            record.initialized = true;
            configureBreakpoints(record).catch(error =>
                reportAsyncFailure('debug configuration failed', error));
        } else if (message.event === 'terminated' || message.event === 'exited') {
            callHost('vscode.debug.stopSession', {
                ...record.owner, sessionId: record.id,
            }).catch(() => undefined);
        } else if (message.event === 'output') {
            debugEvents.console.fire({ session: record.session,
                output: message.body?.output || '', category: message.body?.category || 'console' });
        } else {
            debugEvents.custom.fire({ session: record.session,
                event: message.event, body: message.body });
        }
    }

    function handleDebugEvent(payload, existing) {
        const id = String(payload.sessionId || processEventId(payload));
        const record = existing || state.debugSessions.get(id);
        if (!record) {
            queueEarlyProcessEvent(payload);
            return false;
        }
        const op = String(payload.op || '');
        if (op === 'dap' && payload.message && typeof payload.message === 'object')
            handleDapMessage(record, payload.message);
        else if (op === 'inlineInput' && record.inline) {
            try { runWithActivation(record.scope,
                () => record.inline.handleMessage(payload.message)); }
            catch (error) { trackerCall(record, 'onError', error); }
        } else if (op === 'protocolError') {
            trackerCall(record, 'onError', new Error(payload.message || 'DAP protocol error'));
        } else if (op === 'output') {
            debugEvents.console.fire({ session: record.session,
                output: String(payload.output || ''),
                category: String(payload.category || 'console') });
        } else if (op === 'end') {
            callHost('vscode.debug.stopSession', {
                ...record.owner, sessionId: record.id,
            }).catch(() => undefined);
            finishDebugRecord(record, payload);
        }
        return true;
    }

    async function provideDebugConfigurations(type = '', folder) {
        const configurations = [];
        for (const record of Array.from(state.debugProviders.values())) {
            if (!ownerCurrent(record) || record.kind !== 'configuration' ||
                (type && record.type !== type)) continue;
            await record.ready;
            const result = await invokeDebugProvider({
                invoke: 'debug.provideConfigurations', key: record.key,
                extensionId: record.owner.extensionId,
                generation: record.owner.generation, folder: folderSnapshot(folder),
            });
            for (const value of result.configurations || []) {
                if (configurations.length >= maximumTasks) break;
                configurations.push(value);
            }
        }
        return configurations;
    }

    async function prepareDebugSession(folder, inputConfiguration) {
        const providerKey = inputConfiguration?._saoDebugProviderKey;
        const candidate = { ...(inputConfiguration || {}) };
        delete candidate._saoDebugProviderKey;
        let configuration = bounded(candidate, 'Debug configuration');
        const type = String(configuration.type || '');
        const providers = Array.from(state.debugProviders.values())
            .filter(record => ownerCurrent(record) && record.type === type);
        let descriptorScope = ownerCurrent(state.debugProviders.get(String(providerKey || '')))
            ? state.debugProviders.get(String(providerKey)).scope : undefined;
        for (const substituted of [false, true]) {
            for (const record of providers) {
                if (record.kind !== 'configuration') continue;
                await record.ready;
                const result = await invokeDebugProvider({
                    invoke: 'debug.resolveConfiguration', key: record.key,
                    extensionId: record.owner.extensionId,
                    generation: record.owner.generation,
                    folder: folderSnapshot(folder), configuration, substituted,
                });
                if (result.cancelled) return null;
                configuration = result.configuration || configuration;
            }
        }
        let descriptor = null;
        for (const record of providers) {
            if (record.kind !== 'adapterDescriptorFactory' &&
                record.kind !== 'adapterFactory') continue;
            await record.ready;
            const result = await invokeDebugProvider({
                invoke: 'debug.createDescriptor', key: record.key,
                extensionId: record.owner.extensionId,
                generation: record.owner.generation, configuration,
                executable: null,
            });
            descriptor = result.descriptor;
            if (descriptor) {
                descriptorScope = record.scope;
                break;
            }
        }
        if (!descriptor) {
            const command = configuration.debugAdapterExecutable ||
                configuration.adapterCommand;
            if (command) descriptor = { kind: 'executable', command,
                args: configuration.adapterArgs || [] };
        }
        if (!descriptor) throw new Error(`No debug adapter for type '${type}'`);
        return { configuration, descriptor, scope: descriptorScope, providerKey };
    }

    async function startDebugging(folder, configuration, parentSession, options = {}) {
        let callerScope;
        try { callerScope = requireActivationScope('startDebugging'); }
        catch (_) { /* workbench and reverse requests select a provider below */ }
        const prepared = await prepareDebugSession(folder, configuration);
        if (!prepared) return false;
        const scope = prepared.scope || callerScope || executionScope(
            'startDebugging', String(prepared.configuration?.type || ''),
            undefined, 'debug', prepared.providerKey);
        const owner = surfaceOwner(scope, 'debug-session');
        const id = owner.registrationId;
        const config = prepared.configuration || configuration;
        const descriptor = prepared.descriptor;
        const inlineEntry = descriptor?.kind === 'inline'
            ? state.debugInlineAdapters.get(descriptor.inlineId) : undefined;
        if (descriptor?.kind === 'inline') state.debugInlineAdapters.delete(descriptor.inlineId);
        if (descriptor?.kind === 'inline' &&
            (!inlineEntry || !activationScopeCurrent(inlineEntry.scope))) {
            throw new Error('Inline debug adapter generation is no longer active');
        }
        const record = { id, owner, scope, configuration: config,
            descriptor, folder, parentSession, active: true, initialized: false,
            nextSeq: 0, pending: new Map(), protocolBreakpoints: new Map(), trackers: [],
            inline: inlineEntry?.implementation,
            routeDispose: undefined, inlineSubscription: undefined };
        record.session = new DebugSession(record);
        for (const provider of state.debugProviders.values()) {
            if (!ownerCurrent(provider) || provider.kind !== 'trackerFactory' ||
                provider.type !== String(config.type || '')) continue;
            try {
                const tracker = await Promise.resolve(runWithActivation(provider.scope,
                    () => provider.provider.createDebugAdapterTracker(record.session)));
                if (!ownerCurrent(provider))
                    throw new Error('Debug tracker provider generation retired');
                if (tracker) record.trackers.push(tracker);
            } catch (error) { reportAsyncFailure('debug tracker creation failed', error); }
        }
        state.debugSessions.set(id, record);
        record.routeDispose = registerProcessRoute(id, payload => handleDebugEvent(payload, record));
        if (record.inline?.onDidSendMessage) {
            record.inlineSubscription = record.inline.onDidSendMessage(message =>
                handleDapMessage(record, message));
        }
        let nativeStarted = false;
        try {
            await callHost('vscode.debug.startSession', {
                ...owner, sessionId: id, configuration: config, descriptor,
                parentSessionId: parentSession?.id, options,
            });
            nativeStarted = true;
            if (!activationScopeCurrent(scope))
                throw new Error('Debug session generation retired during startup');
            trackerCall(record, 'onWillStartSession');
            state.activeDebugSessionId = id;
            debugEvents.start.fire(record.session);
            debugEvents.active.fire(record.session);
            await sendDapRequest(record, 'initialize', {
                clientID: 'sao-ai-editor', clientName: 'SAO AI Editor',
                adapterID: String(config.type || ''), locale: 'en',
                linesStartAt1: true, columnsStartAt1: true,
                pathFormat: 'path', supportsVariableType: true,
                supportsVariablePaging: true, supportsRunInTerminalRequest: true,
                supportsProgressReporting: true,
            }, launchTimeoutMs);
            const request = config.request === 'attach' ? 'attach' : 'launch';
            sendDapRequest(record, request, config, launchTimeoutMs).catch(error => {
                trackerCall(record, 'onError', error);
                stopDebugging(record.session).catch(() => undefined);
            });
            return true;
        } catch (error) {
            if (nativeStarted) {
                await callHost('vscode.debug.stopSession', {
                    ...owner, sessionId: id,
                }).catch(() => undefined);
            }
            finishDebugRecord(record, { exitCode: undefined });
            throw error;
        }
    }

    async function stopDebugging(session) {
        const record = session ? state.debugSessions.get(session.id) :
            state.debugSessions.get(state.activeDebugSessionId);
        if (!record) return false;
        if (record.stopPromise) return record.stopPromise;
        record.stopPromise = (async () => {
            try {
                await Promise.race([
                    sendDapRequest(record, 'disconnect', { restart: false,
                        terminateDebuggee: true }, 2000),
                    new Promise(resolve => setTimeout(resolve, 2100)),
                ]);
            } catch (_) { /* adapter may already be gone */ }
            await callHost('vscode.debug.stopSession', {
                ...record.owner, sessionId: record.id,
            }).catch(() => undefined);
            finishDebugRecord(record, { exitCode: undefined });
            return true;
        })();
        return record.stopPromise;
    }

    function addBreakpoints(values) {
        const added = [];
        for (const value of Array.isArray(values) ? values : []) {
            if (!value || !value.id || state.breakpoints.size >= maximumBreakpoints) continue;
            if (!state.breakpoints.has(value.id)) {
                state.breakpoints.set(value.id, value);
                added.push(value);
            }
        }
        if (added.length) {
            debugEvents.breakpoints.fire({ added, removed: [], changed: [] });
            refreshActiveBreakpoints();
        }
    }

    function removeBreakpoints(values) {
        const removed = [];
        for (const value of Array.isArray(values) ? values : []) {
            if (value?.id && state.breakpoints.delete(value.id)) removed.push(value);
        }
        if (removed.length) {
            debugEvents.breakpoints.fire({ added: [], removed, changed: [] });
            refreshActiveBreakpoints();
        }
    }

    const debug = {
        registerDebugConfigurationProvider(type, provider, triggerKind = 0) {
            return registerDebugProvider(type, provider, 'configuration', triggerKind);
        },
        registerDebugAdapterDescriptorFactory(type, factory) {
            return registerDebugProvider(type, factory, 'adapterDescriptorFactory');
        },
        registerDebugAdapterTrackerFactory(type, factory) {
            return registerDebugProvider(type, factory, 'trackerFactory');
        },
        startDebugging,
        stopDebugging,
        addBreakpoints,
        removeBreakpoints,
        get breakpoints() { return Array.from(state.breakpoints.values()); },
        get activeDebugSession() {
            return state.debugSessions.get(state.activeDebugSessionId)?.session;
        },
        get activeDebugConsole() {
            return {
                append(value) { debugEvents.console.fire({ output: String(value), category: 'console' }); },
                appendLine(value) { this.append(String(value) + '\n'); },
            };
        },
        onDidStartDebugSession: debugEvents.start.event,
        onDidTerminateDebugSession: debugEvents.terminate.event,
        onDidChangeActiveDebugSession: debugEvents.active.event,
        onDidReceiveDebugSessionCustomEvent: debugEvents.custom.event,
        onDidChangeBreakpoints: debugEvents.breakpoints.event,
    };

    function registerTerminalProfileProvider(id, provider) {
        const scope = requireActivationScope('registerTerminalProfileProvider');
        const key = String(id || '');
        if (!key || !provider || typeof provider.provideTerminalProfile !== 'function')
            throw new TypeError('TerminalProfileProvider requires an id and provider');
        if (state.terminalProfileProviders.size >= maximumTasks)
            throw new Error('Terminal profile provider limit reached');
        if (state.terminalProfileProviders.has(key))
            throw new Error(`Terminal profile provider already registered: ${key}`);
        const record = { id: key, provider, scope, active: true };
        state.terminalProfileProviders.set(key, record);
        return disposable(() => {
            record.active = false;
            if (state.terminalProfileProviders.get(key) === record)
                state.terminalProfileProviders.delete(key);
        });
    }

    async function initialize() {
        if (state.surfacesInitialized) return;
        state.surfacesInitialized = true;
        try { host.hydrateTerminals(await callHost('vscode.window.listTerminals', {})); }
        catch (error) { reportAsyncFailure('terminal hydration failed', error); }
    }

    function handleEvent(kind, payload) {
        if (kind === 'terminal') {
            if (!host.handleTerminalProcessEvent(payload)) queueEarlyProcessEvent(payload);
            return true;
        }
        if (kind === 'tasks') return handleTaskEvent(payload);
        if (kind === 'debug') return handleDebugEvent(payload);
        return false;
    }

    async function invoke(request) {
        if (!request || typeof request !== 'object')
            throw new Error('Extension callback request must be an object');
        if (String(request.invoke || '').startsWith('tasks.'))
            return invokeTaskProvider(request);
        if (String(request.invoke || '').startsWith('debug.'))
            return invokeDebugProvider(request);
        throw new Error('Unsupported extension callback');
    }

    return {
        tasks, debug, Task, TaskExecution, ProcessExecution, ShellExecution, CustomExecution,
        DebugSession, DebugAdapterExecutable, DebugAdapterServer,
        DebugAdapterNamedPipeServer, DebugAdapterInlineImplementation,
        SourceBreakpoint, FunctionBreakpoint, DataBreakpoint,
        TaskScope: { Global: 1, Workspace: 2 },
        TaskGroup: {
            Clean: Object.freeze({ id: 'clean', label: 'Clean' }),
            Build: Object.freeze({ id: 'build', label: 'Build' }),
            Rebuild: Object.freeze({ id: 'rebuild', label: 'Rebuild' }),
            Test: Object.freeze({ id: 'test', label: 'Test' }),
        },
        TaskRevealKind: { Always: 1, Silent: 2, Never: 3 },
        TaskPanelKind: { Shared: 1, Dedicated: 2, New: 3 },
        DebugConfigurationProviderTriggerKind: { Initial: 1, Dynamic: 2 },
        registerTerminalProfileProvider, provideDebugConfigurations,
        initialize, handleEvent, invoke,
    };
};
