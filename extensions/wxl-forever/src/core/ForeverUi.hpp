#pragma once

// The one Forever window (core/ForeverUi.cpp): Overview, one tab per feature, Debug.
namespace wxl::forever::ui
{
    /// Registers the window. Called once at load, after the features are installed.
    void InstallWindow();
}
