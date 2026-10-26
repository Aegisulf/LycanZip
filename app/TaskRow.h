#pragma once
#include "TaskRow.g.h"
#include "lycan/taskqueue.h"

#include <chrono>

namespace winrt::LycanZip::implementation
{
    struct TaskRow : TaskRowT<TaskRow>
    {
        explicit TaskRow(uint64_t id) : id_(id) {}

        uint64_t Id() const { return id_; }
        hstring Title() const { return title_; }
        hstring StatusText() const { return status_; }
        double Progress() const { return progress_; }
        bool IsIndeterminate() const { return indeterminate_; }
        Microsoft::UI::Xaml::Visibility PauseVisibility() const { return vis(pause_); }
        Microsoft::UI::Xaml::Visibility ResumeVisibility() const { return vis(resume_); }
        Microsoft::UI::Xaml::Visibility CancelVisibility() const { return vis(cancel_); }
        Microsoft::UI::Xaml::Visibility OpenFolderVisibility() const { return vis(!outputPath_.empty() && finishedOk_); }
        Microsoft::UI::Xaml::Visibility ProgressVisibility() const { return vis(showProgress_); }
        Microsoft::UI::Xaml::Visibility TestVisibility() const { return vis(testable_ && !testing_); }
        Microsoft::UI::Xaml::Visibility DeleteVisibility() const { return vis(deletable_ && !testing_); }
        hstring DeleteLabel() const { return deleteLabel_; }
        hstring OutputPath() const { return outputPath_; }
        Microsoft::UI::Xaml::Media::Geometry LitGeometry() const { return lit_; }
        Microsoft::UI::Xaml::Media::Brush LitBrush() const { return litBrush_; }
        Microsoft::UI::Xaml::Media::Brush StateGlyphBrush() const { return glyphBrush_; }
        hstring StateGlyph() const { return glyph_; }
        Microsoft::UI::Xaml::Visibility StateGlyphVisibility() const { return vis(!glyph_.empty()); }
        Microsoft::UI::Xaml::Visibility RingErrorVisibility() const { return vis(error_); }
        Microsoft::UI::Xaml::Visibility SpinnerVisibility() const { return vis(indeterminate_); }

        // Updates all fields from the task state (called on the UI thread)
        void Apply(lycan::TaskInfo const& info);
        // Additional note (for example "the original archive was moved to the recycle bin"), shown after the status text
        void SetNote(hstring const& note)
        {
            note_ = note;
            status_ = hstring{std::wstring(status_) + std::wstring(note)};
            Raise(L"StatusText");
        }

        // The buttons of a finished task (the owner decides what is possible)
        void SetTestable(bool v) { testable_ = v; Raise(L"TestVisibility"); }
        void SetDeletable(bool v, hstring const& label)
        {
            deletable_ = v;
            deleteLabel_ = label;
            Raise(L"DeleteVisibility");
            Raise(L"DeleteLabel");
        }
        // A follow-up integrity test is shown inside this card: while it runs the card shows the test's progress
        // (the owner calls Apply() with the test task's info); when it ends the card returns to the finished
        // state of the original task with a note about the result.
        void BeginTest()
        {
            testing_ = true;
            Raise(L"TestVisibility");
            Raise(L"DeleteVisibility");
        }
        void EndTest(hstring const& note)
        {
            testing_ = false;
            if (haveBase_) Apply(base_);
            SetNote(note);
            Raise(L"TestVisibility");
            Raise(L"DeleteVisibility");
        }
        bool Testing() const { return testing_; }

        winrt::event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& h)
        {
            return changed_.add(h);
        }
        void PropertyChanged(winrt::event_token const& t) noexcept { changed_.remove(t); }

    private:
        static Microsoft::UI::Xaml::Visibility vis(bool v)
        {
            return v ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed;
        }
        void Raise(wchar_t const* name)
        {
            changed_(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{name});
        }

        uint64_t id_;
        hstring title_, status_, outputPath_, note_;
        Microsoft::UI::Xaml::Media::Geometry lit_{nullptr};
        Microsoft::UI::Xaml::Media::Brush litBrush_{nullptr}, glyphBrush_{nullptr};
        hstring glyph_;
        bool error_ = false;
        uint64_t lastDone_ = 0;                                   // speed estimate (exponential smoothing)
        std::chrono::steady_clock::time_point lastTime_{};
        double speed_ = 0;
        double progress_ = 0;
        lycan::TaskInfo base_{};          // the finished task this card belongs to (restored after a follow-up test)
        bool haveBase_ = false, testing_ = false, testable_ = false, deletable_ = false;
        hstring deleteLabel_;
        bool indeterminate_ = true, pause_ = false, resume_ = false, cancel_ = true, showProgress_ = true,
             finishedOk_ = false;
        winrt::event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> changed_;
    };
}

namespace winrt::LycanZip::factory_implementation
{
    struct TaskRow : TaskRowT<TaskRow, implementation::TaskRow> {};
}
