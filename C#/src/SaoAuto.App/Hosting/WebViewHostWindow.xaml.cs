using System.Windows;

namespace SaoAuto.App.Hosting;

public partial class WebViewHostWindow : Window
{
    public WebViewHostWindow(string? runtimeStatus = null)
    {
        InitializeComponent();
        if (!string.IsNullOrEmpty(runtimeStatus))
        {
            StatusText.Text = runtimeStatus;
        }
    }
}
