using System.IO;
using Microsoft.Extensions.Logging;
using SaoAuto.Core.Logging;
using Serilog;
using Serilog.Events;
using Serilog.Extensions.Logging;

namespace SaoAuto.App.Startup;

/// <summary>
/// Wires Serilog into <see cref="SaoLog"/>. Matches the Python log category names
/// (startup/packet/vision/overlay/updater/automation) by routing through the
/// shared <see cref="ILoggerFactory"/>.
/// </summary>
public static class LoggingBootstrap
{
    public static void Configure(string? logDirectory = null)
    {
        var directory = logDirectory ?? Path.Combine(AppContext.BaseDirectory, "logs");
        Directory.CreateDirectory(directory);

        Log.Logger = new LoggerConfiguration()
            .MinimumLevel.Information()
            .MinimumLevel.Override("Microsoft", LogEventLevel.Warning)
            .Enrich.FromLogContext()
            .WriteTo.Console()
            .WriteTo.File(
                Path.Combine(directory, "sao-auto-.log"),
                rollingInterval: RollingInterval.Day,
                retainedFileCountLimit: 14,
                shared: true)
            .CreateLogger();

        var factory = new SerilogLoggerFactory(Log.Logger, dispose: true);
        SaoLog.Configure(factory);
    }
}
