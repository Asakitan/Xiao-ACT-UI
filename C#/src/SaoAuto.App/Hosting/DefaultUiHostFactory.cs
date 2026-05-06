using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Hosting;

public sealed class DefaultUiHostFactory : IUiHostFactory
{
    public IUiHost Create(string modeName) => modeName switch
    {
        UiMode.Entity => new EntityUiHost(),
        UiMode.WebView => new WebViewUiHost(),
        _ => throw new ArgumentException($"Unknown UI mode '{modeName}'", nameof(modeName)),
    };
}
