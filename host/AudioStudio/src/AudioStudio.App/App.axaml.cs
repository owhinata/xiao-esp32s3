// SPDX-License-Identifier: MIT
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace AudioStudio.App;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            // --target NAME or FILE.json: the target profile; --board ADDRESS: connect the audio stream;
            // --shell: and the shell; --open FILE.wav: show a file; --model FILE.onnx or FILE.npz: load a model;
            // --image FILE.bin: the firmware image; --view audio|firmware.
            string[] args = desktop.Args ?? [];
            desktop.MainWindow = new Views.MainWindow(new Views.StartOptions(
                Option(args, "--board"), args.Contains("--shell"), Option(args, "--open"), Option(args, "--model"),
                Option(args, "--image"), Option(args, "--view"), Option(args, "--target")));
        }

        base.OnFrameworkInitializationCompleted();
    }

    private static string? Option(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }
}
