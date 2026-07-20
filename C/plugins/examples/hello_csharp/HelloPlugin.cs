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

    public static unsafe class HelloPlugin
    {
        private static SdkBridge s_bridge;
        private static int s_tickCount;
        private static int s_totalTicks;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void LogInfoDelegate(IntPtr utf8);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void RegisterUiPanelDelegate(IntPtr utf8);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void RegisterHotkeyDelegate(IntPtr id, IntPtr key);

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

        public static int OnLoad(IntPtr _unused, int _unusedSize)
        {
            try
            {
                CallLog("hello_csharp: OnLoad fired");
                CallRegisterUiPanel("C# Hello");
                CallRegisterHotkey("greet_hotkey", "F10");
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
