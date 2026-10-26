#include "pch.h"
#include "ImagePreview.h"
#include "AltBeep.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <coroutine>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sstream>

#include "AiMeta.h"
#include "DragOut.h"
#include "Foreground.h"
#include <shlobj.h>
#include <shobjidl_core.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "PreviewReader.h"
#include "Pickers.h"
#include "lycan/i18n.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/security.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace lycan_app {
namespace {

// An awaiter that continues on the UI thread (does not depend on WIL)
struct resume_ui {
    Microsoft::UI::Dispatching::DispatcherQueue q;
    bool await_ready() const { return q.HasThreadAccess(); }
    void await_suspend(std::coroutine_handle<> h) const { q.TryEnqueue([h] { h(); }); }
    void await_resume() const noexcept {}
};

constexpr uint64_t kMaxBytes = 64ull << 20;
constexpr uint64_t kMaxThumbSourceBytes = 32ull << 20;  // above this the whole file is not read just for a thumbnail
constexpr uint64_t kMaxPixels = 100ull * 1000 * 1000;
constexpr uint32_t kMaxDecodeWidth = 4096;
constexpr uint64_t kMaxAnimBytes = 256ull * 1024 * 1024;  // upper limit of all frames of an animated WebP after they are unpacked
constexpr uint32_t kThumbWidth = 96;
constexpr double kSlowLoadMs = 1500;  // if the current picture takes longer than this to read (solid / large file), the read-ahead pauses
constexpr uint64_t kMinPrefetchShare = 64ull << 20;  // when several preview windows share the read-ahead limit, each gets at least this much
int LivePreviewCount();  // number of preview windows that are open (not closing)

std::wstring LowerExt(const std::wstring& name) {
    auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring e = name.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return e;
}

std::wstring SizeText(uint64_t b) {
    wchar_t buf[48];
    if (b < 1024) swprintf_s(buf, L"%llu B", b);
    else if (b < (1ull << 20)) swprintf_s(buf, L"%.1f KB", b / 1024.0);
    else swprintf_s(buf, L"%.1f MB", b / 1048576.0);
    return buf;
}

struct Decoded {
    Microsoft::UI::Xaml::Media::Imaging::BitmapImage image{nullptr};
    uint32_t w = 0, h = 0;
    uint16_t orient = 1;          // EXIF orientation (1-8)
    bool bitmapRotated = false;   // BitmapImage has turned it upright by the EXIF orientation already (then we do not turn it again)
    uint32_t frames = 1;          // number of frames (> 1 = animation)
    std::shared_ptr<std::vector<uint8_t>> webpBytes;  // raw bytes of an animated WebP (BitmapImage does not play it, we play it frame by frame)
};

// Whether BitmapImage turns pictures upright by the EXIF orientation by itself: -1 = not known yet. Found out from the first picture with EXIF orientation 5-8 (width and height swap).
std::atomic<int> g_bitmapAutoRotates{-1};

// The orientation this picture still needs to apply itself
uint16_t EffectiveOrientation(Decoded const& d) {
    if (d.orient <= 1 || d.orient > 8) return 1;
    if (d.orient >= 5 && d.bitmapRotated) return 1;
    if (d.orient < 5 && g_bitmapAutoRotates.load() == 1) return 1;
    return d.orient;
}

// EXIF orientation -> number of clockwise rotations (x 90 degrees) and whether to mirror horizontally first (mirror, then rotate)
void OrientationToTransform(uint16_t o, int& rot, bool& mirror) {
    static const int kRot[9] = {0, 0, 0, 2, 2, 3, 1, 1, 3};
    static const bool kMir[9] = {false, false, true, false, true, true, false, true, false};
    if (o < 1 || o > 8) o = 1;
    rot = kRot[o];
    mirror = kMir[o];
}

// Display time of each frame of an animated WebP (ms): reads each ANMF chunk in the RIFF in order (bytes 12-14 of the data, 24-bit little-endian)
std::vector<int> WebpFrameDurations(std::vector<uint8_t> const& d) {
    std::vector<int> out;
    size_t p = 12;
    while (p + 8 <= d.size()) {
        const uint32_t sz = d[p + 4] | (d[p + 5] << 8) | (d[p + 6] << 16) | (static_cast<uint32_t>(d[p + 7]) << 24);
        if (std::memcmp(&d[p], "ANMF", 4) == 0 && p + 8 + 16 <= d.size()) out.push_back(d[p + 8 + 12] | (d[p + 8 + 13] << 8) | (d[p + 8 + 14] << 16));
        p += 8 + static_cast<size_t>(sz) + (sz & 1);
    }
    return out;
}

// Reads the header first to check the size (throws above the limit), then hands it to BitmapImage to decode (decodes at a smaller size when the width is above maxWidth). Must be called on the UI thread.
Windows::Foundation::IAsyncAction DecodeInto(std::shared_ptr<std::vector<uint8_t>> data, uint32_t maxWidth, Decoded& out, bool wantOrient = false) {
    Windows::Storage::Streams::InMemoryRandomAccessStream ras;
    {
        Windows::Storage::Streams::DataWriter dw(ras);
        dw.WriteBytes(array_view<const uint8_t>(data->data(), data->data() + data->size()));
        co_await dw.StoreAsync();
        dw.DetachStream();
    }
    ras.Seek(0);
    auto decoder = co_await Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(ras);
    const uint32_t w = decoder.PixelWidth(), h = decoder.PixelHeight();
    if (static_cast<uint64_t>(w) * h > kMaxPixels) throw hresult_error(E_FAIL, L"too large");
    out.frames = decoder.FrameCount();
    if (out.frames > 1 && data->size() > 12 && std::memcmp(data->data() + 8, "WEBP", 4) == 0) out.webpBytes = data;
    if (wantOrient) {
        try {
            auto props = co_await decoder.BitmapProperties().GetPropertiesAsync(single_threaded_vector<hstring>({L"System.Photo.Orientation"}));
            if (props.HasKey(L"System.Photo.Orientation"))
                if (auto r = props.Lookup(L"System.Photo.Orientation").Value().try_as<Windows::Foundation::IReference<uint16_t>>()) out.orient = r.Value();
        } catch (...) {}
    }
    ras.Seek(0);
    Microsoft::UI::Xaml::Media::Imaging::BitmapImage bi;
    if (w > maxWidth) bi.DecodePixelWidth(maxWidth);
    co_await bi.SetSourceAsync(ras);
    out.image = bi;
    out.w = w;
    out.h = h;

    if (wantOrient && out.orient >= 5 && out.orient <= 8 && w != h && bi.PixelHeight() > 0) {
        // width and height swapped = BitmapImage turned it upright itself
        const double ar = static_cast<double>(bi.PixelWidth()) / bi.PixelHeight(), raw = static_cast<double>(w) / h;
        out.bitmapRotated = std::abs(ar - 1.0 / raw) < std::abs(ar - raw);
        g_bitmapAutoRotates = out.bitmapRotated ? 1 : 0;
        // The image is already upright: layout and the status line must use the displayed size, not the stored one
        if (out.bitmapRotated) std::swap(out.w, out.h);
    }
}

struct Cached {
    std::shared_ptr<std::vector<uint8_t>> bytes;
    Decoded dec;  // image == nullptr means only the bytes are there, not decoded yet
    uint64_t Cost() const {
        uint64_t c = bytes ? bytes->size() : 0;
        if (dec.image) {
            const uint64_t dw = std::min<uint64_t>(dec.w, kMaxDecodeWidth);
            const uint64_t dh = dec.w ? static_cast<uint64_t>(dec.h) * dw / dec.w : dec.h;
            c += dw * dh * 4;
        }
        return c;
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Converting a picture to another format when it is saved, and setting it as the desktop picture

namespace {

struct EncoderChoice {
    std::wstring label;   // PNG, JPEG ...
    std::wstring ext;     // without the dot, lower case
    winrt::guid id;
};

std::wstring SaveExt(std::wstring e) {
    if (!e.empty() && e[0] == L'.') e.erase(0, 1);
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    if (e == L"jpeg" || e == L"jpe") e = L"jpg";
    if (e == L"tiff") e = L"tif";
    return e;
}

// The formats this computer can write (the codecs of Windows: PNG, JPEG, BMP, TIFF and GIF are always there; WebP, HEIF ... only when an extension
// for them is installed). Asked once.
const std::vector<EncoderChoice>& Encoders() {
    static const std::vector<EncoderChoice> list = [] {
        std::vector<EncoderChoice> out;
        try {
            static const std::pair<const wchar_t*, const wchar_t*> kWanted[] = {{L"png", L"PNG"}, {L"jpg", L"JPEG"}, {L"bmp", L"BMP"}, {L"tif", L"TIFF"},
                                                                                  {L"gif", L"GIF"}, {L"webp", L"WebP"}, {L"heic", L"HEIF"}, {L"jxr", L"JPEG XR"}};
            for (auto const& info : Windows::Graphics::Imaging::BitmapEncoder::GetEncoderInformationEnumerator()) {
                for (auto const& e : info.FileExtensions()) {
                    const std::wstring ext = SaveExt(std::wstring{e});
                    for (auto const& w : kWanted) {
                        if (ext != w.first) continue;
                        bool have = false;
                        for (auto const& o : out) have = have || o.ext == ext;
                        if (!have) out.push_back({w.second, ext, info.CodecId()});
                    }
                }
            }
            // the order of the list above (the codec list of Windows comes in any order)
            std::stable_sort(out.begin(), out.end(), [](EncoderChoice const& a, EncoderChoice const& b) {
                auto rank = [](std::wstring const& e) { int i = 0; for (auto const& w : kWanted) { if (e == w.first) return i; ++i; } return 99; };
                return rank(a.ext) < rank(b.ext);
            });
        } catch (...) {}
        return out;
    }();
    return list;
}

// The bytes of a picture in another format. `flatten`: the format has no transparency (JPEG, BMP): the picture is put on white.
Windows::Foundation::IAsyncOperation<Windows::Storage::Streams::IBuffer> ConvertImageAsync(std::vector<uint8_t> bytes, winrt::guid encoder, bool flatten) {
    using namespace Windows::Graphics::Imaging;
    using namespace Windows::Storage::Streams;
    InMemoryRandomAccessStream in;
    {
        DataWriter w(in);
        w.WriteBytes(array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        co_await w.StoreAsync();
        w.DetachStream();
    }
    std::vector<uint8_t>().swap(bytes);
    in.Seek(0);
    auto dec = co_await BitmapDecoder::CreateAsync(in);
    SoftwareBitmap sb = co_await dec.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied);
    if (flatten) {
        const int32_t w = sb.PixelWidth(), h = sb.PixelHeight();
        Buffer buf(static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4);
        sb.CopyToBuffer(buf);
        uint8_t* px = buf.data();
        buf.Length(static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4);
        for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; ++i, px += 4) {
            const unsigned inv = 255u - px[3];   // premultiplied: colour + (white * (1 - alpha))
            for (int c = 0; c < 3; ++c) px[c] = static_cast<uint8_t>((std::min)(255u, px[c] + inv));
            px[3] = 255;
        }
        sb = SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);
    }
    InMemoryRandomAccessStream out;
    auto enc = co_await BitmapEncoder::CreateAsync(encoder, out);
    enc.SetSoftwareBitmap(sb);
    co_await enc.FlushAsync();
    const uint32_t size = static_cast<uint32_t>(out.Size());
    out.Seek(0);
    DataReader r(out);
    co_await r.LoadAsync(size);
    co_return r.ReadBuffer(size);
}

// The folder for the pictures that were set as the desktop picture. Not the folder of the program (Program Files cannot be written; the
// Store version is not allowed to either) and not the data folder of the program (the Store version sees a private copy of it that Windows
// cannot read): the Pictures folder of the user.
std::filesystem::path WallpaperFolder() {
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)) && p) dir = std::filesystem::path(p) / L"LycanZip wallpapers";
    CoTaskMemFree(p);
    return dir;
}

}  // namespace

class PreviewWindow : public std::enable_shared_from_this<PreviewWindow> {
public:
    void Create(PreviewRequest req) {
        dq_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        win_ = Window{};
        // Frosted glass: a darker Acrylic, the content extends into the title bar (the minimize / maximize / close buttons of the system stay, the window can still be resized and snapped)
        win_.SystemBackdrop(Microsoft::UI::Xaml::Media::DesktopAcrylicBackdrop{});
        win_.ExtendsContentIntoTitleBar(true);
        Build();
        Start(std::move(req));
        win_.Closed([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) s->OnClosed(); });
        BringToFrontSoon();
    }

    // Open already: for the same list only change the picture; otherwise switch to the new list
    void Apply(PreviewRequest req) {
        const bool same = req.archive == req_.archive && req.items.size() == req_.items.size() && !req.items.empty() &&
                          req.items.front().entryIndex == req_.items.front().entryIndex && req.items.back().entryIndex == req_.items.back().entryIndex;
        if (same) {
            req_.start = req.start;
            req_.prefetchBytes = req.prefetchBytes;
            req_.enlargeSmall = req.enlargeSmall;
            req_.animations = req.animations;
            req_.setEnlargeSmall = std::move(req.setEnlargeSmall);
            GoTo(req.start);
        } else {
            Start(std::move(req));
        }
        BringToFrontSoon();
    }

    void Close() {
        ++gen_;
        ++session_;
        closed_ = true;
        RevokeOpened();
        if (win_) win_.Close();
    }
    bool IsClosed() const { return closed_; }
    // Release the picture data on close: WinUI keeps the Image element after the window closes, with the decoded data of its BitmapImage,
    // and without releasing it each open/close of a preview keeps about 5 MB (measured; per round 6.0 MB -> 1.4 MB)
    void ReleaseImages() {
        try {
            if (img0_) img0_.Source(nullptr);
            if (img1_) img1_.Source(nullptr);
        } catch (...) {}
        cache_.clear();
        thumbs_.clear();
        anim_ = nullptr;
        pendAnim_ = nullptr;
        webpFrames_.clear();
    }
    void OnClosed();

private:
    // ---- Window
    void Build() {
        root_ = Grid{};
        root_.RequestedTheme(ElementTheme::Dark);  // dark frosted glass, does not follow the light / dark of the main window
        ColumnDefinition c0, c1;
        c0.Width(GridLength{128, GridUnitType::Pixel});
        c1.Width(GridLength{1, GridUnitType::Star});
        root_.ColumnDefinitions().Append(c0);
        root_.ColumnDefinitions().Append(c1);
        RowDefinition r0, r1, r2;
        r0.Height(GridLength{0, GridUnitType::Auto});
        r1.Height(GridLength{1, GridUnitType::Star});
        r2.Height(GridLength{0, GridUnitType::Auto});
        root_.RowDefinitions().Append(r0);
        root_.RowDefinitions().Append(r1);
        root_.RowDefinitions().Append(r2);
        auto dim = [](uint8_t a) { return Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{a, 0, 0, 0}}; };

        // Toolbar (also the drag area of the window; the right side is left for the window buttons of the system)
        bar_ = Grid{};
        bar_.Background(dim(0x55));
        bar_.Padding(ThicknessHelper::FromLengths(10, 6, 150, 6));
        bar_.ColumnSpacing(8);
        for (int i = 0; i < 6; ++i) {
            ColumnDefinition cd;
            cd.Width(i == 3 ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Auto});
            bar_.ColumnDefinitions().Append(cd);
        }
        auto mkBtn = [](const wchar_t* glyph, const wchar_t* tip) {
            Button b;
            FontIcon ic;
            ic.Glyph(glyph);
            ic.FontSize(14);
            b.Content(ic);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(b, box_value(hstring{tip}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(b, hstring{tip});
            return b;
        };
        prev_ = mkBtn(L"", lycan::Tr(L"preview.previous"));
        next_ = mkBtn(L"", lycan::Tr(L"preview.next"));
        prev_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->Navigate(-1); });
        next_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->Navigate(+1); });
        counter_ = TextBlock{};
        counter_.VerticalAlignment(VerticalAlignment::Center);
        counter_.Opacity(0.8);
        name_ = TextBlock{};
        name_.VerticalAlignment(VerticalAlignment::Center);
        name_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        name_.TextTrimming(TextTrimming::CharacterEllipsis);
        fit_ = Primitives::ToggleButton{};
        fit_.Content(box_value(lycan::Tr(L"preview.fit_window")));
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(fit_, box_value(lycan::Tr(L"preview.switch_f_between_fit_window_width")));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(fit_, lycan::Tr(L"preview.actual_size"));
        fit_.Click([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->ToggleFit(); });
        Grid::SetColumn(prev_, 0);
        Grid::SetColumn(next_, 1);
        Grid::SetColumn(counter_, 2);
        Grid::SetColumn(name_, 3);
        Grid::SetColumn(fit_, 4);
        menuBtn_ = Button{};
        {
            FontIcon ic;
            ic.Glyph(L"\uE712");
            ic.FontSize(14);
            menuBtn_.Content(ic);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(menuBtn_, box_value(lycan::Tr(L"preview.menu_you_can_also_right_click")));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(menuBtn_, lycan::Tr(L"preview.menu"));
            menuBtn_.Flyout(BuildMenu());
        }
        Grid::SetColumn(menuBtn_, 5);
        bar_.Children().Append(prev_);
        bar_.Children().Append(next_);
        bar_.Children().Append(counter_);
        bar_.Children().Append(name_);
        bar_.Children().Append(fit_);
        bar_.Children().Append(menuBtn_);
        Grid::SetColumnSpan(bar_, 2);
        root_.Children().Append(bar_);

        // Thumbnail strip on the left
        strip_ = ListView{};
        strip_.SelectionMode(ListViewSelectionMode::Single);
        strip_.Background(dim(0x33));
        strip_.IsItemClickEnabled(false);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(strip_, lycan::Tr(L"preview.thumbnail_strip"));
        strip_.ItemTemplate(Microsoft::UI::Xaml::Markup::XamlReader::Load(
            L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>"
            L"<Grid Width='96' Height='82'>"
            L"<Border CornerRadius='4' Background='#22FFFFFF' Width='92' Height='62' VerticalAlignment='Top' Margin='0,2,0,0'/>"
            L"<FontIcon x:Name='Ph' Glyph='&#xEB9F;' FontSize='22' Opacity='0.45' VerticalAlignment='Top' Margin='0,20,0,0'/>"
            L"<Image x:Name='Thumb' Width='92' Height='62' Stretch='UniformToFill' VerticalAlignment='Top' Margin='0,2,0,0'/>"
            L"<TextBlock x:Name='Cap' FontSize='11' Opacity='0.75' TextTrimming='CharacterEllipsis' VerticalAlignment='Bottom' HorizontalAlignment='Center' MaxWidth='94'/>"
            L"</Grid></DataTemplate>").as<DataTemplate>());
        strip_.ContainerContentChanging([self = weak_from_this()](ListViewBase const&, ContainerContentChangingEventArgs const& a) {
            if (auto s = self.lock()) s->OnTile(a);
        });
        // A picture can be dragged out of the strip into Explorer (virtual file, like the list of the main window). The tile under the
        // pointer is looked up when the button goes down: the selection does not follow the press yet.
        strip_.CanDragItems(true);
        strip_.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler{[self = weak_from_this()](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            auto s = self.lock();
            if (!s) return;
            s->pressedTile_ = -1;
            for (auto const& el : Microsoft::UI::Xaml::Media::VisualTreeHelper::FindElementsInHostCoordinates(e.GetCurrentPoint(nullptr).Position(), s->strip_))
                if (auto li = el.try_as<ListViewItem>()) { s->pressedTile_ = s->strip_.IndexFromContainer(li); break; }
        }}), true);
        strip_.DragItemsStarting([self = weak_from_this()](Windows::Foundation::IInspectable const&, DragItemsStartingEventArgs const& e) {
            auto s = self.lock();
            e.Cancel(true);  // (the drag itself runs below, not through WinUI)
            if (!s || s->pressedTile_ < 0 || static_cast<size_t>(s->pressedTile_) >= s->req_.items.size()) return;
            const PreviewItem& it = s->req_.items[static_cast<size_t>(s->pressedTile_)];
            VirtualDragRequest vr;
            if (!PlanSingleFileDrag(it.entryIndex, it.name, it.size, it.mtimeFileTime, vr)) return;
            vr.archive = s->req_.archive;
            vr.open = s->req_.open;
            s->strip_.ReleasePointerCaptures();
            RunVirtualDrag(std::move(vr));
        });
        strip_.SelectionChanged([self = weak_from_this()](auto&&, auto&&) {
            auto s = self.lock();
            if (!s || s->syncingStrip_) return;
            const int i = s->strip_.SelectedIndex();
            if (i >= 0 && static_cast<size_t>(i) != s->pos_) s->GoTo(static_cast<size_t>(i));
        });
        Grid::SetRow(strip_, 1);
        root_.Children().Append(strip_);

        // Content: scroll area (scrollable at actual size) + loading + message
        scroll_ = ScrollViewer{};
        scroll_.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll_.ZoomMode(ZoomMode::Disabled);
        host_ = Grid{};
        host_.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});  // an empty area also receives the mouse (drag to pan, double click)
        host_.ContextFlyout(BuildMenu());  // right click on the picture: the same menu
        wrapper_ = Grid{};
        wrapper_.HorizontalAlignment(HorizontalAlignment::Center);
        wrapper_.VerticalAlignment(VerticalAlignment::Center);
        // Two Images on top of each other: the new picture loads behind first and after it is drawn both change at once (old fades out = new appears), so switching does not flash
        for (Image* img : {&img0_, &img1_}) {
            *img = Image{};
            img->Stretch(Microsoft::UI::Xaml::Media::Stretch::Fill);
            img->HorizontalAlignment(HorizontalAlignment::Center);
            img->VerticalAlignment(VerticalAlignment::Center);
            img->Opacity(0);
            wrapper_.Children().Append(*img);
        }
        host_.Children().Append(wrapper_);
        scroll_.Content(host_);
        host_.DoubleTapped([self = weak_from_this()](Windows::Foundation::IInspectable const&, Input::DoubleTappedRoutedEventArgs const& e) {
            if (auto s = self.lock()) {
                auto p = e.GetPosition(s->scroll_);
                s->ToggleFit(p.X, p.Y);
            }
        });
        host_.PointerPressed([self = weak_from_this()](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            auto s = self.lock();
            if (!s || !s->CanPan()) return;
            auto pt = e.GetCurrentPoint(s->scroll_);
            if (!pt.Properties().IsLeftButtonPressed()) return;
            s->panning_ = true;
            s->panStart_ = pt.Position();
            s->panOffX_ = s->scroll_.HorizontalOffset();
            s->panOffY_ = s->scroll_.VerticalOffset();
            s->host_.CapturePointer(e.Pointer());
        });
        host_.PointerMoved([self = weak_from_this()](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            auto s = self.lock();
            if (!s || !s->panning_) return;
            auto p = e.GetCurrentPoint(s->scroll_).Position();
            s->scroll_.ChangeView(s->panOffX_ - (p.X - s->panStart_.X), s->panOffY_ - (p.Y - s->panStart_.Y), nullptr, true);
        });
        auto endPan = [self = weak_from_this()](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            if (auto s = self.lock()) { s->panning_ = false; s->host_.ReleasePointerCaptures(); }
        };
        host_.PointerReleased(endPan);
        host_.PointerCanceled(endPan);
        host_.PointerCaptureLost(endPan);
        scroll_.SizeChanged([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->Reflow(); });
        bodyGrid_ = Grid{};
        auto& body = bodyGrid_;
        {
            ColumnDefinition b0, b1;
            b0.Width(GridLength{1, GridUnitType::Star});
            b1.Width(GridLength{0, GridUnitType::Pixel});
            body.ColumnDefinitions().Append(b0);
            body.ColumnDefinitions().Append(b1);
        }
        body.Children().Append(scroll_);
        // Picture information panel (right side, hidden by default)
        infoPanel_ = Border{};
        infoPanel_.Background(dim(0x77));
        infoPanel_.Visibility(Visibility::Collapsed);
        {
            ScrollViewer sv;
            sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            infoList_ = StackPanel{};
            infoList_.Spacing(2);
            infoList_.Padding(ThicknessHelper::FromLengths(14, 10, 14, 14));
            sv.Content(infoList_);
            infoPanel_.Child(sv);
        }
        Grid::SetColumn(infoPanel_, 1);
        body.Children().Append(infoPanel_);
        ring_ = ProgressRing{};
        ring_.Width(48);
        ring_.Height(48);
        ring_.HorizontalAlignment(HorizontalAlignment::Center);
        ring_.VerticalAlignment(VerticalAlignment::Center);
        body.Children().Append(ring_);
        msg_ = TextBlock{};
        msg_.HorizontalAlignment(HorizontalAlignment::Center);
        msg_.VerticalAlignment(VerticalAlignment::Center);
        msg_.TextWrapping(TextWrapping::Wrap);
        msg_.MaxWidth(480);
        msg_.TextAlignment(TextAlignment::Center);
        msg_.Visibility(Visibility::Collapsed);
        body.Children().Append(msg_);
        toastText_ = TextBlock{};
        toastText_.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 255, 255, 255}});
        toast_ = Border{};
        toast_.Child(toastText_);
        toast_.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        toast_.Padding(ThicknessHelper::FromLengths(16, 8, 16, 8));
        toast_.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0xD8, 0x20, 0x20, 0x24}});
        toast_.HorizontalAlignment(HorizontalAlignment::Center);
        toast_.VerticalAlignment(VerticalAlignment::Bottom);
        toast_.Margin(ThicknessHelper::FromLengths(0, 0, 0, 28));
        toast_.IsHitTestVisible(false);
        toast_.Visibility(Visibility::Collapsed);
        body.Children().Append(toast_);
        // Mouse wheel switches to the previous / next picture (switches only after a full notch accumulates, so small scrolls of a touchpad do not jump several times).
        // At "actual size" with a picture larger than the window the wheel scrolls. The thumbnail strip scrolls by itself, not affected.
        body.AddHandler(UIElement::PointerWheelChangedEvent(), box_value(Input::PointerEventHandler(
            [self = weak_from_this(), body](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
                auto s = self.lock();
                if (!s) return;
                const int delta = e.GetCurrentPoint(body).Properties().MouseWheelDelta();
                if (static_cast<uint32_t>(e.KeyModifiers()) & static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Control)) {
                    auto p = e.GetCurrentPoint(s->scroll_).Position();  // Ctrl + wheel: zoom around the cursor position
                    s->ZoomBy(std::pow(1.1, delta / 120.0), p.X, p.Y);
                    e.Handled(true);
                    return;
                }
                if (s->CanPan()) return;  // picture larger than the window: the wheel is for scrolling
                s->wheelAcc_ += delta;
                while (s->wheelAcc_ >= 120) { s->wheelAcc_ -= 120; s->Navigate(-1); }
                while (s->wheelAcc_ <= -120) { s->wheelAcc_ += 120; s->Navigate(+1); }
                e.Handled(true);
            })), true);
        Grid::SetRow(body, 1);
        Grid::SetColumn(body, 1);
        root_.Children().Append(body);

        status_ = TextBlock{};
        status_.Margin(ThicknessHelper::FromLengths(14, 6, 14, 8));
        status_.Opacity(0.8);
        statusBar_ = Border{};
        auto& statusBar = statusBar_;
        statusBar.Background(dim(0x55));
        statusBar.Child(status_);
        Grid::SetRow(statusBar, 2);
        Grid::SetColumnSpan(statusBar, 2);
        root_.Children().Append(statusBar);

        // Keyboard shortcuts (KeyboardAccelerator: they work wherever the focus is)
        auto key = [&](Windows::System::VirtualKey k, auto&& act, Windows::System::VirtualKeyModifiers m = Windows::System::VirtualKeyModifiers::None) {
            Input::KeyboardAccelerator ka;
            ka.Key(k);
            ka.Modifiers(m);
            ka.Invoked([self = weak_from_this(), act](auto&&, Input::KeyboardAcceleratorInvokedEventArgs const& a) {
                if (auto s = self.lock()) { act(*s); a.Handled(true); }
            });
            root_.KeyboardAccelerators().Append(ka);
        };
        using VK = Windows::System::VirtualKey;
        using VM = Windows::System::VirtualKeyModifiers;
        const auto kShift = VM::Shift;
        const auto kCtrl = VM::Control;
        const auto kCtrlShift = static_cast<VM>(static_cast<uint32_t>(VM::Control) | static_cast<uint32_t>(VM::Shift));
        key(VK::Escape, [](PreviewWindow& s) { if (s.fullscreen_) s.ToggleFullscreen(); else s.Close(); });
        // Arrow keys: pan when the picture is larger than the window, otherwise page
        key(VK::Left, [](PreviewWindow& s) { if (s.CanPan()) s.Pan(-80, 0); else s.Navigate(-1); });
        key(VK::Up, [](PreviewWindow& s) { if (s.CanPan()) s.Pan(0, -80); else s.Navigate(-1); });
        key(VK::Right, [](PreviewWindow& s) { if (s.CanPan()) s.Pan(80, 0); else s.Navigate(+1); });
        key(VK::Down, [](PreviewWindow& s) { if (s.CanPan()) s.Pan(0, 80); else s.Navigate(+1); });
        key(VK::Space, [](PreviewWindow& s) { s.Navigate(+1); });
        key(VK::PageDown, [](PreviewWindow& s) { s.Navigate(+1); });
        key(VK::Back, [](PreviewWindow& s) { s.Navigate(-1); });
        key(VK::PageUp, [](PreviewWindow& s) { s.Navigate(-1); });
        key(VK::Home, [](PreviewWindow& s) { s.GoTo(0); });
        key(VK::End, [](PreviewWindow& s) { s.GoTo(s.req_.items.empty() ? 0 : s.req_.items.size() - 1); });
        key(VK::F, [](PreviewWindow& s) { s.ToggleFit(); });
        key(VK::Number0, [](PreviewWindow& s) { s.SetFit(0); }, kCtrl);
        key(VK::Number1, [](PreviewWindow& s) { s.ActualSize(); }, kCtrl);
        key(VK::Number2, [](PreviewWindow& s) { s.SetFit(1); }, kCtrl);
        key(VK::Number3, [](PreviewWindow& s) { s.SetFit(2); }, kCtrl);
        for (auto k : {VK::Add, static_cast<VK>(187)}) key(k, [](PreviewWindow& s) { s.ZoomBy(1.25); });                // + / =
        key(static_cast<VK>(187), [](PreviewWindow& s) { s.ZoomBy(1.25); }, kShift);                                    // Shift + = is +
        for (auto k : {VK::Subtract, static_cast<VK>(189)}) key(k, [](PreviewWindow& s) { s.ZoomBy(0.8); });            // − / -
        key(VK::F11, [](PreviewWindow& s) { s.ToggleFullscreen(); });
        key(VK::R, [](PreviewWindow& s) { s.Rotate(+1); });
        key(VK::R, [](PreviewWindow& s) { s.Rotate(-1); }, kShift);
        key(VK::T, [](PreviewWindow& s) { s.ToggleThumbs(); });
        key(VK::B, [](PreviewWindow& s) { s.ToggleThumbs(); }, kCtrl);
        key(VK::C, [](PreviewWindow& s) { s.CopyImageAsync(); }, kCtrl);
        key(VK::C, [](PreviewWindow& s) { s.CopyPath(); }, kCtrlShift);
        key(VK::S, [](PreviewWindow& s) { s.ExtractCurrentAsync(); }, kCtrl);
        key(VK::E, [](PreviewWindow& s) { s.ExtractCurrentAsync(); }, kCtrl);
        key(VK::P, [](PreviewWindow& s) { s.TogglePlay(); });
        key(VK::I, [](PreviewWindow& s) { s.ToggleInfo(); });
        key(VK::Enter, [](PreviewWindow& s) { s.ToggleInfo(); }, VM::Menu);
        key(VK::Enter, [](PreviewWindow& s) { if (s.req_.openExternal && s.pos_ < s.req_.items.size()) s.req_.openExternal(s.req_.items[s.pos_].node); });
        root_.KeyboardAcceleratorPlacementMode(Input::KeyboardAcceleratorPlacementMode::Hidden);
        // A button that has the focus (the first thing focused is the "previous" button) uses Enter to press itself, and the
        // accelerator above never sees it. Enter means "open with the associated program" wherever the focus is.
        root_.PreviewKeyDown([self = weak_from_this()](auto&&, Input::KeyRoutedEventArgs const& a) {
            using Microsoft::UI::Input::InputKeyboardSource;
            using Windows::UI::Core::CoreVirtualKeyStates;
            if (a.Key() != Windows::System::VirtualKey::Enter) return;
            if (static_cast<uint32_t>(InputKeyboardSource::GetKeyStateForCurrentThread(Windows::System::VirtualKey::Menu)) & static_cast<uint32_t>(CoreVirtualKeyStates::Down)) return;  // Alt+Enter: the info panel
            auto s = self.lock();
            if (s && s->req_.openExternal && s->pos_ < s->req_.items.size()) {
                s->req_.openExternal(s->req_.items[s->pos_].node);
                a.Handled(true);
            }
        });

        win_.Content(root_);
        // SetTitleBar is not used (the whole toolbar would count as the title bar, and the buttons for previous / next picture, pressed quickly, would be taken as a double click on the title bar and maximize / restore);
        // only the empty column in the middle of the toolbar is made the drag area, the rest (buttons, counter) is ordinary client area
        bar_.SizeChanged([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) s->UpdateDragRects(); });
        auto tb = win_.AppWindow().TitleBar();
        const Windows::UI::Color clear{0, 0, 0, 0};
        tb.ButtonBackgroundColor(clear);
        tb.ButtonInactiveBackgroundColor(clear);
        tb.ButtonForegroundColor(Windows::UI::Color{255, 255, 255, 255});
        tb.ButtonInactiveForegroundColor(Windows::UI::Color{255, 160, 160, 160});

        // Window size: about 1100 x 720 DIP
        HWND h{};
        check_hresult(win_.as<::IWindowNative>()->get_WindowHandle(&h));
        const double scale = GetDpiForWindow(h) / 96.0;
        win_.AppWindow().Resize(Windows::Graphics::SizeInt32{static_cast<int32_t>(1100 * scale), static_cast<int32_t>(720 * scale)});
    }

    // Starts (or restarts) a list
    void Start(PreviewRequest req) {
        ++session_;
        ++gen_;
        req_ = std::move(req);
        cache_.clear();
        thumbs_.clear();
        wanted_.clear();
        readerA_.Reset(req_.archive, req_.open);
        readerB_.Reset(req_.archive, req_.open);
        readerC_.Reset(req_.archive, req_.open);
        auto names = single_threaded_observable_vector<Windows::Foundation::IInspectable>();
        // Every item must be a different object: two pictures with the same name (in different folders) boxed as equal
        // strings would be the same item to the list, and selecting one would select / scroll to the other. The tile's
        // text comes from req_.items (OnTile); the value is only the position.
        for (size_t i = 0; i < req_.items.size(); ++i) names.Append(box_value(static_cast<uint32_t>(i)));
        syncingStrip_ = true;
        strip_.ItemsSource(names);
        syncingStrip_ = false;
        pos_ = std::min(req_.start, req_.items.empty() ? size_t{0} : req_.items.size() - 1);
        GoTo(pos_);
    }

    void UpdateDragRects() {
        if (!win_ || !bar_) return;
        HWND h{};
        check_hresult(win_.as<::IWindowNative>()->get_WindowHandle(&h));
        const double scale = GetDpiForWindow(h) / 96.0;
        auto cols = bar_.ColumnDefinitions();
        double x = bar_.Padding().Left;
        for (uint32_t i = 0; i < 3; ++i) x += cols.GetAt(i).ActualWidth() + bar_.ColumnSpacing();
        const double w = cols.GetAt(3).ActualWidth();
        if (w <= 0) return;
        Windows::Graphics::RectInt32 r{static_cast<int32_t>(x * scale), 0, static_cast<int32_t>(w * scale), static_cast<int32_t>(bar_.ActualHeight() * scale)};
        win_.AppWindow().TitleBar().SetDragRectangles(array_view<const Windows::Graphics::RectInt32>(&r, &r + 1));
    }

    void Navigate(int d) {
        if (req_.items.size() < 2) return;
        const int64_t n = static_cast<int64_t>(req_.items.size());
        const bool wraps = (d > 0 && pos_ + 1 == req_.items.size()) || (d < 0 && pos_ == 0);
        GoTo(static_cast<size_t>(((static_cast<int64_t>(pos_) + d) % n + n) % n));  // wraps around to the other end at the ends
        if (wraps) ShowToast(lycan::Tr(d > 0 ? L"preview.this_is_first" : L"preview.this_is_last"));   // going round: say where we are
    }

    // A short note over the picture (1.5 seconds)
    void ShowToast(hstring const& text) {
        if (!toast_) return;
        toastText_.Text(text);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(toast_, text);
        toast_.Visibility(Visibility::Visible);
        if (!toastTimer_) {
            toastTimer_ = dq_.CreateTimer();
            toastTimer_.IsRepeating(false);
            toastTimer_.Interval(std::chrono::milliseconds(1500));
            toastTimer_.Tick([weak = weak_from_this()](auto&&, auto&&) {
                if (auto s = weak.lock()) { s->toastTimer_.Stop(); s->toast_.Visibility(Visibility::Collapsed); }
            });
        }
        toastTimer_.Stop();
        toastTimer_.Start();
    }

    void GoTo(size_t pos) {
        if (req_.items.empty() || pos >= req_.items.size()) return;
        if (pos != pos_) dir_ = pos > pos_ ? +1 : -1;
        pos_ = pos;
        syncingStrip_ = true;
        strip_.SelectedIndex(static_cast<int32_t>(pos_));
        if (auto item = strip_.SelectedItem()) strip_.ScrollIntoView(item);
        syncingStrip_ = false;
        LoadAsync(pos_);
    }

    // On a double click open, the main window takes back the focus after the mouse is released and would cover the new window: bring this one to the front at once and again later (150 ms)
    void BringToFront() {
        HWND h{};
        check_hresult(win_.as<::IWindowNative>()->get_WindowHandle(&h));
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        { HWND hw{}; if (SUCCEEDED(win_.as<::IWindowNative>()->get_WindowHandle(&hw))) lycan_app::SuppressAltBeep(hw); }
        win_.Activate();
        lycan_app::ForceForeground(h);
        win_.AppWindow().MoveInZOrderAtTop();
    }
    void BringToFrontSoon() {
        BringToFront();
        if (!activateTimer_) {
            activateTimer_ = dq_.CreateTimer();
            activateTimer_.IsRepeating(false);
            activateTimer_.Interval(std::chrono::milliseconds(150));
            activateTimer_.Tick([weak = weak_from_this()](auto&&, auto&&) { if (auto s = weak.lock()) { s->activateTimer_.Stop(); if (!s->closed_) s->BringToFront(); } });
        }
        activateTimer_.Stop();
        activateTimer_.Start();
    }

    // ---- Showing the picture: zoom / pan / rotate / switch without flicker
    Image& Slot(int i) { return i == 0 ? img0_ : img1_; }
    double ScaleF() {
        HWND h{};
        check_hresult(win_.as<::IWindowNative>()->get_WindowHandle(&h));
        return std::max(1.0, GetDpiForWindow(h) / 96.0);
    }
    bool CanPan() { return hasImage_ && (scroll_.ScrollableWidth() > 0.5 || scroll_.ScrollableHeight() > 0.5); }
    int TotalRot() const { return (baseRot_ + rot_) % 4; }
    static void RotDimsFor(int rot, uint32_t w, uint32_t h, uint32_t& rw, uint32_t& rh) {
        if (rot % 2) { rw = h; rh = w; } else { rw = w; rh = h; }
    }
    // Zoom ratio of fit window / width / height (1.0 = actual pixels; by default small pictures are not enlarged, only with the setting "also enlarge small pictures")
    double FitZoomFor(uint32_t rw, uint32_t rh) {
        const double s = ScaleF(), vw = scroll_.ActualWidth(), vh = scroll_.ActualHeight();
        if (vw <= 4 || vh <= 4 || !rw || !rh) return 1.0;
        const double zw = (vw - 4) * s / rw, zh = (vh - 4) * s / rh;
        const double z = fitKind_ == 1 ? zw : fitKind_ == 2 ? zh : std::min(zw, zh);
        return std::min(z, req_.enlargeSmall ? 32.0 : 1.0);
    }
    static const wchar_t* FitName(int kind) { return kind == 1 ? lycan::Tr(L"preview.fit_width") : kind == 2 ? lycan::Tr(L"preview.fit_height") : lycan::Tr(L"preview.fit_window"); }
    double ZoomNow() {
        uint32_t rw, rh;
        RotDimsFor(TotalRot(), imgW_, imgH_, rw, rh);
        return fitMode_ ? FitZoomFor(rw, rh) : zoom_;
    }
    void LayoutSlot(Image const& img, uint32_t iw, uint32_t ih, double z, int rot, bool mirror) {
        const double s = ScaleF();
        const double iwD = iw * z / s, ihD = ih * z / s;
        img.Width(iwD);
        img.Height(ihD);
        // The wrapper has the size of the rotated picture. At 90 / 270 degrees the picture (laid out unrotated) is larger than the wrapper in one
        // direction, and the layout would squeeze it into the wrapper before it is rotated: a negative margin gives it its full size back.
        const bool odd = rot % 2 != 0;
        const double mx = odd ? std::min((ihD - iwD) / 2, 0.0) : 0.0, my = odd ? std::min((iwD - ihD) / 2, 0.0) : 0.0;
        img.Margin(ThicknessHelper::FromLengths(mx, my, mx, my));
        auto ct = img.RenderTransform().try_as<Microsoft::UI::Xaml::Media::CompositeTransform>();
        if (!ct) {
            ct = Microsoft::UI::Xaml::Media::CompositeTransform{};
            img.RenderTransform(ct);
            img.RenderTransformOrigin(Windows::Foundation::Point{0.5f, 0.5f});
        }
        ct.ScaleX(mirror ? -1.0 : 1.0);   // mirror first, then rotate (order of CompositeTransform: scale -> rotate)
        ct.Rotation(rot * 90.0);
    }
    void LayoutWrapper(uint32_t iw, uint32_t ih, double z, int rot, bool keepLarger = false) {
        uint32_t rw, rh;
        RotDimsFor(rot, iw, ih, rw, rh);
        const double s = ScaleF();
        double ww = rw * z / s, wh = rh * z / s;
        if (keepLarger) {  // while waiting for a switch: the old picture is still in the frame, and the frame must not shrink first, or the old picture would be squeezed to the size of the new one
            if (!std::isnan(wrapper_.Width())) ww = std::max(ww, wrapper_.Width());
            if (!std::isnan(wrapper_.Height())) wh = std::max(wh, wrapper_.Height());
        }
        wrapper_.Width(ww);
        wrapper_.Height(wh);
        host_.MinWidth(std::max(0.0, scroll_.ActualWidth() - 2));
        host_.MinHeight(std::max(0.0, scroll_.ActualHeight() - 2));
    }
    // Lays out again when the window size, zoom or rotation changes (the picture shown now)
    void Reflow() {
        if (!hasImage_) return;
        const double z = ZoomNow();
        LayoutSlot(Slot(front_), imgW_, imgH_, z, TotalRot(), mirror_);
        LayoutWrapper(imgW_, imgH_, z, TotalRot());
        UpdateZoomUi(z);
    }
    void UpdateZoomUi(double z) {
        wchar_t pct[32];
        swprintf_s(pct, L"%d%%", static_cast<int>(std::lround(z * 100)));
        fit_.IsChecked(!fitMode_);
        const hstring label = fitMode_ ? hstring{lycan::Tf(L"fmt.value_paren", FitName(fitKind_), pct)} : (std::abs(z - 1.0) < 1e-6 ? hstring{lycan::Tf(L"preview.actual_size_percent", pct)} : hstring{pct});
        fit_.Content(box_value(label));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(fit_, label);
        if (flashTimer_ && flashTimer_.IsRunning()) return;  // a temporary message is shown, do not cover it
        status_.Text(hstring{statusBase_ + std::wstring{lycan::Tr(L"fmt.dot_gap")} + pct});
    }
    void Flash(const std::wstring& text) {
        status_.Text(hstring{text});
        if (!flashTimer_) {
            flashTimer_ = dq_.CreateTimer();
            flashTimer_.IsRepeating(false);
            flashTimer_.Interval(std::chrono::milliseconds(2500));
            flashTimer_.Tick([self = weak_from_this()](auto&&, auto&&) { if (auto s = self.lock()) { s->flashTimer_.Stop(); if (s->hasImage_) s->UpdateZoomUi(s->ZoomNow()); } });
        }
        flashTimer_.Stop();
        flashTimer_.Start();
    }
    // Zoom to z1 around (fx, fy) (coordinates in the scroll area); fx < 0 = around the center of the window
    void ZoomTo(double z1, double fx = -1, double fy = -1) {
        if (!hasImage_) return;
        z1 = std::clamp(z1, 0.02, 32.0);
        const double z0 = ZoomNow();
        if (fx < 0) { fx = scroll_.ActualWidth() / 2; fy = scroll_.ActualHeight() / 2; }
        const auto p0 = wrapper_.TransformToVisual(scroll_).TransformPoint(Windows::Foundation::Point{0, 0});
        const double w0 = std::max(1.0, wrapper_.ActualWidth()), h0 = std::max(1.0, wrapper_.ActualHeight());
        const double u = std::clamp((fx - p0.X) / w0, 0.0, 1.0), v = std::clamp((fy - p0.Y) / h0, 0.0, 1.0);
        fitMode_ = false;
        zoom_ = z1;
        Reflow();
        scroll_.UpdateLayout();
        const auto p1 = wrapper_.TransformToVisual(scroll_).TransformPoint(Windows::Foundation::Point{0, 0});
        const double dx = p1.X + u * wrapper_.ActualWidth() - fx, dy = p1.Y + v * wrapper_.ActualHeight() - fy;
        scroll_.ChangeView(scroll_.HorizontalOffset() + dx, scroll_.VerticalOffset() + dy, nullptr, true);
        (void)z0;
    }
    void ZoomBy(double factor, double fx = -1, double fy = -1) { if (hasImage_) ZoomTo(ZoomNow() * factor, fx, fy); }
    void SetFit(int kind = -1) {
        if (!hasImage_) return;
        if (kind >= 0) fitKind_ = kind;
        fitMode_ = true;
        Reflow();
        scroll_.ChangeView(0.0, 0.0, nullptr, true);
    }
    void ActualSize(double fx = -1, double fy = -1) { ZoomTo(1.0, fx, fy); }
    // Menu: "enlarge small images" (same setting as the settings page). Only the fit modes depend on it.
    void ToggleEnlargeSmall() {
        req_.enlargeSmall = !req_.enlargeSmall;
        if (req_.setEnlargeSmall) req_.setEnlargeSmall(req_.enlargeSmall);
        if (hasImage_ && fitMode_) Reflow();
    }
    void ToggleFit(double fx = -1, double fy = -1) {
        if (!hasImage_) return;
        if (fitMode_) ActualSize(fx, fy); else SetFit();
    }
    void Pan(double dx, double dy) {
        scroll_.ChangeView(scroll_.HorizontalOffset() + dx, scroll_.VerticalOffset() + dy, nullptr, false);
    }
    void Rotate(int dir) {
        if (!hasImage_) return;
        rot_ = (rot_ + dir + 4) % 4;
        Reflow();
        scroll_.ChangeView(0.0, 0.0, nullptr, true);
    }
    void UpdateChrome() {
        const bool bars = !fullscreen_;
        const bool strip = !fullscreen_ && !thumbsHidden_;
        bar_.Visibility(bars ? Visibility::Visible : Visibility::Collapsed);
        statusBar_.Visibility(bars ? Visibility::Visible : Visibility::Collapsed);
        strip_.Visibility(strip ? Visibility::Visible : Visibility::Collapsed);
        root_.ColumnDefinitions().GetAt(0).Width(GridLength{strip ? 128.0 : 0.0, GridUnitType::Pixel});
        Reflow();
    }
    void ToggleThumbs() { thumbsHidden_ = !thumbsHidden_; UpdateChrome(); }
    // ---- Animated WebP: WIC decodes frame by frame into a SoftwareBitmapSource and the picture is switched by the ANMF timing (if the total is too large only the first frame is shown)
    void StopWebp() {
        ++webpGen_;
        webpPlaying_ = false;
        if (webpTimer_) webpTimer_.Stop();
        webpFrames_.clear();
        webpDur_.clear();
    }
    void WebpNext() {
        if (!webpPlaying_ || closed_ || animPaused_ || webpFrames_.empty()) return;
        if (!webpTimer_) {
            webpTimer_ = dq_.CreateTimer();
            webpTimer_.IsRepeating(false);
            webpTimer_.Tick([weak = weak_from_this()](auto&&, auto&&) {
                auto s = weak.lock();
                if (!s || !s->webpPlaying_ || s->closed_ || s->animPaused_ || s->webpFrames_.empty()) return;
                s->webpIdx_ = (s->webpIdx_ + 1) % s->webpFrames_.size();
                s->Slot(s->webpSlot_).Source(s->webpFrames_[s->webpIdx_]);
                s->WebpNext();
            });
        }
        webpTimer_.Stop();
        const int ms = webpDur_.size() > webpIdx_ ? std::clamp(webpDur_[webpIdx_], 20, 10000) : 100;
        webpTimer_.Interval(std::chrono::milliseconds(ms));
        webpTimer_.Start();
    }
    fire_and_forget PlayWebp(std::shared_ptr<std::vector<uint8_t>> bytes, int slot, uint32_t gen) {
        auto self = shared_from_this();
        try {
            Windows::Storage::Streams::InMemoryRandomAccessStream ras;
            {
                Windows::Storage::Streams::DataWriter dw(ras);
                dw.WriteBytes(array_view<const uint8_t>(bytes->data(), bytes->data() + bytes->size()));
                co_await dw.StoreAsync();
                dw.DetachStream();
            }
            ras.Seek(0);
            auto dec = co_await Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(ras);
            const uint32_t n = dec.FrameCount();
            if (n < 2 || n > 1000 || static_cast<uint64_t>(n) * dec.PixelWidth() * dec.PixelHeight() * 4 > kMaxAnimBytes) {
                if (gen == webpGen_) Flash(lycan::Tr(L"preview.animation_too_large_only_first"));
                co_return;
            }
            std::vector<Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource> frames;
            for (uint32_t i = 0; i < n; ++i) {
                auto fr = co_await dec.GetFrameAsync(i);
                auto sb = co_await fr.GetSoftwareBitmapAsync(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied);
                Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource src;
                co_await src.SetBitmapAsync(sb);
                if (gen != webpGen_ || closed_) co_return;
                frames.push_back(src);
            }
            if (gen != webpGen_ || closed_) co_return;
            webpFrames_ = std::move(frames);
            webpDur_ = WebpFrameDurations(*bytes);
            webpIdx_ = 0;
            webpSlot_ = slot;
            webpPlaying_ = true;
            if (!animPaused_) WebpNext();
        } catch (...) {}
    }

    // Animation (GIF) play / pause
    void TogglePlay() {
        if (!anim_ || !hasImage_) return;
        animPaused_ = !animPaused_;
        if (webpPlaying_) {
            if (animPaused_) { if (webpTimer_) webpTimer_.Stop(); } else WebpNext();
        } else if (animPaused_) anim_.Stop(); else anim_.Play();
        Flash(animPaused_ ? lycan::Tr(L"preview.paused_press_p_resume") : lycan::Tr(L"preview.playing"));
    }
    void ToggleFullscreen() {
        fullscreen_ = !fullscreen_;
        win_.AppWindow().SetPresenter(fullscreen_ ? Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen
                                                  : Microsoft::UI::Windowing::AppWindowPresenterKind::Default);
        UpdateChrome();
    }

    // ---- Loading indicator: nothing is shown if it is quick; after 250 ms the current picture is dimmed and a ring is shown (the old picture is not cleared, to avoid a flash)
    void BeginLoading(uint32_t gen) {
        loading_ = true;
        loadGen_ = gen;
        if (!loadTimer_) {
            loadTimer_ = dq_.CreateTimer();
            loadTimer_.IsRepeating(false);
            loadTimer_.Interval(std::chrono::milliseconds(250));
            loadTimer_.Tick([self = weak_from_this()](auto&&, auto&&) {
                auto s = self.lock();
                if (!s) return;
                s->loadTimer_.Stop();
                if (!s->loading_ || s->loadGen_ != s->gen_.load()) return;
                s->ring_.Visibility(Visibility::Visible);
                s->ring_.IsActive(true);
                if (s->hasImage_) s->Slot(s->front_).Opacity(0.45);
            });
        }
        loadTimer_.Stop();
        loadTimer_.Start();
    }
    void EndLoading() {
        loading_ = false;
        if (loadTimer_) loadTimer_.Stop();
        ring_.IsActive(false);
        ring_.Visibility(Visibility::Collapsed);
        if (hasImage_) Slot(front_).Opacity(1);
    }

    void ClearImages() {
        ++presentToken_;
        img0_.Source(nullptr);
        img1_.Source(nullptr);
        img0_.Opacity(0);
        img1_.Opacity(0);
        hasImage_ = false;
    }

    void ShowMessage(const std::wstring& text) {
        EndLoading();
        ClearImages();
        msg_.Text(hstring{text});
        msg_.Visibility(Visibility::Visible);
    }

    // Switch without flicker: the new picture goes into the Image behind and is laid out first (Opacity 0); when it is really drawn (ImageOpened; safety 60 ms)
    // the new picture appears and the old one disappears at once, so the screen never shows "old picture gone -> empty -> new picture pops up".
    void Present(Decoded const& d, PreviewItem const& item) {
        const uint32_t token = ++presentToken_;
        const bool keepActual = hasImage_ && !fitMode_;  // "actual size" is kept until the next picture, other zoom modes go back to fit window
        int br = 0;
        bool mir = false;
        OrientationToTransform(EffectiveOrientation(d), br, mir);  // EXIF orientation: turn upright automatically
        fitMode_ = !keepActual;
        zoom_ = 1.0;
        uint32_t rw, rh;
        RotDimsFor(br, d.w, d.h, rw, rh);
        const double z = fitMode_ ? FitZoomFor(rw, rh) : 1.0;
        const int back = front_ ^ 1;
        Image img = Slot(back);
        img.Opacity(0.003);  // almost transparent but still drawn (an element with Opacity 0 may not be drawn at all, which gives an empty frame when it first appears)
        img.Source(d.image);
        LayoutSlot(img, d.w, d.h, z, br, mir);
        LayoutWrapper(d.w, d.h, z, br, hasImage_);
        statusBase_ = std::to_wstring(d.w) + L" × " + std::to_wstring(d.h) + std::wstring{lycan::Tr(L"fmt.dot_gap")} + SizeText(item.size) + std::wstring{lycan::Tr(L"fmt.dot_gap")} + item.path;
        pendBase_ = br;
        pendMirror_ = mir;
        pendAnim_ = d.frames > 1 ? d.image : nullptr;
        pendWebp_ = d.webpBytes;
        const uint32_t w = d.w, h = d.h;
        // The handler may only capture a weak reference: ImageOpened often never fires (the switch is done by the safety timer below), and a strong reference would keep the whole preview window
        // (with the decode cache) from ever being released - about 200 MB leaked for each open/close of a preview. A handler that did not fire is removed by RevokeOpened.
        RevokeOpened();
        openedImg_ = img;
        openedTok_ = img.ImageOpened([weak = weak_from_this(), token, back, w, h](auto&&, auto&&) {
            auto self = weak.lock();
            if (!self) return;
            self->RevokeOpened();
            self->AfterFrames(2, [weak, token, back, w, h] { if (auto s = weak.lock()) s->Swap(token, back, w, h); });  // the new picture must really be drawn for two frames before they swap
        });
        pendToken_ = token;
        pendBack_ = back;
        pendW_ = w;
        pendH_ = h;
        if (!swapTimer_) {
            swapTimer_ = dq_.CreateTimer();
            swapTimer_.IsRepeating(false);
            swapTimer_.Interval(std::chrono::milliseconds(150));
            swapTimer_.Tick([weak = weak_from_this()](auto&&, auto&&) {
                if (auto s = weak.lock()) { s->swapTimer_.Stop(); s->Swap(s->pendToken_, s->pendBack_, s->pendW_, s->pendH_); }
            });
        }
        swapTimer_.Stop();
        swapTimer_.Start();
    }
    // Runs once after n screen updates (CompositionTarget.Rendering)
    void AfterFrames(int n, std::function<void()> fn) {
        auto tok = std::make_shared<event_token>();
        auto count = std::make_shared<int>(0);
        *tok = Microsoft::UI::Xaml::Media::CompositionTarget::Rendering([tok, count, n, fn](Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&) {
            if (++*count >= n) {
                Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(*tok);
                fn();
            }
        });
    }
    // Removes an ImageOpened handler that has not fired (can be called repeatedly)
    void RevokeOpened() {
        if (!openedImg_) return;
        auto img = openedImg_;
        openedImg_ = nullptr;
        img.ImageOpened(openedTok_);
    }
    // The picture that has just been swapped in fades up quickly (it starts at 0.25: an element at opacity 0 may not be drawn at all, see above)
    void FadeInNew(Image const& img) {
        bool system = true;
        try { system = winrt::Windows::UI::ViewManagement::UISettings{}.AnimationsEnabled(); } catch (...) {}
        if (!req_.animations || !system) return;
        using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
        DoubleAnimation a;
        a.From(0.25);
        a.To(1.0);
        a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(160)));
        Storyboard::SetTarget(a, img);
        Storyboard::SetTargetProperty(a, L"Opacity");
        Storyboard sb;
        sb.Children().Append(a);
        sb.Completed([img, sb](auto&&, auto&&) { sb.Stop(); img.Opacity(1); });
        sb.Begin();
    }

    void Swap(uint32_t token, int back, uint32_t w, uint32_t h) {
        if (token != presentToken_ || closed_) return;
        presentToken_ = token + 1;  // swap only once (whichever comes first, ImageOpened or the safety timer)
        RevokeOpened();
        Image oldImg = Slot(front_);
        Image newImg = Slot(back);
        newImg.Opacity(1);
        oldImg.Opacity(0);
        oldImg.Source(nullptr);
        FadeInNew(newImg);
        front_ = back;
        StopWebp();
        anim_ = pendAnim_;
        animPaused_ = false;
        if (anim_ && !pendWebp_) anim_.Play();  // an animation in the cache may have stopped where it was paused last time: play from the start
        imgW_ = w;
        imgH_ = h;
        baseRot_ = pendBase_;
        mirror_ = pendMirror_;
        rot_ = 0;
        hasImage_ = true;
        EndLoading();
        scroll_.ChangeView(0.0, 0.0, nullptr, true);
        Reflow();
        if (infoVisible_) InfoAsync(pos_);
        if (pendWebp_) PlayWebp(pendWebp_, front_, ++webpGen_);
    }

    // ---- Menu (the "..." at the top right and the right click on the picture share the same content; each place builds its own, and updates the check marks when opened)
    MenuFlyout BuildMenu() {
        MenuFlyout m;
        auto weak = weak_from_this();
        auto item = [&](const wchar_t* text, const wchar_t* accel, std::function<void(PreviewWindow&)> act, const wchar_t* glyph = nullptr) {
            MenuFlyoutItem it;
            it.Text(text);
            if (accel && *accel) it.KeyboardAcceleratorTextOverride(accel);
            if (glyph) { FontIcon ic; ic.Glyph(glyph); it.Icon(ic); }   // the same symbols as the menus of the main window
            it.Click([weak, act](auto&&, auto&&) { if (auto s = weak.lock()) act(*s); });
            m.Items().Append(it);
            return it;
        };
        auto toggle = [&](const wchar_t* text, const wchar_t* accel, std::function<void(PreviewWindow&)> act) {
            ToggleMenuFlyoutItem it;
            it.Text(text);
            if (accel && *accel) it.KeyboardAcceleratorTextOverride(accel);
            it.Click([weak, act](auto&&, auto&&) { if (auto s = weak.lock()) act(*s); });
            m.Items().Append(it);
            return it;
        };
        auto sep = [&] { m.Items().Append(MenuFlyoutSeparator{}); };
        item(lycan::Tr(L"preview.previous_name"), L"←  Backspace", [](PreviewWindow& s) { s.Navigate(-1); }, L"\uE76B");
        item(lycan::Tr(L"preview.next_name"), L"→  Space", [](PreviewWindow& s) { s.Navigate(+1); }, L"\uE76C");
        item(lycan::Tr(L"preview.first"), L"Home", [](PreviewWindow& s) { s.GoTo(0); }, L"\uE892");
        item(lycan::Tr(L"preview.last"), L"End", [](PreviewWindow& s) { s.GoTo(s.req_.items.empty() ? 0 : s.req_.items.size() - 1); }, L"\uE893");
        sep();
        auto fitW = toggle(lycan::Tr(L"preview.fit_window"), L"Ctrl+0", [](PreviewWindow& s) { s.SetFit(0); });
        auto fitWd = toggle(lycan::Tr(L"preview.fit_width"), L"Ctrl+2", [](PreviewWindow& s) { s.SetFit(1); });
        auto fitH = toggle(lycan::Tr(L"preview.fit_height"), L"Ctrl+3", [](PreviewWindow& s) { s.SetFit(2); });
        auto actual = toggle(lycan::Tr(L"preview.actual_size_name"), L"Ctrl+1", [](PreviewWindow& s) { s.ActualSize(); });
        auto enlarge = toggle(lycan::Tr(L"main.enlarge_small_images_too"), L"", [](PreviewWindow& s) { s.ToggleEnlargeSmall(); });
        item(lycan::Tr(L"preview.zoom"), L"+", [](PreviewWindow& s) { s.ZoomBy(1.25); }, L"\uE8A3");
        item(lycan::Tr(L"preview.zoom_out"), L"−", [](PreviewWindow& s) { s.ZoomBy(0.8); }, L"\uE71F");
        item(lycan::Tr(L"preview.rotate_clockwise"), L"R", [](PreviewWindow& s) { s.Rotate(+1); }, L"\uE7AD");
        item(lycan::Tr(L"preview.rotate_counterclockwise"), L"Shift+R", [](PreviewWindow& s) { s.Rotate(-1); }, L"\uE7AD");
        auto full = toggle(lycan::Tr(L"preview.full_screen"), L"F11", [](PreviewWindow& s) { s.ToggleFullscreen(); });
        auto thumbs = toggle(lycan::Tr(L"preview.thumbnail_strip"), L"T", [](PreviewWindow& s) { s.ToggleThumbs(); });
        auto play = toggle(lycan::Tr(L"preview.play_animation"), L"P", [](PreviewWindow& s) { s.TogglePlay(); });
        sep();
        item(lycan::Tr(L"preview.copy_image"), L"Ctrl+C", [](PreviewWindow& s) { s.CopyImageAsync(); }, L"\uE8C8");
        item(lycan::Tr(L"main.copy_path"), L"Ctrl+Shift+C", [](PreviewWindow& s) { s.CopyPath(); }, L"\uE8C8");
        item(lycan::Tr(L"preview.save_image_as"), L"Ctrl+S", [](PreviewWindow& s) { s.ExtractCurrentAsync(); }, L"\uE74E");
        item(lycan::Tr(L"preview.set_wallpaper"), L"", [](PreviewWindow& s) { s.SetWallpaperAsync(); }, L"\uE7F4");
        auto openExt = item(lycan::Tr(L"main.open_with_associated_app"), L"Enter", [](PreviewWindow& s) { if (s.req_.openExternal && s.pos_ < s.req_.items.size()) s.req_.openExternal(s.req_.items[s.pos_].node); }, L"\uE8A7");
        sep();
        auto info = toggle(lycan::Tr(L"preview.image_info"), L"I", [](PreviewWindow& s) { s.ToggleInfo(); });
        sep();
        item(lycan::Tr(L"main.close"), L"Esc", [](PreviewWindow& s) { s.Close(); }, L"\uE711");
        m.Opening([weak, full, thumbs, info, play, fitW, fitWd, fitH, actual, enlarge, openExt](auto&&, auto&&) {
            if (auto s = weak.lock()) {
                openExt.IsEnabled(static_cast<bool>(s->req_.openExternal));   // a preview that was started from a file (no main window to hand the file to) cannot open it with the associated program
                full.IsChecked(s->fullscreen_);
                thumbs.IsChecked(!s->thumbsHidden_);
                info.IsChecked(s->infoVisible_);
                fitW.IsChecked(s->fitMode_ && s->fitKind_ == 0);
                fitWd.IsChecked(s->fitMode_ && s->fitKind_ == 1);
                fitH.IsChecked(s->fitMode_ && s->fitKind_ == 2);
                actual.IsChecked(!s->fitMode_ && std::abs(s->zoom_ - 1.0) < 1e-6);
                enlarge.IsChecked(s->req_.enlargeSmall);
                play.IsEnabled(static_cast<bool>(s->anim_));
                play.IsChecked(s->anim_ && !s->animPaused_);
            }
        });
        return m;
    }

    // ---- Picture information
    static std::wstring FileTimeText(uint64_t ft) {
        if (!ft) return L"";
        FILETIME f{static_cast<DWORD>(ft & 0xFFFFFFFF), static_cast<DWORD>(ft >> 32)};
        SYSTEMTIME utc{}, local{};
        if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"";
        wchar_t buf[40];
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
        return buf;
    }

    void InfoSection(const std::wstring& title) {
        TextBlock t;
        t.Text(hstring{title});
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        t.Margin(ThicknessHelper::FromLengths(0, infoList_.Children().Size() ? 12 : 0, 0, 2));
        infoList_.Children().Append(t);
    }
    void InfoRow(const std::wstring& label, const std::wstring& value) {
        if (value.empty()) return;
        Grid g;
        g.ColumnSpacing(10);
        ColumnDefinition c0, c1;
        c0.Width(GridLength{86, GridUnitType::Pixel});
        c1.Width(GridLength{1, GridUnitType::Star});
        g.ColumnDefinitions().Append(c0);
        g.ColumnDefinitions().Append(c1);
        TextBlock l;
        l.Text(hstring{label});
        l.Opacity(0.6);
        l.TextWrapping(TextWrapping::Wrap);
        TextBlock v;
        v.Text(hstring{value});
        v.TextWrapping(TextWrapping::Wrap);
        v.IsTextSelectionEnabled(true);
        Grid::SetColumn(v, 1);
        g.Children().Append(l);
        g.Children().Append(v);
        infoList_.Children().Append(g);
    }

    // Block of long text (prompts etc.): title + "Copy" + selectable text box
    void InfoBlock(const std::wstring& label, const std::wstring& text) {
        if (text.empty()) return;
        Grid head;
        ColumnDefinition c0, c1;
        c0.Width(GridLength{1, GridUnitType::Star});
        c1.Width(GridLength{0, GridUnitType::Auto});
        head.ColumnDefinitions().Append(c0);
        head.ColumnDefinitions().Append(c1);
        head.Margin(ThicknessHelper::FromLengths(0, 6, 0, 0));
        TextBlock l;
        l.Text(hstring{label});
        l.Opacity(0.6);
        l.VerticalAlignment(VerticalAlignment::Center);
        HyperlinkButton copy;
        copy.Content(box_value(lycan::Tr(L"preview.copy")));
        copy.Padding(ThicknessHelper::FromLengths(6, 0, 6, 0));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(copy, hstring{lycan::Tf(L"preview.copy_what", label)});
        copy.Click([text](auto&&, auto&&) {
            Windows::ApplicationModel::DataTransfer::DataPackage dp;
            dp.SetText(hstring{text});
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        });
        Grid::SetColumn(copy, 1);
        head.Children().Append(l);
        head.Children().Append(copy);
        infoList_.Children().Append(head);
        Border box;
        box.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        box.Padding(ThicknessHelper::FromLengths(8, 6, 8, 6));
        box.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x22, 0xFF, 0xFF, 0xFF}});
        TextBlock t;
        t.Text(hstring{text});
        t.TextWrapping(TextWrapping::Wrap);
        t.IsTextSelectionEnabled(true);
        box.Child(t);
        infoList_.Children().Append(box);
    }

    void ToggleInfo() {
        infoVisible_ = !infoVisible_;
        infoPanel_.Visibility(infoVisible_ ? Visibility::Visible : Visibility::Collapsed);
        bodyGrid_.ColumnDefinitions().GetAt(1).Width(GridLength{infoVisible_ ? 340.0 : 0.0, GridUnitType::Pixel});
        if (infoVisible_) InfoAsync(pos_);
    }

    fire_and_forget InfoAsync(size_t pos) {
        auto self = shared_from_this();
        const uint32_t gen = ++infoGen_;
        if (pos >= req_.items.size()) co_return;
        const PreviewItem item = req_.items[pos];

        // Image information (size, format, resolution, colour) and camera information (EXIF): reads the headers from the bytes in the cache, no need to read the archive again
        std::wstring format, colour, dpi;
        uint32_t w = 0, h = 0;
        std::vector<std::pair<std::wstring, std::wstring>> exif;
        std::wstring userComment;
        std::shared_ptr<std::vector<uint8_t>> bytes;
        if (auto it = cache_.find(pos); it != cache_.end()) bytes = it->second.bytes;
        if (bytes) {
            try {
                Windows::Storage::Streams::InMemoryRandomAccessStream ras;
                {
                    Windows::Storage::Streams::DataWriter dw(ras);
                    dw.WriteBytes(array_view<const uint8_t>(bytes->data(), bytes->data() + bytes->size()));
                    co_await dw.StoreAsync();
                    dw.DetachStream();
                }
                ras.Seek(0);
                auto dec = co_await Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(ras);
                w = dec.PixelWidth();
                h = dec.PixelHeight();
                format = std::wstring(dec.DecoderInformation().FriendlyName());
                if (auto p = format.rfind(L" Decoder"); p != std::wstring::npos) format.erase(p);
                if (dec.DpiX() > 1 && dec.DpiY() > 1) {
                    wchar_t b[48];
                    swprintf_s(b, L"%.0f × %.0f DPI", dec.DpiX(), dec.DpiY());
                    dpi = b;
                }
                using Windows::Graphics::Imaging::BitmapPixelFormat;
                using Windows::Graphics::Imaging::BitmapAlphaMode;
                switch (dec.BitmapPixelFormat()) {
                    case BitmapPixelFormat::Gray8: case BitmapPixelFormat::Gray16: colour = lycan::Tr(L"preview.grayscale"); break;
                    case BitmapPixelFormat::Rgba16: colour = lycan::Tr(L"preview.16_bits_channel"); break;
                    default: colour = lycan::Tr(L"preview.color_8_bits_channel"); break;
                }
                if (dec.BitmapAlphaMode() != BitmapAlphaMode::Ignore) colour += lycan::Tr(L"preview.with_transparency");
                try {
                    auto keys = single_threaded_vector<hstring>({L"System.Photo.DateTaken", L"System.Photo.CameraManufacturer", L"System.Photo.CameraModel",
                                                                 L"System.Photo.FNumber", L"System.Photo.ExposureTime", L"System.Photo.ISOSpeed",
                                                                 L"System.Photo.FocalLength"});
                    auto props = co_await dec.BitmapProperties().GetPropertiesAsync(keys);
                    auto get = [&](const wchar_t* k) -> Windows::Foundation::IInspectable {
                        if (props.HasKey(k)) return props.Lookup(k).Value();
                        return nullptr;
                    };
                    auto asDouble = [](Windows::Foundation::IInspectable const& v, double& out) {
                        if (!v) return false;
                        if (auto r = v.try_as<Windows::Foundation::IReference<double>>()) { out = r.Value(); return true; }
                        if (auto r = v.try_as<Windows::Foundation::IReference<float>>()) { out = r.Value(); return true; }
                        if (auto r = v.try_as<Windows::Foundation::IReference<uint32_t>>()) { out = r.Value(); return true; }
                        if (auto r = v.try_as<Windows::Foundation::IReference<uint16_t>>()) { out = r.Value(); return true; }
                        return false;
                    };
                    if (auto v = get(L"System.Photo.DateTaken"))
                        if (auto d = v.try_as<Windows::Foundation::IReference<Windows::Foundation::DateTime>>())
                            exif.push_back({lycan::Tr(L"preview.date_taken"), FileTimeText(static_cast<uint64_t>(d.Value().time_since_epoch().count()))});
                    std::wstring cam;
                    if (auto v = get(L"System.Photo.CameraManufacturer")) if (auto s = v.try_as<Windows::Foundation::IReference<hstring>>()) cam = std::wstring(s.Value());
                    if (auto v = get(L"System.Photo.CameraModel")) if (auto s = v.try_as<Windows::Foundation::IReference<hstring>>()) cam += (cam.empty() ? L"" : L" ") + std::wstring(s.Value());
                    if (!cam.empty()) exif.push_back({lycan::Tr(L"preview.camera"), cam});
                    double x = 0;
                    wchar_t b[48];
                    if (asDouble(get(L"System.Photo.FNumber"), x)) { swprintf_s(b, L"f/%.1f", x); exif.push_back({lycan::Tr(L"preview.aperture"), b}); }
                    if (asDouble(get(L"System.Photo.ExposureTime"), x) && x > 0) {
                        {
                            std::wstring t;
                            if (x < 1) t = lycan::Tf(L"preview.1_s", static_cast<int>(std::lround(1.0 / x)));
                            else { wchar_t n[16]; swprintf_s(n, L"%.1f", x); t = lycan::Tf(L"preview.s", n); }
                            wcsncpy_s(b, t.c_str(), _TRUNCATE);
                        }
                        exif.push_back({lycan::Tr(L"preview.shutter"), b});
                    }
                    if (asDouble(get(L"System.Photo.ISOSpeed"), x)) { swprintf_s(b, L"ISO %d", static_cast<int>(x)); exif.push_back({lycan::Tr(L"preview.iso"), b}); }
                    if (asDouble(get(L"System.Photo.FocalLength"), x)) { swprintf_s(b, L"%.0f mm", x); exif.push_back({lycan::Tr(L"preview.focal_length"), b}); }
                } catch (...) {}  // no EXIF (PNG etc.)
                try {  // JPEG / WebP: EXIF UserComment (Stable Diffusion WebUI puts the prompt here)
                    auto cprops = co_await dec.BitmapProperties().GetPropertiesAsync(single_threaded_vector<hstring>({L"/app1/ifd/exif/{ushort=37510}"}));
                    if (cprops.HasKey(L"/app1/ifd/exif/{ushort=37510}"))
                        if (auto arr = cprops.Lookup(L"/app1/ifd/exif/{ushort=37510}").Value().try_as<Windows::Foundation::IReferenceArray<uint8_t>>()) {
                            auto a = arr.Value();
                            userComment = DecodeUserComment(a.data(), a.size());
                        }
                } catch (...) {}
            } catch (...) {}
        }
        if (gen != infoGen_.load() || !infoVisible_ || closed_) co_return;
        AiMeta ai;
        if (bytes) ai = ParseAiMeta(*bytes, userComment);

        infoList_.Children().Clear();
        InfoSection(lycan::Tr(L"preview.file"));
        InfoRow(lycan::Tr(L"main.name"), item.name);
        InfoRow(lycan::Tr(L"preview.path_archive"), item.path);
        InfoRow(lycan::Tr(L"main.size"), lycan::Tf(L"fmt.size_bytes", SizeText(item.size), item.size));
        if (item.packedSize) InfoRow(lycan::Tr(L"main.packed"), SizeText(item.packedSize));
        InfoRow(lycan::Tr(L"main.modified"), FileTimeText(item.mtimeFileTime));
        if (w && h) {
            InfoSection(lycan::Tr(L"preview.image"));
            wchar_t mp[32];
            swprintf_s(mp, L"%.1f", static_cast<double>(w) * h / 1e6);
            InfoRow(lycan::Tr(L"preview.dimensions"), lycan::Tf(L"preview.px_megapixels", w, h, mp));
            InfoRow(lycan::Tr(L"main.format"), format);
            InfoRow(lycan::Tr(L"preview.color"), colour);
            InfoRow(lycan::Tr(L"preview.resolution"), dpi);
        } else if (!bytes) {
            InfoSection(lycan::Tr(L"preview.image"));
            InfoRow(lycan::Tr(L"preview.status"), lycan::Tr(L"preview.not_fully_loaded_yet"));
        }
        if (!exif.empty()) {
            InfoSection(lycan::Tr(L"preview.photo_info"));
            for (auto const& [k, v] : exif) InfoRow(k, v);
        }
        if (ai.found) {
            InfoSection(lycan::Tr(L"preview.ai_generation_info"));
            InfoRow(lycan::Tr(L"preview.source"), ai.source);
            InfoBlock(lycan::Tr(L"preview.prompt"), ai.positive);
            InfoBlock(lycan::Tr(L"preview.negative_prompt"), ai.negative);
            for (auto const& [k, v] : ai.params) InfoRow(k, v);
            InfoBlock(lycan::Tr(L"preview.raw_data"), ai.raw);
            if (!ai.other.empty()) {
                InfoSection(lycan::Tr(L"preview.other_text_data"));
                for (auto const& [k, v] : ai.other) InfoRow(k, v);
            }
        }
    }

    // ---- Copy / extract this picture
    fire_and_forget CopyImageAsync() {
        auto self = shared_from_this();
        auto it = cache_.find(pos_);
        if (it == cache_.end() || !it->second.bytes) co_return;
        auto bytes = it->second.bytes;
        Windows::Storage::Streams::InMemoryRandomAccessStream ras;
        {
            Windows::Storage::Streams::DataWriter dw(ras);
            dw.WriteBytes(array_view<const uint8_t>(bytes->data(), bytes->data() + bytes->size()));
            co_await dw.StoreAsync();
            dw.DetachStream();
        }
        ras.Seek(0);
        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetBitmap(Windows::Storage::Streams::RandomAccessStreamReference::CreateFromStream(ras));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        Flash(lycan::Tr(L"preview.image_copied"));
    }
    void CopyPath() {
        if (pos_ >= req_.items.size()) return;
        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(hstring{req_.items[pos_].path});
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        Flash(lycan::Tr(L"preview.path_copied"));
    }
    fire_and_forget ExtractCurrentAsync() {
        auto self = shared_from_this();
        if (pos_ >= req_.items.size()) co_return;
        const PreviewItem item = req_.items[pos_];
        const auto archive = req_.archive;
        const auto open = req_.open;
        // "Save as" for this one image: the user can change the file name (and the folder) and the format: the original file as it is, or the
        // picture in another format that this computer can write
        std::wstring ext = std::filesystem::path(item.name).extension().wstring();
        if (!ext.empty()) ext.erase(0, 1);
        const std::wstring origExt = SaveExt(ext);
        std::vector<std::pair<std::wstring, std::wstring>> types;
        types.push_back({std::wstring{lycan::Tf(L"preview.save_as_original", ext)}, ext});
        for (auto const& e : Encoders())
            if (e.ext != origExt) types.push_back({std::wstring{lycan::Tf(L"preview.save_as_type", e.label)}, e.ext});
        // (LYCAN_TEST_SAVE_IMAGE=<file>: tests do not open the dialog; the extension of the file chooses the format)
        const wchar_t* testDest = _wgetenv(L"LYCAN_TEST_SAVE_IMAGE");
        const std::filesystem::path dest = testDest && *testDest ? std::filesystem::path(testDest)
                                                                 : std::filesystem::path{std::wstring(co_await lycan_app::PickSaveFileAsync(win_.AppWindow().Id(), lycan_app::kPurposeSaveImage,
                                                                                            archive.parent_path(), item.name, types, lycan::Tr(L"preview.save_dialog_title")))};
        if (dest.empty() || closed_) co_return;
        co_await resume_background();
        bool ok = false;
        try {
            auto src = lycan::ArchiveSource::Open(archive, open);
            // written next to the target under a temporary name and moved over it, so a failure never damages an existing file
            std::filesystem::path tmp = dest;
            tmp += L".lycantmp";
            struct FileSink : lycan::ByteSink {
                HANDLE h = INVALID_HANDLE_VALUE;
                void Write(const void* d, size_t n) override {
                    auto p = static_cast<const char*>(d);
                    while (n) {
                        DWORD w = 0;
                        if (!WriteFile(h, p, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &w, nullptr) || !w) throw lycan::Error(HRESULT_FROM_WIN32(GetLastError()), "write failed");
                        p += w;
                        n -= w;
                    }
                }
            } sink;
            sink.h = CreateFileW(lycan::ToLongPath(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (sink.h == INVALID_HANDLE_VALUE) throw lycan::Error(HRESULT_FROM_WIN32(GetLastError()), "cannot create file");
            try {
                const std::wstring destExt = SaveExt(dest.extension().wstring());
                const EncoderChoice* target = nullptr;
                if (destExt != origExt)
                    for (auto const& e : Encoders())
                        if (e.ext == destExt) target = &e;
                if (!target) {
                    src->ReadEntry(item.entryIndex, sink);   // the same format: the file as it is
                } else {
                    std::vector<uint8_t> bytes;
                    VecSink vs(bytes, 256ull << 20);
                    src->ReadEntry(item.entryIndex, vs);
                    const auto converted = ConvertImageAsync(std::move(bytes), target->id, target->ext == L"jpg" || target->ext == L"bmp").get();
                    sink.Write(converted.data(), converted.Length());
                }
            } catch (...) {
                CloseHandle(sink.h);
                DeleteFileW(lycan::ToLongPath(tmp).c_str());
                throw;
            }
            if (item.mtimeFileTime) {
                FILETIME ft{static_cast<DWORD>(item.mtimeFileTime & 0xFFFFFFFF), static_cast<DWORD>(item.mtimeFileTime >> 32)};
                SetFileTime(sink.h, nullptr, nullptr, &ft);
            }
            CloseHandle(sink.h);
            ok = MoveFileExW(lycan::ToLongPath(tmp).c_str(), lycan::ToLongPath(dest).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) DeleteFileW(lycan::ToLongPath(tmp).c_str());
            // keep the "downloaded from the internet" mark of the archive on the saved image (like extracting does)
            if (ok) {
                HANDLE z = CreateFileW((lycan::ToLongPath(archive).wstring() + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (z != INVALID_HANDLE_VALUE) {
                    char buf[4096];
                    DWORD n = 0;
                    if (ReadFile(z, buf, sizeof buf, &n, nullptr) && n) {
                        HANDLE o = CreateFileW((lycan::ToLongPath(dest).wstring() + L":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (o != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(o, buf, n, &w, nullptr); CloseHandle(o); }
                    }
                    CloseHandle(z);
                }
            }
        } catch (...) {}
        co_await resume_ui{dq_};
        if (!closed_) Flash(ok ? lycan::Tf(L"preview.saved", dest.wstring()) : std::wstring(lycan::Tr(L"preview.could_not_save")));
    }

    // "Set as desktop picture": asks once (it changes a setting of the system), saves a copy of the picture in the Pictures folder (so that it
    // stays when the archive is closed) and gives it to Windows (IDesktopWallpaper).
    fire_and_forget SetWallpaperAsync() {
        auto self = shared_from_this();
        if (pos_ >= req_.items.size() || !root_) co_return;
        const PreviewItem item = req_.items[pos_];
        const auto archive = req_.archive;
        const auto open = req_.open;
        // (LYCAN_TEST_WALLPAPER_DIR=<folder>: tests keep the picture there and do not touch the desktop picture of the machine)
        const wchar_t* testDir = _wgetenv(L"LYCAN_TEST_WALLPAPER_DIR");
        const std::filesystem::path folder = testDir && *testDir ? std::filesystem::path(testDir) : WallpaperFolder();

        // the screens
        std::vector<std::wstring> monitors;
        try {
            winrt::com_ptr<IDesktopWallpaper> dw = winrt::create_instance<IDesktopWallpaper>(CLSID_DesktopWallpaper, CLSCTX_LOCAL_SERVER);
            UINT n = 0;
            if (SUCCEEDED(dw->GetMonitorDevicePathCount(&n)))
                for (UINT i = 0; i < n && i < 16; ++i) {
                    PWSTR id = nullptr;
                    if (SUCCEEDED(dw->GetMonitorDevicePathAt(i, &id)) && id) monitors.push_back(id);
                    CoTaskMemFree(id);
                }
        } catch (...) {}

        using namespace Microsoft::UI::Xaml::Controls;
        ContentDialog dlg;
        dlg.XamlRoot(root_.XamlRoot());
        dlg.RequestedTheme(root_.ActualTheme());
        dlg.Title(box_value(lycan::Tr(L"preview.wallpaper_title")));
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(380);
        TextBlock hint;
        hint.Text(hstring{lycan::Tf(L"preview.wallpaper_hint", folder.wstring())});
        hint.TextWrapping(TextWrapping::Wrap);
        hint.Opacity(0.8);
        sp.Children().Append(hint);
        ComboBox pos;
        pos.Header(box_value(lycan::Tr(L"preview.wallpaper_position")));
        for (const wchar_t* id : {L"preview.wallpaper_fill", L"preview.wallpaper_fit", L"preview.wallpaper_stretch", L"preview.wallpaper_tile",
                                  L"preview.wallpaper_center", L"preview.wallpaper_span"})
            pos.Items().Append(box_value(lycan::Tr(id)));
        pos.SelectedIndex(0);
        pos.HorizontalAlignment(HorizontalAlignment::Stretch);
        sp.Children().Append(pos);
        ComboBox screen;
        screen.Header(box_value(lycan::Tr(L"preview.wallpaper_screen")));
        screen.Items().Append(box_value(lycan::Tr(L"preview.wallpaper_all_screens")));
        for (size_t i = 0; i < monitors.size() && monitors.size() > 1; ++i) screen.Items().Append(box_value(lycan::Tf(L"preview.wallpaper_screen_n", i + 1)));
        screen.SelectedIndex(0);
        screen.HorizontalAlignment(HorizontalAlignment::Stretch);
        if (monitors.size() > 1) sp.Children().Append(screen);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"preview.wallpaper_set"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        if (co_await dlg.ShowAsync() != ContentDialogResult::Primary || closed_) co_return;
        static const DESKTOP_WALLPAPER_POSITION kPos[] = {DWPOS_FILL, DWPOS_FIT, DWPOS_STRETCH, DWPOS_TILE, DWPOS_CENTER, DWPOS_SPAN};
        const DESKTOP_WALLPAPER_POSITION position = kPos[std::clamp(pos.SelectedIndex(), 0, 5)];
        const int screenIndex = monitors.size() > 1 ? screen.SelectedIndex() - 1 : -1;   // -1 = all
        const std::wstring monitorId = screenIndex >= 0 && static_cast<size_t>(screenIndex) < monitors.size() ? monitors[static_cast<size_t>(screenIndex)] : std::wstring{};

        co_await resume_background();
        bool ok = false;
        try {
            if (folder.empty()) throw lycan::Error(E_FAIL, std::string("no pictures folder"));
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            auto src = lycan::ArchiveSource::Open(archive, open);
            std::vector<uint8_t> bytes;
            VecSink vs(bytes, 256ull << 20);
            src->ReadEntry(item.entryIndex, vs);
            // Windows takes JPEG, PNG and BMP: everything else goes in as PNG
            std::wstring ext = SaveExt(std::filesystem::path(item.name).extension().wstring());
            if (ext != L"jpg" && ext != L"png" && ext != L"bmp") {
                bytes.shrink_to_fit();
                const auto converted = ConvertImageAsync(std::move(bytes), Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), false).get();
                bytes.assign(converted.data(), converted.data() + converted.Length());
                ext = L"png";
            }
            std::wstring stem = std::filesystem::path(item.name).stem().wstring();
            for (auto& c : stem) if (wcschr(L"\\/:*?\"<>|", c) || c < 0x20) c = L'_';
            if (stem.size() > 60) stem.resize(60);
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t stamp[32];
            swprintf_s(stamp, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            const std::filesystem::path file = folder / (L"wallpaper-" + std::wstring(stamp) + L"-" + stem + L"." + ext);
            HANDLE h = CreateFileW(lycan::ToLongPath(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) throw lycan::Error(HRESULT_FROM_WIN32(GetLastError()), std::string("cannot create file"));
            DWORD w = 0;
            const BOOL wrote = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr);
            CloseHandle(h);
            if (!wrote || w != bytes.size()) throw lycan::Error(E_FAIL, std::string("cannot write file"));
            // the pictures of earlier times: only the newest ones are kept (only files that this feature made)
            std::vector<std::filesystem::path> old;
            for (auto const& e : std::filesystem::directory_iterator(folder, ec))
                if (e.path().filename().wstring().rfind(L"wallpaper-", 0) == 0) old.push_back(e.path());
            std::sort(old.begin(), old.end());
            for (size_t i = 0; old.size() > 8 && i + 8 < old.size(); ++i) std::filesystem::remove(old[i], ec);

            const wchar_t* noApply = _wgetenv(L"LYCAN_TEST_WALLPAPER_NOAPPLY");   // the real folder, but the desktop picture is not changed
            if ((testDir && *testDir) || (noApply && *noApply)) {
                lycan::diag::Info(L"desktop picture (test, not set): {} position {}", file.wstring(), static_cast<int>(position));
            } else {
                winrt::com_ptr<IDesktopWallpaper> dw = winrt::create_instance<IDesktopWallpaper>(CLSID_DesktopWallpaper, CLSCTX_LOCAL_SERVER);
                winrt::check_hresult(dw->SetPosition(position));
                winrt::check_hresult(dw->SetWallpaper(monitorId.empty() ? nullptr : monitorId.c_str(), file.c_str()));
            }
            ok = true;
        } catch (winrt::hresult_error const& e) {
            lycan::diag::Warn(L"set as desktop picture failed: {} (0x{:08x})", std::wstring(e.message()), static_cast<uint32_t>(e.code().value));
        } catch (lycan::Error const& e) {
            lycan::diag::Warn(L"set as desktop picture failed: {} (0x{:08x})", e.message().English(), static_cast<uint32_t>(e.hr()));
        } catch (std::exception const& e) {
            lycan::diag::Warn(L"set as desktop picture failed: {}", lycan::diag::FromUtf8(e.what()));
        } catch (...) {
            lycan::diag::Warn(L"set as desktop picture failed");
        }
        co_await resume_ui{dq_};
        if (!closed_) Flash(ok ? std::wstring{lycan::Tr(L"preview.wallpaper_done")} : std::wstring{lycan::Tr(L"preview.wallpaper_failed")});
    }

    // ---- Cache (accessed on the UI thread only)
    uint64_t CacheCost() const {
        uint64_t c = 0;
        for (auto const& [k, v] : cache_) c += v.Cost();
        return c;
    }
    // The read-ahead limit of the setting is the total of the whole process, shared equally among the open preview windows (each gets at least kMinPrefetchShare; if the total is smaller the total rules)
    uint64_t PrefetchBudget() const {
        const uint64_t total = req_.prefetchBytes;
        if (total == 0) return 0;
        return std::max<uint64_t>(std::min<uint64_t>(total, kMinPrefetchShare), total / static_cast<uint64_t>(std::max(1, LivePreviewCount())));
    }
    // When over budget, drop the ones farthest from the current picture (the current one is never dropped)
    void Evict() {
        while (cache_.size() > 1 && CacheCost() > std::max<uint64_t>(PrefetchBudget(), 1)) {
            size_t farKey = pos_;
            int64_t best = -1;
            for (auto const& [k, v] : cache_) {
                if (k == pos_) continue;
                const int64_t d = std::llabs(static_cast<int64_t>(k) - static_cast<int64_t>(pos_));
                if (d > best) { best = d; farKey = k; }
            }
            if (best < 0) break;
            cache_.erase(farKey);
        }
    }

    // ---- The current picture
    fire_and_forget LoadAsync(size_t pos) {
        auto self = shared_from_this();
        const uint32_t gen = ++gen_;
        const uint32_t session = session_;
        if (pos >= req_.items.size()) co_return;
        const PreviewItem item = req_.items[pos];

        // the title has the archive name: with previews of several main windows it tells which preview belongs to which archive
        win_.Title(hstring{req_.archive.empty() ? item.name : item.name + L" — " + req_.archive.filename().wstring()});
        name_.Text(hstring{item.name});
        counter_.Text(hstring{std::to_wstring(pos + 1) + L" / " + std::to_wstring(req_.items.size())});
        prev_.IsEnabled(req_.items.size() > 1);
        next_.IsEnabled(req_.items.size() > 1);
        msg_.Visibility(Visibility::Collapsed);

        // in the cache: switch to it at once (without flicker)
        if (auto it = cache_.find(pos); it != cache_.end() && it->second.dec.image) {
            Present(it->second.dec, item);
            SchedulePrefetch();
            co_return;
        }
        BeginLoading(gen);  // the old picture stays; only after 250 ms it is dimmed and a ring is shown

        if (item.size > kMaxBytes) {
            ShowMessage(lycan::Tf(L"preview.file_too_large_preview_limit_64", SizeText(item.size)));
            co_return;
        }

        std::shared_ptr<std::vector<uint8_t>> bytes;
        if (auto it = cache_.find(pos); it != cache_.end()) bytes = it->second.bytes;
        double ms = 0;
        if (!bytes) {
            // Background: read this one item only
            const auto t0 = std::chrono::steady_clock::now();
            co_await resume_background();
            auto data = std::make_shared<std::vector<uint8_t>>();
            data->reserve(static_cast<size_t>(std::min<uint64_t>(item.size, kMaxBytes)));
            std::wstring error;
            const bool ok = readerA_.Read(item.entryIndex, kMaxBytes, *data, [self, gen] { return self->gen_.load() == gen; }, error);
            co_await resume_ui{dq_};
            if (gen != gen_.load() || closed_ || session != session_) co_return;
            if (!ok) { if (!error.empty()) ShowMessage(error); co_return; }
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            bytes = data;
            cache_[pos].bytes = bytes;
            OnBytes(pos);
        }
        lastLoadMs_ = ms;

        try {
            Decoded d;
            co_await DecodeInto(bytes, kMaxDecodeWidth, d, true);
            if (gen != gen_.load() || closed_ || session != session_) co_return;
            cache_[pos].dec = d;
            Evict();
            Present(d, item);
            SchedulePrefetch();
        } catch (hresult_error const& e) {
            if (gen == gen_.load() && !closed_)
                ShowMessage(e.code() == E_FAIL ? lycan::Tr(L"preview.image_too_large_exceeds_preview")
                            : !IsImageDecoderAvailable(item.name) ? lycan::Tf(L"preview.use_open_with_associated_app_file", MissingDecoderHint(item.name))
                                                                  : lycan::Tr(L"preview.cannot_display_image_file_may_be"));
        }
    }

    // ---- Read-ahead: leans to the direction of paging, up to 4 before and 2 after; one at a time, and a new page turn replaces it and stops it
    void SchedulePrefetch() {
        if (PrefetchBudget() == 0 || lastLoadMs_ > kSlowLoadMs || req_.items.size() < 2) return;
        const uint32_t epoch = ++prefetchEpoch_;
        PrefetchAsync(epoch, session_, pos_, dir_);
    }

    fire_and_forget PrefetchAsync(uint32_t epoch, uint32_t session, size_t center, int dir) {
        auto self = shared_from_this();
        std::vector<size_t> order;
        const int64_t n = static_cast<int64_t>(req_.items.size());
        auto at = [&](int64_t i) { return static_cast<size_t>(((i % n) + n) % n); };
        for (int k = 1; k <= 4; ++k) order.push_back(at(static_cast<int64_t>(center) + dir * k));
        for (int k = 1; k <= 2; ++k) order.push_back(at(static_cast<int64_t>(center) - dir * k));
        for (size_t idx : order) {
            if (epoch != prefetchEpoch_ || session != session_ || closed_) co_return;
            if (idx == center) continue;
            if (auto it = cache_.find(idx); it != cache_.end() && it->second.dec.image) continue;
            const PreviewItem item = req_.items[idx];
            if (item.size > kMaxBytes) continue;
            std::shared_ptr<std::vector<uint8_t>> bytes;
            if (auto it = cache_.find(idx); it != cache_.end()) bytes = it->second.bytes;
            if (!bytes) {
                co_await resume_background();
                auto data = std::make_shared<std::vector<uint8_t>>();
                std::wstring error;
                const bool ok = readerB_.Read(item.entryIndex, kMaxBytes, *data, [self, epoch] { return self->prefetchEpoch_.load() == epoch; }, error);
                co_await resume_ui{dq_};
                if (epoch != prefetchEpoch_ || session != session_ || closed_) co_return;
                if (!ok) continue;
                bytes = data;
                cache_[idx].bytes = bytes;
                OnBytes(idx);
            }
            try {
                Decoded d;
                co_await DecodeInto(bytes, kMaxDecodeWidth, d, true);
                if (epoch != prefetchEpoch_ || session != session_ || closed_) co_return;
                cache_[idx].dec = d;
                Evict();
            } catch (hresult_error const&) {
                // a picture that cannot be decoded is not cached as decoded; the error is shown when it is reached
            }
        }
    }

    // ---- Thumbnail strip
    void OnTile(ContainerContentChangingEventArgs const& a) {
        const size_t idx = static_cast<size_t>(a.ItemIndex());
        if (a.InRecycleQueue()) { wanted_.erase(idx); return; }
        auto root = a.ItemContainer().ContentTemplateRoot().try_as<FrameworkElement>();
        if (!root || idx >= req_.items.size()) return;
        if (auto cap = root.FindName(L"Cap").try_as<TextBlock>()) cap.Text(hstring{req_.items[idx].name});
        auto img = root.FindName(L"Thumb").try_as<Image>();
        auto ph = root.FindName(L"Ph").try_as<UIElement>();
        if (auto it = thumbs_.find(idx); it != thumbs_.end()) {
            if (img) img.Source(it->second);
            if (ph) ph.Visibility(Visibility::Collapsed);
        } else {
            if (img) img.Source(nullptr);
            if (ph) ph.Visibility(Visibility::Visible);
            wanted_.insert(idx);
            ThumbLoop();
        }
    }

    // The bytes of a picture are there already (seen / read ahead): the thumbnail can be made from them, no need to read the archive again
    void OnBytes(size_t idx) {
        if (thumbs_.count(idx) || !wanted_.count(idx)) return;
        ThumbLoop();
    }

    void ApplyThumb(size_t idx, Microsoft::UI::Xaml::Media::Imaging::BitmapImage const& bi) {
        thumbs_[idx] = bi;
        wanted_.erase(idx);
        if (auto c = strip_.ContainerFromIndex(static_cast<int32_t>(idx)).try_as<ListViewItem>())
            if (auto root = c.ContentTemplateRoot().try_as<FrameworkElement>()) {
                if (auto img = root.FindName(L"Thumb").try_as<Image>()) img.Source(bi);
                if (auto ph = root.FindName(L"Ph").try_as<UIElement>()) ph.Visibility(Visibility::Collapsed);
            }
    }

    // A single worker: each time it picks the item nearest to the current picture that has no thumbnail yet. A solid archive (thumbReads=false) uses only the bytes already in the cache.
    fire_and_forget ThumbLoop() {
        if (thumbRunning_) co_return;
        thumbRunning_ = true;
        auto self = shared_from_this();
        const uint32_t session = session_;
        for (;;) {
            if (closed_ || session != session_) break;
            // pick the nearest
            size_t pick = SIZE_MAX;
            int64_t best = INT64_MAX;
            for (size_t idx : wanted_) {
                if (thumbs_.count(idx)) continue;
                const bool haveBytes = cache_.count(idx) && cache_[idx].bytes;
                if (!haveBytes && (!req_.thumbReads || req_.items[idx].size > kMaxThumbSourceBytes)) continue;
                const int64_t d = std::llabs(static_cast<int64_t>(idx) - static_cast<int64_t>(pos_));
                if (d < best) { best = d; pick = idx; }
            }
            if (pick == SIZE_MAX) break;

            std::shared_ptr<std::vector<uint8_t>> bytes;
            if (auto it = cache_.find(pick); it != cache_.end()) bytes = it->second.bytes;
            if (!bytes) {
                const PreviewItem item = req_.items[pick];
                co_await resume_background();
                auto data = std::make_shared<std::vector<uint8_t>>();
                std::wstring error;
                const bool ok = readerC_.Read(item.entryIndex, kMaxThumbSourceBytes, *data, [self, session] { return self->session_.load() == session && !self->closed_; }, error);
                co_await resume_ui{dq_};
                if (closed_ || session != session_) break;
                if (!ok) { wanted_.erase(pick); continue; }
                bytes = data;
            }
            try {
                Decoded d;
                co_await DecodeInto(bytes, kThumbWidth, d);
                if (closed_ || session != session_) break;
                ApplyThumb(pick, d.image);
            } catch (hresult_error const&) {
                wanted_.erase(pick);  // cannot be decoded: keep the placeholder icon
            }
        }
        thumbRunning_ = false;
    }

    Microsoft::UI::Dispatching::DispatcherQueue dq_{nullptr};
    Window win_{nullptr};
    Grid root_{nullptr}, host_{nullptr}, bar_{nullptr};
    ScrollViewer scroll_{nullptr};
    ListView strip_{nullptr};
    int pressedTile_ = -1;   // the tile of the strip under the pointer when the button went down
    Image img0_{nullptr}, img1_{nullptr};
    Button menuBtn_{nullptr};
    Border infoPanel_{nullptr};
    StackPanel infoList_{nullptr};
    Grid bodyGrid_{nullptr};
    std::atomic<uint32_t> infoGen_{0};
    bool infoVisible_ = false;
    Grid wrapper_{nullptr};
    Border statusBar_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer loadTimer_{nullptr}, swapTimer_{nullptr}, flashTimer_{nullptr};
    std::wstring statusBase_;
    double zoom_ = 1.0, panOffX_ = 0, panOffY_ = 0;
    int fitKind_ = 0;  // fit mode: 0 window / 1 width / 2 height
    int front_ = 0, rot_ = 0, baseRot_ = 0, pendBase_ = 0;
    bool mirror_ = false, pendMirror_ = false, animPaused_ = false, webpPlaying_ = false;
    std::shared_ptr<std::vector<uint8_t>> pendWebp_;
    std::vector<Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource> webpFrames_;
    std::vector<int> webpDur_;
    size_t webpIdx_ = 0;
    int webpSlot_ = 0;
    uint32_t webpGen_ = 0;
    Microsoft::UI::Dispatching::DispatcherQueueTimer webpTimer_{nullptr};
    Microsoft::UI::Xaml::Media::Imaging::BitmapImage anim_{nullptr}, pendAnim_{nullptr};  // the animation shown now / being prepared (nullptr for a non-animated picture)
    Microsoft::UI::Dispatching::DispatcherQueueTimer activateTimer_{nullptr};
    uint32_t presentToken_ = 0, loadGen_ = 0, pendToken_ = 0, pendW_ = 0, pendH_ = 0;
    Image openedImg_{nullptr};  // the Image that still has an ImageOpened handler attached (registered in Present, removed in Swap / Close)
    event_token openedTok_{};
    int pendBack_ = 0;
    bool fullscreen_ = false, thumbsHidden_ = false, panning_ = false, loading_ = false;
    Windows::Foundation::Point panStart_{};
    ProgressRing ring_{nullptr};
    Border toast_{nullptr};   // a short note at the bottom of the picture ("this is the first picture")
    TextBlock toastText_{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer toastTimer_{nullptr};
    TextBlock msg_{nullptr}, name_{nullptr}, counter_{nullptr}, status_{nullptr};
    Button prev_{nullptr}, next_{nullptr};
    Primitives::ToggleButton fit_{nullptr};
    PreviewRequest req_;
    size_t pos_ = 0;
    int dir_ = +1;
    Reader readerA_, readerB_, readerC_;
    std::map<size_t, Cached> cache_;
    std::map<size_t, Microsoft::UI::Xaml::Media::Imaging::BitmapImage> thumbs_;
    std::set<size_t> wanted_;
    std::atomic<uint32_t> gen_{0}, prefetchEpoch_{0}, session_{0};
    bool closed_ = false, hasImage_ = false, fitMode_ = true, syncingStrip_ = false, thumbRunning_ = false;
  public:
    const void* owner_ = nullptr;  // the main window that made the request (several windows: closing a main window closes only its own preview)
  private:
    double lastLoadMs_ = 0;
    int wheelAcc_ = 0;
    uint32_t imgW_ = 0, imgH_ = 0;
};

// Each main window (owner) has one preview window: previewing again from the same main window reuses it, another main window has its own (several windows, can be compared side by side)
std::map<const void*, std::shared_ptr<PreviewWindow>> g_windows;

int LivePreviewCount() {
    int n = 0;
    for (auto const& [k, w] : g_windows)
        if (!w->IsClosed()) ++n;
    return n;
}

void PreviewWindow::OnClosed() {
    closed_ = true;
    if (auto done = std::move(req_.onClosed)) done();
    RevokeOpened();
    ReleaseImages();
    ++gen_;
    ++session_;
    ++prefetchEpoch_;
    // A window cannot be destroyed in the middle of its own event handler: queue it for the next round and remove it from the set (only itself; there may be a new preview of the same owner that replaced it already)
    auto dq = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    dq.TryEnqueue([key = owner_, self = this] {
        auto it = g_windows.find(key);
        if (it != g_windows.end() && it->second.get() == self) g_windows.erase(it);
    });
}

}  // namespace

bool IsPreviewableImageName(const std::wstring& name) {
    static const wchar_t* const kExt[] = {L".jpg", L".jpeg", L".jfif", L".png", L".gif", L".bmp", L".dib", L".ico",
                                          L".tif", L".tiff", L".webp", L".heic", L".heif", L".avif", L".jxl"};
    const auto e = LowerExt(name);
    for (auto* x : kExt)
        if (e == x) return true;
    return false;
}

bool IsImageDecoderAvailable(const std::wstring& name) {
    // Asks WIC for the extensions that all installed decoders support (only once)
    static std::set<std::wstring> exts = [] {
        std::set<std::wstring> s;
        try {
            for (auto const& info : Windows::Graphics::Imaging::BitmapDecoder::GetDecoderInformationEnumerator())
                for (auto const& e : info.FileExtensions()) {
                    std::wstring x{e};
                    std::transform(x.begin(), x.end(), x.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                    s.insert(x);
                }
        } catch (...) {}
        return s;
    }();
    // For tests: the environment variable LYCAN_TEST_NO_DECODER=".jxl,.avif" makes the listed extensions count as "no decoder" (a development machine usually has them all)
    if (const wchar_t* fake = _wgetenv(L"LYCAN_TEST_NO_DECODER")) {
        std::wstring list = std::wstring(L",") + fake + L",";
        std::transform(list.begin(), list.end(), list.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (list.find(L"," + LowerExt(name) + L",") != std::wstring::npos) return false;
    }
    if (exts.empty()) return true;  // not found: do not block, let the real decoding decide
    return exts.count(LowerExt(name)) != 0;
}

bool CanPreviewImage(const std::wstring& name) { return IsPreviewableImageName(name) && IsImageDecoderAvailable(name); }

std::wstring MissingDecoderHint(const std::wstring& name) {
    const auto e = LowerExt(name);
    if (e == L".jxl") return lycan::Tr(L"preview.jpeg_xl_images_need_jpeg_xl");
    if (e == L".avif") return lycan::Tr(L"preview.avif_images_need_av1_video");
    if (e == L".heic" || e == L".heif") return lycan::Tr(L"preview.heic_heif_images_need_heif_image");
    if (e == L".webp") return lycan::Tr(L"preview.webp_images_need_webp_image");
    return lycan::Tr(L"preview.pc_has_no_decoder_can_preview");
}

void ShowImagePreview(PreviewRequest req) {
    if (req.items.empty()) return;
    const void* owner = req.owner;
    if (auto it = g_windows.find(owner); it != g_windows.end() && !it->second->IsClosed()) { it->second->Apply(std::move(req)); return; }
    auto w = std::make_shared<PreviewWindow>();
    w->owner_ = owner;
    g_windows[owner] = w;  // register first: the read-ahead limit has to count this window
    w->Create(std::move(req));
}

void CloseImagePreview(const void* owner) {
    if (!owner) {  // close all
        auto all = std::move(g_windows);
        g_windows.clear();
        for (auto& [k, w] : all) w->Close();
        return;
    }
    auto it = g_windows.find(owner);
    if (it == g_windows.end()) return;  // this main window has no preview (the previews of other windows are not touched)
    auto w = it->second;
    g_windows.erase(it);
    w->Close();
}

// ---- File-list thumbnails
namespace {
// All live thumbnail caches (UI thread only). The process-wide total is split among the caches that hold
// something, so N windows do not hold N x the cap.
std::vector<const uint64_t*> g_thumbSets;  // each cache's cached byte count
constexpr uint64_t kThumbTotal = 128ull << 20;
constexpr uint64_t kThumbMinShare = 32ull << 20;
}  // namespace

struct ListThumbs::Impl : std::enable_shared_from_this<Impl> {
    struct Req { uint32_t entry; uint64_t size; uint32_t px; Done done; };
    struct Entry { Microsoft::UI::Xaml::Media::Imaging::BitmapImage img{nullptr}; uint64_t cost = 0; std::list<uint64_t>::iterator it; };
    // Share of the process-wide cap (decoded pixels). Other caches only shrink on their own next Put.
    uint64_t Budget() const {
        size_t active = 1;
        for (auto* o : g_thumbSets) if (o != &bytes && *o > 0) ++active;
        return (std::max)(kThumbMinShare, kThumbTotal / active);
    }

    Microsoft::UI::Dispatching::DispatcherQueue dq{nullptr};
    Reader reader;
    bool canRead = false, running = false;
    std::atomic<uint32_t> session{0};
    std::vector<Req> queue;
    std::unordered_map<uint64_t, Entry> cache;
    std::list<uint64_t> lru;  // the most recently used first
    std::set<uint64_t> failed;
    uint64_t bytes = 0;

    static uint64_t Key(uint32_t entry, uint32_t px) { return (static_cast<uint64_t>(px) << 32) | entry; }

    void Put(uint64_t key, Microsoft::UI::Xaml::Media::Imaging::BitmapImage const& img, uint64_t cost) {
        lru.push_front(key);
        cache[key] = Entry{img, cost, lru.begin()};
        bytes += cost;
        const uint64_t budget = Budget();
        while (bytes > budget && lru.size() > 1) {
            const uint64_t k = lru.back();
            lru.pop_back();
            bytes -= cache[k].cost;
            cache.erase(k);
        }
    }

    fire_and_forget Loop() {
        if (running) co_return;
        running = true;
        auto self = shared_from_this();
        const uint32_t s = session;
        for (;;) {
            if (s != session || queue.empty()) break;
            Req r = queue.front();  // first queued, first handled: the items that appear on the screen are loaded in order (the ones that left the screen were cancelled)
            queue.erase(queue.begin());
            const uint64_t key = Key(r.entry, r.px);
            if (cache.count(key) || failed.count(key)) continue;
            if (!canRead || r.size > kMaxThumbSourceBytes) { failed.insert(key); continue; }
            co_await resume_background();
            auto data = std::make_shared<std::vector<uint8_t>>();
            std::wstring err;
            const bool ok = reader.Read(r.entry, kMaxThumbSourceBytes, *data, [self, s] { return self->session.load() == s; }, err);
            co_await resume_ui{dq};
            if (s != session) break;
            if (!ok) { failed.insert(key); continue; }
            try {
                Decoded d;
                co_await DecodeInto(data, r.px, d);
                if (s != session) break;
                if (d.frames > 1) d.image.Stop();  // an animation does not play in the list
                const uint64_t dw = std::min<uint64_t>(d.w, r.px), dh = d.w ? static_cast<uint64_t>(d.h) * dw / d.w : d.h;
                Put(key, d.image, dw * dh * 4);
                if (r.done) r.done(d.image);
            } catch (hresult_error const&) {
                failed.insert(key);  // cannot be decoded: keep the original icon
            }
        }
        if (s == session) running = false;
    }
};

ListThumbs::ListThumbs() : p_(std::make_shared<Impl>()) {
    p_->dq = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    g_thumbSets.push_back(&p_->bytes);
}
ListThumbs::~ListThumbs() {
    ++p_->session;
    g_thumbSets.erase(std::remove(g_thumbSets.begin(), g_thumbSets.end(), &p_->bytes), g_thumbSets.end());
}

void ListThumbs::Reset(const std::filesystem::path& archive, const lycan::OpenOptions& open, bool canRead) {
    ++p_->session;
    p_->running = false;
    p_->queue.clear();
    p_->cache.clear();
    p_->lru.clear();
    p_->failed.clear();
    p_->bytes = 0;
    p_->canRead = canRead;
    p_->reader.Reset(archive, open);
}

bool ListThumbs::CanRead() const { return p_->canRead; }

winrt::Windows::Foundation::IInspectable ListThumbs::Get(uint32_t entryIndex, uint32_t px) {
    auto it = p_->cache.find(Impl::Key(entryIndex, px));
    if (it == p_->cache.end()) return nullptr;
    p_->lru.splice(p_->lru.begin(), p_->lru, it->second.it);
    return it->second.img;
}

void ListThumbs::Request(uint32_t entryIndex, uint64_t size, uint32_t px, Done done) {
    if (!p_->canRead) return;
    const uint64_t key = Impl::Key(entryIndex, px);
    if (p_->cache.count(key) || p_->failed.count(key)) return;
    for (auto& q : p_->queue) if (q.entry == entryIndex && q.px == px) { q.done = std::move(done); return; }
    p_->queue.push_back({entryIndex, size, px, std::move(done)});
    p_->Loop();
}

void ListThumbs::Cancel(uint32_t entryIndex) {
    auto& q = p_->queue;
    q.erase(std::remove_if(q.begin(), q.end(), [&](Impl::Req const& r) { return r.entry == entryIndex; }), q.end());
}

}  // namespace lycan_app
