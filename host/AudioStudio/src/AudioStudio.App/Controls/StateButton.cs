// SPDX-License-Identifier: MIT
using Avalonia.Controls.Primitives;

namespace AudioStudio.App.Controls;

/// <summary>
/// A toggle button that shows a state of the view model instead of keeping
/// its own: a click runs the command only, and <c>IsChecked</c> follows its
/// binding, so an action that fails leaves the button as the state is.
/// </summary>
public sealed class StateButton : ToggleButton
{
    protected override Type StyleKeyOverride => typeof(ToggleButton);

    protected override void Toggle()
    {
    }
}
