using System.Windows;

namespace SaoAuto.App;

/// <summary>
/// WPF <see cref="Application"/>. Startup hooks (DPI, priority, logging,
/// apply-on-exit) live in <see cref="Program.Main"/> so they fire for every
/// run mode, not just <c>RunMode.Ui</c>.
/// </summary>
public partial class App : Application
{
}
