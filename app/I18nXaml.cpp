#include "pch.h"
#include "I18nXaml.h"

#include "lycan/i18n.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Automation;

namespace lycan_app {
namespace {

// If the current value is a string that has a translation, replace it; if there is no string (or it is not a string) do nothing
bool Translated(hstring const& cur, hstring& out) {
    if (cur.empty()) return false;
    const wchar_t* t = lycan::Tr(cur.c_str());
    if (std::wstring_view(t) == std::wstring_view(cur)) return false;
    out = hstring{t};
    return true;
}

// Properties of type IInspectable such as Content / Header / ToolTip: only strings are handled
bool TranslatedObj(Windows::Foundation::IInspectable const& v, hstring& out) {
    if (!v) return false;
    auto s = v.try_as<Windows::Foundation::IReference<hstring>>();
    return s && Translated(s.Value(), out);
}

// Template mode: a template has strings bound to data (file names, status text); they must not be translated, or a file in an archive named "Folder" would be shown with another name.
// So only the strings written into buttons / check boxes are translated.
struct Ctx {
    bool templateMode = false;
    bool inButton = false;
};

void VisitFlyout(Controls::Primitives::FlyoutBase const& fb, Ctx ctx);
void Visit(DependencyObject const& d, Ctx ctx);

void VisitMenuItems(Windows::Foundation::Collections::IVector<MenuFlyoutItemBase> const& items) {
    for (auto const& it : items) {
        hstring t;
        if (auto name = AutomationProperties::GetName(it); Translated(name, t)) AutomationProperties::SetName(it, t);
        if (TranslatedObj(Controls::ToolTipService::GetToolTip(it), t)) Controls::ToolTipService::SetToolTip(it, box_value(t));
        if (auto sub = it.try_as<MenuFlyoutSubItem>()) {
            if (Translated(sub.Text(), t)) sub.Text(t);
            VisitMenuItems(sub.Items());
        } else if (auto mi = it.try_as<MenuFlyoutItem>()) {  // includes ToggleMenuFlyoutItem and RadioMenuFlyoutItem
            if (Translated(mi.Text(), t)) mi.Text(t);
        }
    }
}

void VisitFlyout(Controls::Primitives::FlyoutBase const& fb, Ctx ctx) {
    if (!fb) return;
    if (auto mf = fb.try_as<MenuFlyout>()) VisitMenuItems(mf.Items());
    else if (auto f = fb.try_as<Flyout>()) Visit(f.Content(), ctx);
}

void Visit(DependencyObject const& d, Ctx ctx) {
    if (!d) return;
    hstring t;
    const bool isButton = static_cast<bool>(d.try_as<Controls::Primitives::ButtonBase>());
    const bool generic = !ctx.templateMode || isButton;  // template mode: only buttons are handled for the name / tooltip
    if (isButton) ctx.inButton = true;

    if (generic) {
        if (auto name = AutomationProperties::GetName(d); Translated(name, t)) AutomationProperties::SetName(d, t);
        if (d.try_as<UIElement>() && TranslatedObj(Controls::ToolTipService::GetToolTip(d), t)) Controls::ToolTipService::SetToolTip(d, box_value(t));
    }
    if (auto uie = d.try_as<UIElement>()) VisitFlyout(uie.ContextFlyout(), ctx);

    if (auto tb = d.try_as<TextBlock>()) {
        if ((!ctx.templateMode || ctx.inButton) && Translated(tb.Text(), t)) tb.Text(t);
        return;
    }
    if (auto box = d.try_as<TextBox>()) {
        if (TranslatedObj(box.Header(), t)) box.Header(box_value(t));
        if (Translated(box.PlaceholderText(), t)) box.PlaceholderText(t);
        return;
    }
    if (auto pw = d.try_as<PasswordBox>()) {
        if (TranslatedObj(pw.Header(), t)) pw.Header(box_value(t));
        if (Translated(pw.PlaceholderText(), t)) pw.PlaceholderText(t);
        return;
    }
    if (auto nb = d.try_as<NumberBox>()) {
        if (TranslatedObj(nb.Header(), t)) nb.Header(box_value(t));
        return;
    }
    if (auto bar = d.try_as<SelectorBar>()) {
        for (auto const& it : bar.Items())
            if (Translated(it.Text(), t)) it.Text(t);
        return;
    }
    if (auto ts = d.try_as<ToggleSwitch>()) {
        if (TranslatedObj(ts.Header(), t)) ts.Header(box_value(t));
        return;
    }
    if (auto ib = d.try_as<InfoBar>()) {
        if (Translated(ib.Title(), t)) ib.Title(t);
        if (Translated(ib.Message(), t)) ib.Message(t);
        return;
    }
    if (auto exp = d.try_as<Expander>()) {
        if (TranslatedObj(exp.Header(), t)) exp.Header(box_value(t));
        else if (auto h = exp.Header().try_as<DependencyObject>()) Visit(h, ctx);
    }

    if (auto cc = d.try_as<ContentControl>()) {
        if (auto btn = d.try_as<Controls::Button>()) VisitFlyout(btn.Flyout(), ctx);
        if (auto sb = d.try_as<SplitButton>()) VisitFlyout(sb.Flyout(), ctx);
        auto content = cc.Content();
        if (!ctx.templateMode || ctx.inButton) {
            if (TranslatedObj(content, t)) { cc.Content(box_value(t)); return; }
        }
        if (auto child = content.try_as<DependencyObject>()) Visit(child, ctx);
        return;
    }
    if (auto items = d.try_as<ItemsControl>()) {
        if (auto cb = d.try_as<ComboBox>()) {
            if (TranslatedObj(cb.Header(), t)) cb.Header(box_value(t));
            if (Translated(cb.PlaceholderText(), t)) cb.PlaceholderText(t);
        }
        for (auto const& it : items.Items())
            if (auto child = it.try_as<DependencyObject>()) Visit(child, ctx);
        return;
    }
    if (auto panel = d.try_as<Controls::Panel>()) {
        for (auto const& c : panel.Children()) Visit(c, ctx);
        return;
    }
    if (auto border = d.try_as<Controls::Border>()) { Visit(border.Child(), ctx); return; }
    if (auto vb = d.try_as<Controls::Viewbox>()) { Visit(vb.Child(), ctx); return; }
    if (auto uc = d.try_as<UserControl>()) { Visit(uc.Content(), ctx); return; }
}

}  // namespace

void TranslateTree(DependencyObject const& root) {
    Visit(root, Ctx{});
}

void TranslateTemplateRoot(DependencyObject const& root) {
    Visit(root, Ctx{true, false});
}

}  // namespace lycan_app
