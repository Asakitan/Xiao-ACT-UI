// HelloPlugin.cs — C# 插件宿主生命周期 smoke
//
// hostfxr load_assembly_and_get_function_pointer 默认 signature:
//   public delegate int ComponentEntryPoint(IntPtr args, int sizeBytes);
// 所以每个入口就是 plain static int fn(IntPtr, int).
// (不用 UnmanagedCallersOnly, 那需要传 UNMANAGEDCALLERSONLY_METHOD 常量,
//  用默认 signature 更简洁 — hostfxr 内部会用 delegate marshaler wrap.)
//
// SDK 桥用 struct with function pointers 从 C++ 传入, C# 侧缓存后
// 通过 Marshal.GetDelegateForFunctionPointer 转成 delegate 调用.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace SaoAuto.Plugins.HelloCsharp
{
    // 逐字节匹配 C++ 侧 cs_sdk_bridge (顺序: log_info / register_ui_panel / register_hotkey).
    [StructLayout(LayoutKind.Sequential)]
    internal struct SdkBridge
    {
        public IntPtr LogInfo;
        public IntPtr RegisterUiPanel;
        public IntPtr RegisterHotkey;
    }

#if SAO_MANAGED_ENTITY_FIXTURE
    [StructLayout(LayoutKind.Sequential)]
    internal struct ManagedPluginContext
    {
        public uint StructSize;
        public uint AbiVersion;
        public IntPtr SdkContext;
        public IntPtr LoaderContext;
        public IntPtr SdkTable;
        public IntPtr SdkSession;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ManagedSdkTable
    {
        public uint StructSize;
        public uint AbiVersion;
        public IntPtr Dispatch;
        public IntPtr ReleaseCallback;
        public IntPtr Log;
        public IntPtr RegisterEngine;
        public IntPtr GetEngine;
        public IntPtr RegisterEntityProvider;
        public IntPtr UnregisterEntityProvider;
        public IntPtr LastError;
        public IntPtr RegisterEntityProviderV2;
        public IntPtr EmitContext;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ManagedCallbackDescriptor
    {
        public uint StructSize;
        public uint AbiVersion;
        public uint Kind;
        public uint Reserved;
        public IntPtr GcHandle;
        public IntPtr Invoke;
        public IntPtr Retain;
        public IntPtr Release;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ManagedEntityProviderDescriptorV2
    {
        public uint StructSize;
        public IntPtr ProviderId;
        public IntPtr ContributionId;
        public IntPtr RootId;
        public IntPtr Name;
        public IntPtr Icon;
        public double Priority;
        public IntPtr Snapshot;
        public IntPtr ActionHandler;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ManagedEntitySnapshotInvocationV2
    {
        public IntPtr Rows;
        public uint Capacity;
        public uint RowStrideBytes;
        public IntPtr OutCount;
        public IntPtr OutRevision;
        public IntPtr OutContentToken;
        public IntPtr OutRowStrideBytes;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct ManagedEntityMenuRowV2
    {
        public uint StructSize;
        public IntPtr CategoryId;
        public IntPtr CategoryLabel;
        public IntPtr CategoryIcon;
        public double CategoryPriority;
        public IntPtr RowLabel;
        public IntPtr RowIcon;
        public IntPtr ActionId;
        public IntPtr PayloadJson;
        public byte CanActivate;
        public byte KeepMenuOpen;
        public byte CloseMenuBefore;
        public fixed byte Reserved[5];
    }
#endif

    public static unsafe class HelloPlugin
    {
#if SAO_MANAGED_ENTITY_FIXTURE
        private const int SaoOk = 0;
        private const int SaoInvalidArgument = -1;
        private const int SaoBufferTooSmall = -4;
        private const uint ManagedAbiVersion = 1;
        private const uint EntitySnapshotKind = 4;
        private const uint EntityActionKind = 5;
        private const uint ManagedRowPrefixSize = 80;
        private const uint PhysicalRowStride = 88;
        private const string ReentrantUnloadTopic = "csharp_entity_reentrant_unload";

        private enum SnapshotMode
        {
            Stable,
            InvalidUtf8,
            InvalidJson,
            InvalidFlags,
            InvalidReserved,
            OverBudget,
            ZeroToken,
            CountMismatch,
            RevisionMismatch,
            TokenMismatch,
            StrideMismatch,
        }
#endif

        private static SdkBridge s_bridge;
        private static int s_tickCount;
        private static int s_totalTicks;
#if SAO_MANAGED_ENTITY_FIXTURE
        private static ManagedEntityProvider s_entityProvider;
#endif

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void LogInfoDelegate(IntPtr utf8);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void RegisterUiPanelDelegate(IntPtr utf8);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void RegisterHotkeyDelegate(IntPtr id, IntPtr key);
    #if SAO_MANAGED_ENTITY_FIXTURE
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int RegisterEntityProviderV2Delegate(IntPtr session, IntPtr descriptor);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int UnregisterEntityProviderDelegate(IntPtr session, IntPtr providerId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int EmitContextDelegate(IntPtr session, IntPtr topic, IntPtr payload);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int ManagedInvokeDelegate(IntPtr gcHandle, uint kind, IntPtr invocation);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate int ManagedLifetimeDelegate(IntPtr gcHandle);

        private static readonly ManagedInvokeDelegate s_managedInvoke = InvokeManaged;
        private static readonly ManagedLifetimeDelegate s_managedRetain = RetainManaged;
        private static readonly ManagedLifetimeDelegate s_managedRelease = ReleaseManaged;

        private sealed class ManagedEntityProvider : IDisposable
        {
            private readonly IntPtr _providerId;
            private readonly IntPtr _contributionId;
            private readonly IntPtr _rootId;
            private readonly IntPtr _name;
            private readonly IntPtr _icon;
            private readonly IntPtr _categoryId;
            private readonly IntPtr _categoryLabel;
            private readonly IntPtr _categoryIcon;
            private readonly IntPtr _rowIcon;
            private readonly IntPtr _actionId;
            private readonly IntPtr _invalidUtf8;
            private readonly IntPtr _invalidJson;
            private readonly IntPtr _overBudget;
            private readonly ManagedSdkTable _sdkTable;
            private readonly IntPtr _sdkSession;
            private GCHandle _selfHandle;
            private int _nativeReferences;
            private bool _disposed;
            private SnapshotMode _mode;
            private ulong _revision = 1;
            private ulong _producerToken = 0xD160001UL;
            private int _actionCount;
            private IntPtr _snapshotRowLabel;
            private IntPtr _snapshotPayload;

            public ManagedEntityProvider(ManagedSdkTable sdkTable, IntPtr sdkSession)
            {
                _sdkTable = sdkTable;
                _sdkSession = sdkSession;
                try
                {
                    _providerId = Marshal.StringToCoTaskMemUTF8("managed-menu");
                    _contributionId = Marshal.StringToCoTaskMemUTF8("managed-root");
                    _rootId = Marshal.StringToCoTaskMemUTF8("plugin:csharp-managed");
                    _name = Marshal.StringToCoTaskMemUTF8("Managed Fixture");
                    _icon = Marshal.StringToCoTaskMemUTF8("CS");
                    _categoryId = Marshal.StringToCoTaskMemUTF8("managed-tools");
                    _categoryLabel = Marshal.StringToCoTaskMemUTF8("Managed Tools");
                    _categoryIcon = Marshal.StringToCoTaskMemUTF8("CS");
                    _rowIcon = Marshal.StringToCoTaskMemUTF8("managed-row");
                    _actionId = Marshal.StringToCoTaskMemUTF8("managed-action");
                    _invalidUtf8 = Marshal.AllocCoTaskMem(3);
                    Marshal.WriteByte(_invalidUtf8, 0, 0xC3);
                    Marshal.WriteByte(_invalidUtf8, 1, 0x28);
                    Marshal.WriteByte(_invalidUtf8, 2, 0x00);
                    _invalidJson = Marshal.StringToCoTaskMemUTF8("{invalid-json");
                    _overBudget = Marshal.StringToCoTaskMemUTF8(new string('B', 16385));
                    _selfHandle = GCHandle.Alloc(this, GCHandleType.Normal);
                }
                catch
                {
                    ReleaseOwnedMemory();
                    throw;
                }
            }

            public IntPtr Handle => GCHandle.ToIntPtr(_selfHandle);

            public int Register()
            {
                if (_sdkTable.RegisterEntityProviderV2 == IntPtr.Zero) return SaoInvalidArgument;
                ManagedCallbackDescriptor snapshot = CallbackDescriptor(EntitySnapshotKind);
                ManagedCallbackDescriptor action = CallbackDescriptor(EntityActionKind);
                IntPtr snapshotPointer = IntPtr.Zero;
                IntPtr actionPointer = IntPtr.Zero;
                IntPtr descriptorPointer = IntPtr.Zero;
                try
                {
                    snapshotPointer =
                        Marshal.AllocHGlobal(Marshal.SizeOf<ManagedCallbackDescriptor>());
                    actionPointer =
                        Marshal.AllocHGlobal(Marshal.SizeOf<ManagedCallbackDescriptor>());
                    descriptorPointer =
                        Marshal.AllocHGlobal(Marshal.SizeOf<ManagedEntityProviderDescriptorV2>());
                    Marshal.StructureToPtr(snapshot, snapshotPointer, false);
                    Marshal.StructureToPtr(action, actionPointer, false);
                    ManagedEntityProviderDescriptorV2 descriptor = new ManagedEntityProviderDescriptorV2
                    {
                        StructSize = (uint)Marshal.SizeOf<ManagedEntityProviderDescriptorV2>(),
                        ProviderId = _providerId,
                        ContributionId = _contributionId,
                        RootId = _rootId,
                        Name = _name,
                        Icon = _icon,
                        Priority = 16.0,
                        Snapshot = snapshotPointer,
                        ActionHandler = actionPointer,
                    };
                    Marshal.StructureToPtr(descriptor, descriptorPointer, false);
                    RegisterEntityProviderV2Delegate register =
                        Marshal.GetDelegateForFunctionPointer<RegisterEntityProviderV2Delegate>(
                            _sdkTable.RegisterEntityProviderV2);
                    return register(_sdkSession, descriptorPointer);
                }
                finally
                {
                    if (descriptorPointer != IntPtr.Zero) Marshal.FreeHGlobal(descriptorPointer);
                    if (actionPointer != IntPtr.Zero) Marshal.FreeHGlobal(actionPointer);
                    if (snapshotPointer != IntPtr.Zero) Marshal.FreeHGlobal(snapshotPointer);
                }
            }

            public int Unregister()
            {
                if (_disposed || _sdkTable.UnregisterEntityProvider == IntPtr.Zero)
                    return SaoInvalidArgument;
                UnregisterEntityProviderDelegate unregister =
                    Marshal.GetDelegateForFunctionPointer<UnregisterEntityProviderDelegate>(
                        _sdkTable.UnregisterEntityProvider);
                return unregister(_sdkSession, _providerId);
            }

            public int Retain()
            {
                if (_disposed) return SaoInvalidArgument;
                _nativeReferences++;
                return SaoOk;
            }

            public int Release()
            {
                if (_disposed || _nativeReferences <= 0) return SaoInvalidArgument;
                _nativeReferences--;
                if (_nativeReferences == 0)
                {
                    Dispose();
                    s_entityProvider = null;
                }
                return SaoOk;
            }

            public int Invoke(uint kind, IntPtr invocation)
            {
                if (_disposed || invocation == IntPtr.Zero) return SaoInvalidArgument;
                if (kind == EntitySnapshotKind) return Snapshot(invocation);
                if (kind == EntityActionKind) return Action(invocation);
                return SaoInvalidArgument;
            }

            private ManagedCallbackDescriptor CallbackDescriptor(uint kind)
            {
                return new ManagedCallbackDescriptor
                {
                    StructSize = (uint)Marshal.SizeOf<ManagedCallbackDescriptor>(),
                    AbiVersion = ManagedAbiVersion,
                    Kind = kind,
                    Reserved = 0,
                    GcHandle = Handle,
                    Invoke = Marshal.GetFunctionPointerForDelegate(s_managedInvoke),
                    Retain = Marshal.GetFunctionPointerForDelegate(s_managedRetain),
                    Release = Marshal.GetFunctionPointerForDelegate(s_managedRelease),
                };
            }

            private int Snapshot(IntPtr invocationPointer)
            {
                ManagedEntitySnapshotInvocationV2 invocation =
                    Marshal.PtrToStructure<ManagedEntitySnapshotInvocationV2>(invocationPointer);
                if (invocation.OutCount == IntPtr.Zero || invocation.OutRevision == IntPtr.Zero ||
                    invocation.OutContentToken == IntPtr.Zero ||
                    invocation.OutRowStrideBytes == IntPtr.Zero)
                {
                    return SaoInvalidArgument;
                }

                bool probe = invocation.Rows == IntPtr.Zero && invocation.Capacity == 0 &&
                             invocation.RowStrideBytes == 0;
                if (probe) PrepareSnapshotStrings();
                ulong token = _mode == SnapshotMode.ZeroToken ? 0 : _producerToken;
                Marshal.WriteInt32(invocation.OutCount, 1);
                Marshal.WriteInt64(invocation.OutRevision, unchecked((long)_revision));
                Marshal.WriteInt64(invocation.OutContentToken, unchecked((long)token));
                Marshal.WriteInt32(invocation.OutRowStrideBytes, unchecked((int)PhysicalRowStride));
                if (probe) return SaoBufferTooSmall;
                if (invocation.Rows == IntPtr.Zero || invocation.Capacity < 1 ||
                    invocation.RowStrideBytes < PhysicalRowStride)
                {
                    return SaoBufferTooSmall;
                }

                if (_mode == SnapshotMode.TokenMismatch)
                {
                    Marshal.WriteInt64(invocation.OutContentToken,
                                       unchecked((long)(_producerToken + 1)));
                }
                if (_mode == SnapshotMode.CountMismatch)
                    Marshal.WriteInt32(invocation.OutCount, 2);
                if (_mode == SnapshotMode.RevisionMismatch)
                {
                    Marshal.WriteInt64(invocation.OutRevision,
                                       unchecked((long)(_revision + 1)));
                }
                if (_mode == SnapshotMode.StrideMismatch)
                {
                    Marshal.WriteInt32(invocation.OutRowStrideBytes,
                                       unchecked((int)(PhysicalRowStride + 8)));
                }

                if (_snapshotRowLabel == IntPtr.Zero || _snapshotPayload == IntPtr.Zero)
                    return SaoInvalidArgument;
                ManagedEntityMenuRowV2 row = new ManagedEntityMenuRowV2
                {
                    StructSize = PhysicalRowStride,
                    CategoryId = _categoryId,
                    CategoryLabel = _categoryLabel,
                    CategoryIcon = _categoryIcon,
                    CategoryPriority = 16.0,
                    RowLabel = _snapshotRowLabel,
                    RowIcon = _rowIcon,
                    ActionId = _actionId,
                    PayloadJson = _snapshotPayload,
                    CanActivate = 1,
                    KeepMenuOpen = 0,
                    CloseMenuBefore = 0,
                };
                if (_mode == SnapshotMode.InvalidUtf8) row.RowLabel = _invalidUtf8;
                if (_mode == SnapshotMode.InvalidJson) row.PayloadJson = _invalidJson;
                if (_mode == SnapshotMode.InvalidFlags) row.CanActivate = 2;
                if (_mode == SnapshotMode.InvalidReserved) row.Reserved[0] = 1;
                if (_mode == SnapshotMode.OverBudget) row.RowLabel = _overBudget;
                Marshal.StructureToPtr(row, invocation.Rows, false);
                if (invocation.RowStrideBytes >= PhysicalRowStride)
                {
                    Marshal.WriteInt64(invocation.Rows, unchecked((int)ManagedRowPrefixSize),
                                       unchecked((long)0xD16F007UL));
                }
                return SaoOk;
            }

            private void PrepareSnapshotStrings()
            {
                string rowLabel = _actionCount == 0 ? "Managed Fixture" :
                    "Managed Fixture " + _actionCount.ToString();
                string payload = "{\"action_count\":" + _actionCount.ToString() + "}";
                IntPtr nextRowLabel = Marshal.StringToCoTaskMemUTF8(rowLabel);
                IntPtr nextPayload = IntPtr.Zero;
                try
                {
                    nextPayload = Marshal.StringToCoTaskMemUTF8(payload);
                }
                catch
                {
                    Marshal.FreeCoTaskMem(nextRowLabel);
                    throw;
                }
                IntPtr previousRowLabel = _snapshotRowLabel;
                IntPtr previousPayload = _snapshotPayload;
                _snapshotRowLabel = nextRowLabel;
                _snapshotPayload = nextPayload;
                if (previousPayload != IntPtr.Zero) Marshal.FreeCoTaskMem(previousPayload);
                if (previousRowLabel != IntPtr.Zero) Marshal.FreeCoTaskMem(previousRowLabel);
            }

            private int Action(IntPtr invocationPointer)
            {
                IntPtr actionPointer = Marshal.ReadIntPtr(invocationPointer, 0);
                IntPtr payloadPointer = Marshal.ReadIntPtr(invocationPointer, IntPtr.Size);
                string action = Marshal.PtrToStringUTF8(actionPointer) ?? string.Empty;
                string payload = Marshal.PtrToStringUTF8(payloadPointer) ?? string.Empty;
                if (action == "fixture:set-stable") _mode = SnapshotMode.Stable;
                else if (action == "fixture:set-invalid-utf8") _mode = SnapshotMode.InvalidUtf8;
                else if (action == "fixture:set-invalid-json") _mode = SnapshotMode.InvalidJson;
                else if (action == "fixture:set-invalid-flags") _mode = SnapshotMode.InvalidFlags;
                else if (action == "fixture:set-invalid-reserved") _mode = SnapshotMode.InvalidReserved;
                else if (action == "fixture:set-over-budget") _mode = SnapshotMode.OverBudget;
                else if (action == "fixture:set-zero-token") _mode = SnapshotMode.ZeroToken;
                else if (action == "fixture:set-count-mismatch") _mode = SnapshotMode.CountMismatch;
                else if (action == "fixture:set-revision-mismatch") _mode = SnapshotMode.RevisionMismatch;
                else if (action == "fixture:set-token-mismatch") _mode = SnapshotMode.TokenMismatch;
                else if (action == "fixture:set-stride-mismatch") _mode = SnapshotMode.StrideMismatch;
                else if (action == "fixture:bump-producer-token") _producerToken++;
                else if (action == "managed-action")
                {
                    if (payload.Length == 0) return SaoInvalidArgument;
                    _actionCount++;
                    _revision++;
                    return EmitReentrantUnload();
                }
                else return SaoInvalidArgument;
                return SaoOk;
            }

            private int EmitReentrantUnload()
            {
                if (_sdkTable.EmitContext == IntPtr.Zero) return SaoInvalidArgument;
                EmitContextDelegate emit = Marshal.GetDelegateForFunctionPointer<EmitContextDelegate>(
                    _sdkTable.EmitContext);
                IntPtr topic = Marshal.StringToCoTaskMemUTF8(ReentrantUnloadTopic);
                IntPtr payload = IntPtr.Zero;
                try
                {
                    payload = Marshal.StringToCoTaskMemUTF8("{}");
                    return emit(_sdkSession, topic, payload);
                }
                finally
                {
                    if (payload != IntPtr.Zero) Marshal.FreeCoTaskMem(payload);
                    Marshal.FreeCoTaskMem(topic);
                }
            }

            public void Dispose()
            {
                if (_disposed) return;
                _disposed = true;
                ReleaseOwnedMemory();
            }

            private void ReleaseOwnedMemory()
            {
                if (_snapshotPayload != IntPtr.Zero) Marshal.FreeCoTaskMem(_snapshotPayload);
                if (_snapshotRowLabel != IntPtr.Zero) Marshal.FreeCoTaskMem(_snapshotRowLabel);
                if (_overBudget != IntPtr.Zero) Marshal.FreeCoTaskMem(_overBudget);
                if (_invalidJson != IntPtr.Zero) Marshal.FreeCoTaskMem(_invalidJson);
                if (_invalidUtf8 != IntPtr.Zero) Marshal.FreeCoTaskMem(_invalidUtf8);
                if (_actionId != IntPtr.Zero) Marshal.FreeCoTaskMem(_actionId);
                if (_rowIcon != IntPtr.Zero) Marshal.FreeCoTaskMem(_rowIcon);
                if (_categoryIcon != IntPtr.Zero) Marshal.FreeCoTaskMem(_categoryIcon);
                if (_categoryLabel != IntPtr.Zero) Marshal.FreeCoTaskMem(_categoryLabel);
                if (_categoryId != IntPtr.Zero) Marshal.FreeCoTaskMem(_categoryId);
                if (_icon != IntPtr.Zero) Marshal.FreeCoTaskMem(_icon);
                if (_name != IntPtr.Zero) Marshal.FreeCoTaskMem(_name);
                if (_rootId != IntPtr.Zero) Marshal.FreeCoTaskMem(_rootId);
                if (_contributionId != IntPtr.Zero) Marshal.FreeCoTaskMem(_contributionId);
                if (_providerId != IntPtr.Zero) Marshal.FreeCoTaskMem(_providerId);
                if (_selfHandle.IsAllocated) _selfHandle.Free();
            }
        }

        private static ManagedEntityProvider ProviderFromHandle(IntPtr handle)
        {
            if (handle == IntPtr.Zero) return null;
            GCHandle gcHandle = GCHandle.FromIntPtr(handle);
            return gcHandle.Target as ManagedEntityProvider;
        }

        private static int InvokeManaged(IntPtr handle, uint kind, IntPtr invocation)
        {
            try
            {
                ManagedEntityProvider provider = ProviderFromHandle(handle);
                return provider == null ? SaoInvalidArgument : provider.Invoke(kind, invocation);
            }
            catch { return SaoInvalidArgument; }
        }

        private static int RetainManaged(IntPtr handle)
        {
            try
            {
                ManagedEntityProvider provider = ProviderFromHandle(handle);
                return provider == null ? SaoInvalidArgument : provider.Retain();
            }
            catch { return SaoInvalidArgument; }
        }

        private static int ReleaseManaged(IntPtr handle)
        {
            try
            {
                ManagedEntityProvider provider = ProviderFromHandle(handle);
                return provider == null ? SaoInvalidArgument : provider.Release();
            }
            catch { return SaoInvalidArgument; }
        }
#endif

        private static void CallLog(string msg)
        {
            if (s_bridge.LogInfo == IntPtr.Zero) return;
            var d = Marshal.GetDelegateForFunctionPointer<LogInfoDelegate>(s_bridge.LogInfo);
            IntPtr p = Marshal.StringToCoTaskMemUTF8(msg);
            try { d(p); } finally { Marshal.FreeCoTaskMem(p); }
        }

        private static void CallRegisterUiPanel(string id)
        {
            if (s_bridge.RegisterUiPanel == IntPtr.Zero) return;
            var d = Marshal.GetDelegateForFunctionPointer<RegisterUiPanelDelegate>(s_bridge.RegisterUiPanel);
            IntPtr p = Marshal.StringToCoTaskMemUTF8(id);
            try { d(p); } finally { Marshal.FreeCoTaskMem(p); }
        }

        private static void CallRegisterHotkey(string id, string key)
        {
            if (s_bridge.RegisterHotkey == IntPtr.Zero) return;
            var d = Marshal.GetDelegateForFunctionPointer<RegisterHotkeyDelegate>(s_bridge.RegisterHotkey);
            IntPtr p1 = Marshal.StringToCoTaskMemUTF8(id);
            IntPtr p2 = Marshal.StringToCoTaskMemUTF8(key);
            try { d(p1, p2); }
            finally { Marshal.FreeCoTaskMem(p1); Marshal.FreeCoTaskMem(p2); }
        }

        // ── 入口 (hostfxr 默认 ComponentEntryPoint 签名) ──
        //
        // C++ 侧签名: int32_t fn(void* arg, int32_t arg_size)
        // 我们对应用 IntPtr + int32.

        public static int InitSdkPointers(IntPtr bridgePtr, int bridgeSize)
        {
            try
            {
                if (bridgePtr == IntPtr.Zero) return 1;
                if (bridgeSize < sizeof(IntPtr) * 3) return 2;
                s_bridge = Marshal.PtrToStructure<SdkBridge>(bridgePtr);
                return 0;
            }
            catch { return 99; }
        }

        public static int OnLoad(IntPtr contextPointer, int contextSize)
        {
            try
            {
                CallLog("hello_csharp: OnLoad fired");
                CallRegisterUiPanel("C# Hello");
                CallRegisterHotkey("greet_hotkey", "F10");
#if SAO_MANAGED_ENTITY_FIXTURE
                if (contextPointer != IntPtr.Zero &&
                    contextSize >= Marshal.SizeOf<ManagedPluginContext>())
                {
                    ManagedPluginContext context =
                        Marshal.PtrToStructure<ManagedPluginContext>(contextPointer);
                    if (context.StructSize >= Marshal.SizeOf<ManagedPluginContext>() &&
                        context.AbiVersion == ManagedAbiVersion &&
                        context.SdkTable != IntPtr.Zero && context.SdkSession != IntPtr.Zero)
                    {
                        uint sdkTableSize = unchecked((uint)Marshal.ReadInt32(context.SdkTable, 0));
                        uint sdkTableAbi = unchecked((uint)Marshal.ReadInt32(context.SdkTable, 4));
                        if (sdkTableSize >= Marshal.SizeOf<ManagedSdkTable>() &&
                            sdkTableAbi == ManagedAbiVersion)
                        {
                            ManagedSdkTable sdkTable =
                                Marshal.PtrToStructure<ManagedSdkTable>(context.SdkTable);
                            if (sdkTable.RegisterEntityProviderV2 != IntPtr.Zero &&
                                sdkTable.EmitContext != IntPtr.Zero)
                            {
                                ManagedEntityProvider provider = null;
                                try
                                {
                                    provider =
                                        new ManagedEntityProvider(sdkTable, context.SdkSession);
                                    int registerStatus = provider.Register();
                                    if (registerStatus != SaoOk) return registerStatus;
                                    s_entityProvider = provider;
                                    provider = null;
                                }
                                finally { provider?.Dispose(); }
                            }
                        }
                    }
                }
#endif
                CallLog("hello_csharp: registered UI + hotkey");
                return 0;
            }
            catch { return 99; }
        }

        public static int OnTick(IntPtr _unused, int _unusedSize)
        {
            try
            {
                s_tickCount++;
                s_totalTicks = s_tickCount;
                if (s_tickCount == 1) CallLog("hello_csharp: first tick");
                return 0;
            }
            catch { return 99; }
        }

#if SAO_MANAGED_ENTITY_FIXTURE
        public static int OnDisable(IntPtr _unused, int _unusedSize)
        {
            try
            {
                ManagedEntityProvider provider = s_entityProvider;
                return provider == null ? SaoOk : provider.Unregister();
            }
            catch { return 99; }
        }
#endif

        public static int OnUnload(IntPtr _unused, int _unusedSize)
        {
            try { CallLog("hello_csharp: OnUnload fired"); return 0; }
            catch { return 99; }
        }

        public static int GetTickCount(IntPtr _unused, int _unusedSize)
        {
            return s_tickCount;
        }
    }
}
