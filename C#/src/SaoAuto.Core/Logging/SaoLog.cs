using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace SaoAuto.Core.Logging;

/// <summary>
/// Static <see cref="ILoggerFactory"/> facade. Defaults to <see cref="NullLoggerFactory"/>
/// so Core never takes a hard Serilog dependency. <c>SaoAuto.App</c> calls
/// <see cref="Configure"/> with a real factory at startup.
/// </summary>
public static class SaoLog
{
    private static ILoggerFactory _factory = NullLoggerFactory.Instance;

    public static ILoggerFactory Factory => _factory;

    public static void Configure(ILoggerFactory factory)
    {
        _factory = factory ?? throw new ArgumentNullException(nameof(factory));
    }

    public static ILogger For(string category) => _factory.CreateLogger(category);

    public static ILogger<T> For<T>() => _factory.CreateLogger<T>();
}
