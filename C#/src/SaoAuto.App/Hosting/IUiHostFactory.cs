namespace SaoAuto.App.Hosting;

public interface IUiHostFactory
{
    IUiHost Create(string modeName);
}
