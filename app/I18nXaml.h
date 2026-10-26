#pragma once
// Translation of XAML: the strings in XAML (Text / Content / AutomationProperties.Name / ToolTip / PlaceholderText / Header, menu items)
// are string ids; after loading, the logical tree is walked and lycan::Tr replaces them with the current language.
// Call once on the UI thread after the page is made (for UI made dynamically use Tr / Tf directly).
#include "pch.h"

namespace lycan_app {

void TranslateTree(winrt::Microsoft::UI::Xaml::DependencyObject const& root);

// Root element of a DataTemplate (call on Loaded): translates only the strings written into buttons / check boxes, not the text bound to data (file names etc.)
void TranslateTemplateRoot(winrt::Microsoft::UI::Xaml::DependencyObject const& root);

}  // namespace lycan_app
