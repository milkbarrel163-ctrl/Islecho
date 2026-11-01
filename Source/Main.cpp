#include "WorldComponent.h"
#include <juce_gui_extra/juce_gui_extra.h>

class IslechoApplication final : public juce::JUCEApplication,
                                  public juce::MenuBarModel {
public:
    const juce::String getApplicationName() override { return "Islecho"; }
    const juce::String getApplicationVersion() override {
#if defined(JUCE_APPLICATION_VERSION_STRING)
        return JUCE_APPLICATION_VERSION_STRING;
#else
        return JUCE_STRINGIFY(JUCE_APP_VERSION);
#endif
    }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        window = std::make_unique<MainWindow>();
#if defined(__APPLE__) && !TARGET_OS_IOS
        juce::MenuBarModel::setMacMainMenu(this);
#endif
    }
    void shutdown() override {
#if defined(__APPLE__) && !TARGET_OS_IOS
        juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
        window.reset();
    }
    void suspended() override { if (window) window->checkpoint(); }
    void resumed() override { if (window) window->resume(); }
    void systemRequestedQuit() override { if (window) window->checkpoint(); quit(); }

    // ── MenuBarModel ─────────────────────────────────────────────────────────
    juce::StringArray getMenuBarNames() override { return { "Islecho" }; }

    juce::PopupMenu getMenuForIndex(int, const juce::String&) override {
        juce::PopupMenu menu;
        menu.addItem(1, "Preferences...");
        menu.addSeparator();
        const bool rec = window && window->isRecording();
        menu.addItem(2, rec ? "Stop Recording" : "Start Recording");
        return menu;
    }

    void menuItemSelected(int menuItemID, int) override {
        if (menuItemID == 1 && window) window->showPreferences();
        if (menuItemID == 2 && window) window->toggleRecording();
    }

private:
    class MainWindow final : public juce::DocumentWindow {
    public:
        MainWindow() : DocumentWindow("Islecho", juce::Colour(0xff07120f),
                                       DocumentWindow::allButtons) {
           #if defined(__APPLE__) && !TARGET_OS_IOS
            // Native title bar gives macOS traffic-light buttons;
            // transparent background lets Metal/OpenGL content show through.
            setUsingNativeTitleBar(true);
            setBackgroundColour(juce::Colours::transparentBlack);
           #else
            setUsingNativeTitleBar(false);
           #endif
            setContentOwned(new WorldComponent(), true);
            setResizable(true, false);
            if (juce::SystemStats::getOperatingSystemType() == juce::SystemStats::iOS) {
                setTitleBarHeight(0);
                setFullScreen(true);
            } else
                centreWithSize(1000, 650);
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
        void checkpoint() { if (auto* w = world()) w->checkpointForInterruption(); }
        void resume()     { if (auto* w = world()) w->resumeAfterInterruption(); }
        void showPreferences()  { if (auto* w = world()) w->showPreferences(); }
        bool isRecording()      { if (auto* w = world()) return w->isRecording(); return false; }
        void toggleRecording()  { if (auto* w = world()) w->toggleRecording(); }
    private:
        WorldComponent* world() { return dynamic_cast<WorldComponent*>(getContentComponent()); }
    };
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION(IslechoApplication)
