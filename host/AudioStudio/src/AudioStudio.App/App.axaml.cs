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
            // --image FILE.bin: the firmware image; --view audio|firmware;
            // --spike-filter on|off: the spike filter before the M4 detector; --mains 50|60: its mains frequency.
            string[] args = desktop.Args ?? [];
            desktop.MainWindow = new Views.MainWindow(new Views.StartOptions(
                Option(args, "--board"), args.Contains("--shell"), Option(args, "--open"), Option(args, "--model"),
                Option(args, "--image"), Option(args, "--view"), Option(args, "--target"),
                Option(args, "--spike-filter") switch { "on" => true, "off" => false, _ => null },
                Option(args, "--mains") switch { "50" => 50.0, "60" => 60.0, _ => null }));
        }

        base.OnFrameworkInitializationCompleted();
    }

    private static string? Option(string[] args, string name)
    {
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }
}
