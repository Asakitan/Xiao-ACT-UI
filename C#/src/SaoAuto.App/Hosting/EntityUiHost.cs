using System.Windows;
using SaoAuto.Core.Configuration;

namespace SaoAuto.App.Hosting;

public sealed class EntityUiHost : IUiHost
{
    public string ModeName => UiMode.Entity;

    public bool IsAvailable => true;

    public Window CreateMainWindow() => new EntityHostWindow();
}
