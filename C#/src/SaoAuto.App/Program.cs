using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging;
using SaoAuto.App.Cli;
using SaoAuto.App.Modes;
using SaoAuto.App.Startup;
using SaoAuto.App.Updater;
using SaoAuto.Core.Configuration;
using SaoAuto.Core.Logging;
using SaoAuto.Core.State;

namespace SaoAuto.App;

public static class Program
{
    private const int ExitOk = 0;
    private const int ExitFatal = 1;
    private const int ExitCancelled = 130; // Ctrl+C convention

    [STAThread]
    public static int Main(string[] args)
    {
        EarlyProcessSetup.Run();
        // OutputEncoding must be set before LoggingBootstrap or any Console.Write —
        // reassigning it after Console.Out is captured by Serilog's sink truncates
        // subsequent writes (observed during Session 4 smoke testing).
        TryEnableUtf8Console();
        LoggingBootstrap.Configure();
        ApplyOnExitHook.Register();
        var log = SaoLog.For("startup");

        var options = CliOptions.Parse(args);
        log.LogInformation("SaoAuto {Version} starting (mode={Mode}, settings={Settings})",
            AppVersion.Label, options.RunMode, options.SettingsPath ?? "<default>");

        if (options.RunMode == RunMode.Help)
        {
            EnsureConsole();
            Console.WriteLine(CliOptions.HelpText);
            return ExitOk;
        }

        if (options.UnknownArg is not null)
        {
            EnsureConsole();
            // Write to stdout (not stderr) — WinExe stderr handle is unreliable when
            // the subsystem is WINDOWS and the parent pipes only stdout. Returning
            // a non-zero exit code is the canonical signal for callers/CI.
            Console.WriteLine($"Unknown argument: {options.UnknownArg}");
            Console.WriteLine(CliOptions.HelpText);
            log.LogError("Unknown CLI argument: {Arg}", options.UnknownArg);
            return ExitFatal;
        }

        using var cts = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            log.LogInformation("Ctrl+C received; signalling cancellation");
            cts.Cancel();
        };
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex)
            {
                log.LogError(ex, "AppDomain unhandled exception (terminating={Terminating})", e.IsTerminating);
            }
        };

        try
        {
            var settings = BuildSettingsManager(options);
            var states = new GameStateManager(SaoLog.For("state"));

            return options.RunMode switch
            {
                RunMode.Test =>
                    RunWithConsole(() => new TestRunner(settings, states).RunAsync(cts.Token).GetAwaiter().GetResult()),
                RunMode.Headless =>
                    RunWithConsole(() => new HeadlessRunner(settings, states).RunAsync(cts.Token).GetAwaiter().GetResult()),
                _ => new UiRunner(settings, states).Run(cts.Token),
            };
        }
        catch (OperationCanceledException)
        {
            log.LogInformation("cancelled");
            return ExitCancelled;
        }
        catch (Exception ex)
        {
            log.LogCritical(ex, "fatal");
            return ExitFatal;
        }
    }

    private static SettingsManager BuildSettingsManager(CliOptions options)
    {
        var resolver = ResourcePathResolver.ForCurrentProcess();
        var path = options.SettingsPath ?? resolver.Settings;
        return new SettingsManager(path, SaoLog.For("settings"));
    }

    private static int RunWithConsole(Func<int> body)
    {
        EnsureConsole();
        return body();
    }

    /// <summary>
    /// WinExe builds inherit no console. For <c>--test</c>/<c>--headless</c> we call
    /// <c>AttachConsole(ATTACH_PARENT_PROCESS)</c> so output reaches the terminal that
    /// launched the process. No-op when a console is already attached (e.g. dotnet run).
    /// </summary>
    private static void EnsureConsole()
    {
        try
        {
            if (NativeMethods.GetConsoleWindow() == IntPtr.Zero)
            {
                NativeMethods.AttachConsole(NativeMethods.AttachParentProcess);
            }
        }
        catch
        {
            // best effort — Console.Write still works against the in-process buffer
        }
    }

    private static void TryEnableUtf8Console()
    {
        try
        {
            // CJK fixture text (咲 / 神盾骑士) round-trips only if the active console
            // code page is UTF-8. Default Windows Chinese installs are CP936; the
            // managed setter alone is not enough because the host console code page
            // sticks unless we also call SetConsoleOutputCP(65001). We additionally
            // rebuild Console.Out as a UTF-8 StreamWriter on the underlying stdout
            // stream — the .NET 8 default writer for redirected stdout wraps a
            // null-codepage encoding that mojibakes CJK input.
            NativeMethods.SetConsoleOutputCP(NativeMethods.CodePageUtf8);
            Console.OutputEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
            var stdout = Console.OpenStandardOutput();
            var utf8Writer = new StreamWriter(stdout, new System.Text.UTF8Encoding(false))
            {
                AutoFlush = true,
            };
            Console.SetOut(utf8Writer);
        }
        catch
        {
            // ignore — Console may not be attached yet on WinExe + no parent
        }
    }

    private static class NativeMethods
    {
        internal const int AttachParentProcess = -1;
        internal const uint CodePageUtf8 = 65001;

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool AttachConsole(int processId);

        [DllImport("kernel32.dll")]
        internal static extern IntPtr GetConsoleWindow();

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetConsoleOutputCP(uint wCodePageID);
    }
}
