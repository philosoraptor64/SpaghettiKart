#include "OnlineWindow.h"
#include "ImguiUI.h"
#include "UIWidgets.h"
#include "ResolutionEditor.h"
#include "FreecamWindow.h"
#include "Tools.h"
#include "SceneExplorer.h"
#include "Properties.h"
#include "TrackProperties.h"
#include "ContentBrowser.h"

#include <spdlog/spdlog.h>
#include <imgui.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ship/Context.h"

#include <imgui_internal.h>
#include <libultraship/libultraship.h>
#include <fast/Fast3dWindow.h>
#include <fast/Fast3dGui.h>
#include "port/Engine.h"
#include "PortMenu.h"

extern "C" {
const char* KartNet_WaitMessage(void); /* port/KartNet.c */
extern s32 gGamestateNext;
extern s32 gMenuSelection;
#include "audio/external.h"
#include "defines.h"
}

namespace GameUI {
/* Netplay status ("Waiting for the other player..."): drawn over everything
 * while KartNet has a wait message (rendezvous, handshake, long stalls). */
class NetplayStatusOverlay : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    void Draw() override {
        const char* msg = KartNet_WaitMessage();
        if (msg == nullptr || msg[0] == '\0') {
            return;
        }
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 170));
        ImFont* font = ImGui::GetFont();
        float size = vp->Size.y / 16.0f;
        if (size < 18.0f) {
            size = 18.0f;
        }
        /* One line per '\n', centered. */
        std::string all(msg);
        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t nl = all.find('\n', start);
            lines.push_back(all.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) {
                break;
            }
            start = nl + 1;
        }
        float lineH = size * 1.4f;
        float y = vp->Pos.y + (vp->Size.y - lineH * (float) lines.size()) / 2.0f;
        for (size_t i = 0; i < lines.size(); i++) {
            float s = i == 0 ? size : size * 0.75f;
            ImVec2 ts = font->CalcTextSizeA(s, FLT_MAX, 0.0f, lines[i].c_str());
            ImVec2 pos(vp->Pos.x + (vp->Size.x - ts.x) / 2.0f, y);
            dl->AddText(font, s, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 0, 255), lines[i].c_str());
            dl->AddText(font, s, pos, i == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255),
                        lines[i].c_str());
            y += lineH;
        }
    }

  protected:
    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
};
std::shared_ptr<Ship::GuiWindow> mNetplayStatus;
std::shared_ptr<Ship::GuiWindow> mOnlineWindow;

// std::shared_ptr<GameMenuBar> mGameMenuBar;
std::shared_ptr<PortMenu> mPortMenu;
std::shared_ptr<Ship::GuiWindow> mConsoleWindow;
std::shared_ptr<Ship::GuiWindow> mStatsWindow;
std::shared_ptr<Ship::GuiWindow> mInputEditorWindow;
std::shared_ptr<Ship::GuiWindow> mGfxDebuggerWindow;
std::shared_ptr<Ship::GuiWindow> mToolsWindow;
std::shared_ptr<Ship::GuiWindow> mSceneExplorerWindow;
std::shared_ptr<Ship::GuiWindow> mPropertiesWindow;
std::shared_ptr<Ship::GuiWindow> mTrackPropertiesWindow;
std::shared_ptr<Ship::GuiWindow> mContentBrowserWindow;

void SetupGuiElements() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();

    // mGameMenuBar = std::make_shared<GameMenuBar>("gOpenMenuBar", CVarGetInteger("gOpenMenuBar", 0));
    // gui->SetMenuBar(mGameMenuBar);

    mPortMenu = std::make_shared<PortMenu>("gOpenMenu", "Port Menu");
    gui->SetMenu(mPortMenu);

    mStatsWindow = gui->GetGuiWindow("Stats");
    if (mStatsWindow == nullptr) {
        SPDLOG_ERROR("Could not find stats window");
    }

    mConsoleWindow = gui->GetGuiWindow("Console");
    if (mConsoleWindow == nullptr) {
        SPDLOG_ERROR("Could not find console window");
    }

    mInputEditorWindow = gui->GetGuiWindow("Input Editor");
    if (mInputEditorWindow == nullptr) {
        SPDLOG_ERROR("Could not find input editor window");
        return;
    }

    mGfxDebuggerWindow = gui->GetGuiWindow("GfxDebuggerWindow");
    if (mGfxDebuggerWindow == nullptr) {
        SPDLOG_ERROR("Could not find input GfxDebuggerWindow");
    }

    mToolsWindow = std::make_shared<TrackEditor::ToolsWindow>("gEditorEnabled", "Tools", ImVec2(100, 100),
                                                                  (ImGuiWindowFlags_NoTitleBar));
    gui->AddGuiWindow(mToolsWindow);

    mSceneExplorerWindow = std::make_shared<TrackEditor::SceneExplorerWindow>("gEditorEnabled", "Scene Explorer");
    gui->AddGuiWindow(mSceneExplorerWindow);

    mPropertiesWindow = std::make_shared<TrackEditor::PropertiesWindow>("gEditorEnabled", "Properties");
    gui->AddGuiWindow(mPropertiesWindow);

    mTrackPropertiesWindow = std::make_shared<TrackEditor::TrackPropertiesWindow>("gEditorEnabled", "Track Properties");
    gui->AddGuiWindow(mTrackPropertiesWindow);

    mContentBrowserWindow =
        std::make_shared<TrackEditor::ContentBrowserWindow>("gEditorEnabled", "Content Browser");
    gui->AddGuiWindow(mContentBrowserWindow);

    mNetplayStatus = std::make_shared<NetplayStatusOverlay>("gNetplayStatusOverlay", true, "Netplay Status");
    gui->AddGuiWindow(mNetplayStatus);
    mOnlineWindow = std::make_shared<OnlineWindow>("gOnlineWindow", true, "Play Online");
    gui->AddGuiWindow(mOnlineWindow);
}

void Destroy() {
    mConsoleWindow = nullptr;
    mStatsWindow = nullptr;
    mInputEditorWindow = nullptr;
    mToolsWindow = nullptr;
    mSceneExplorerWindow = nullptr;
    mPropertiesWindow = nullptr;
    mTrackPropertiesWindow = nullptr;
    mContentBrowserWindow = nullptr;
    mNetplayStatus = nullptr;
    mOnlineWindow = nullptr;
    // these two were missed: the menu held on to the GUI, which holds the
    // window and the whole graphics interpreter (all leaked per run in a host)
    mPortMenu = nullptr;
    mGfxDebuggerWindow = nullptr;
}

std::string GetWindowButtonText(const char* text, bool menuOpen) {
    char buttonText[100] = "";
    if (menuOpen) {
        strcat(buttonText, ICON_FA_CHEVRON_RIGHT " ");
    }
    strcat(buttonText, text);
    if (!menuOpen) {
        strcat(buttonText, "  ");
    }
    return buttonText;
}
} // namespace GameUI

static const char* filters[3] = {
#ifdef __WIIU__
    "",
#else
    "Three-Point",
#endif
    "Linear", "None"
};

void DrawSettingsMenu() {
    //    if (UIWidgets::BeginMenu("Settings")) {
    //         if (UIWidgets::BeginMenu("Audio")) {
    //             UIWidgets::CVarSliderFloat("Master Volume", "gGameMasterVolume", 0.0f, 1.0f, 1.0f, {
    //                 .format = "%.0f%%",
    //                 .isPercentage = true,
    //             });
    //             if (UIWidgets::CVarSliderFloat("Main Music Volume", "gMainMusicVolume", 0.0f, 1.0f, 1.0f,
    //             {
    //                 .format = "%.0f%%",
    //                 .isPercentage = true,
    //             })) {
    //                 audio_set_player_volume(SEQ_PLAYER_LEVEL, CVarGetFloat("gMainMusicVolume", 1.0f));
    //             }
    //             if (UIWidgets::CVarSliderFloat("Sound Effects Volume", "gSFXMusicVolume",
    //             0.0f, 1.0f, 1.0f, {
    //                 .format = "%.0f%%",
    //                 .isPercentage = true,
    //             })) {
    //                 audio_set_player_volume(SEQ_PLAYER_SFX, CVarGetFloat("gSFXMusicVolume", 1.0f));
    //             }
    //             if (UIWidgets::CVarSliderFloat("Environment Volume", "gEnvironmentVolume",
    //             0.0f, 1.0f, 1.0f, {
    //                 .format = "%.0f%%",
    //                 .isPercentage = true,
    //             })) {
    //                 audio_set_player_volume(SEQ_PLAYER_ENV, CVarGetFloat("gEnvironmentVolume", 1.0f));
    //             }
    //
    //             static std::unordered_map<Ship::AudioBackend, const char*> audioBackendNames = {
    //                     { Ship::AudioBackend::WASAPI, "Windows Audio Session API" },
    //                     { Ship::AudioBackend::SDL, "SDL" },
    //             };
    //
    //             ImGui::Text("Audio API (Needs reload)");
    //             auto currentAudioBackend = Ship::Context::GetRawInstance()->GetAudio()->GetCurrentAudioBackend();
    //
    //             if (Ship::Context::GetRawInstance()->GetAudio()->GetAvailableAudioBackends()->size() <= 1) {
    //                 UIWidgets::DisableComponent(ImGui::GetStyle().Alpha * 0.5f);
    //             }
    //             if (ImGui::BeginCombo("##AApi", audioBackendNames[currentAudioBackend])) {
    //                 for (uint8_t i = 0; i <
    //                 Ship::Context::GetRawInstance()->GetAudio()->GetAvailableAudioBackends()->size(); i++) {
    //                     auto backend =
    //                     Ship::Context::GetRawInstance()->GetAudio()->GetAvailableAudioBackends()->data()[i]; if
    //                     (ImGui::Selectable(audioBackendNames[backend], backend == currentAudioBackend)) {
    //                         Ship::Context::GetRawInstance()->GetAudio()->SetCurrentAudioBackend(backend);
    //                     }
    //                 }
    //                 ImGui::EndCombo();
    //             }
    //             if (Ship::Context::GetRawInstance()->GetAudio()->GetAvailableAudioBackends()->size() <= 1) {
    //                 UIWidgets::ReEnableComponent("");
    //             }
    //
    //             ImGui::EndMenu();
    //         }
    //
    //        UIWidgets::Spacer(0);
    //
    //        if (UIWidgets::BeginMenu("Controller")) {
    //            UIWidgets::WindowButton("Controller Mapping", "gInputEditorWindow", GameUI::mInputEditorWindow);
    //
    //            UIWidgets::Spacer(0);
    //
    // #ifndef __SWITCH__
    //            UIWidgets::CVarCheckbox(
    //                "Menubar Controller Navigation", "gControlNav",
    //                { .tooltip = "Allows controller navigation of the SOH menu bar (Settings,
    //                Enhancements,...)\nCAUTION: "
    //                             "This will disable game inputs while the menubar is visible.\n\nD-pad to move between
    //                             " "items, A to select, and X to grab focus on the menu bar" });
    // #endif
    //            UIWidgets::CVarCheckbox("Show Inputs", "gInputEnabled",
    //                                    { .tooltip = "Shows currently pressed inputs on the bottom right of the
    //                                    screen" });
    //            if (CVarGetInteger("gInputEnabled", 0)) {
    //                UIWidgets::CVarSliderFloat(
    //                    "Input Scale", "gInputScale", 1.0f, 3.0f, 1.0f,
    //                    {
    //                        .tooltip = "Sets the on screen size of the displayed inputs from the Show Inputs setting",
    //                        .format = "%.1fx",
    //                    });
    //            }
    //
    //            ImGui::EndMenu();
    //        }
    //
    //        ImGui::EndMenu();
    //    }
    //
    //    ImGui::SetCursorPosY(0.0f);
    //    if (UIWidgets::BeginMenu("Graphics")) {
    //        UIWidgets::WindowButton("Resolution Editor", "gAdvancedResolutionEditorEnabled",
    //                                GameUI::mAdvancedResolutionSettingsWindow);
    //
    //        UIWidgets::Spacer(0);
    //
    //        // Previously was running every frame, and nothing was setting it? Maybe a bad copy/paste?
    //        // Ship::Context::GetRawInstance()->GetWindow()->SetResolutionMultiplier(CVarGetFloat("gInternalResolution",
    //        1));
    //        // UIWidgets::Tooltip("Multiplies your output resolution by the value inputted, as a more intensive but
    //        // effective form of anti-aliasing");
    // #ifndef __WIIU__
    //        if (UIWidgets::CVarSliderInt(
    //                "MSAA: %d", "gMSAAValue", 1, 8, 1,
    //                { .tooltip =
    //                      "Activates multi-sample anti-aliasing when above 1x up to 8x for 8 samples for every pixel"
    //                      })) {
    //            Ship::Context::GetRawInstance()->GetWindow()->SetMsaaLevel(CVarGetInteger("gMSAAValue", 1));
    //        }
    // #endif
    //
    //        { // FPS Slider
    //            const int minFps = 30;
    //            static int maxFps;
    //            if (Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend() ==
    //                Fast::WindowBackend::FAST3D_DXGI_DX11) {
    //                maxFps = 360;
    //            } else {
    //                maxFps = Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate();
    //            }
    //            int currentFps = 0;
    // #ifdef __WIIU__
    //            UIWidgets::Spacer(0);
    //            // only support divisors of 60 on the Wii U
    //            if (currentFps > 60) {
    //                currentFps = 60;
    //            } else {
    //                currentFps = 60 / (60 / currentFps);
    //            }
    //
    //            int fpsSlider = 1;
    //            if (currentFps == 30) {
    //                ImGui::Text("FPS: Original (30)");
    //            } else {
    //                ImGui::Text("FPS: %d", currentFps);
    //                if (currentFps == 30) {
    //                    fpsSlider = 2;
    //                } else { // currentFps == 60
    //                    fpsSlider = 3;
    //                }
    //            }
    //            if (CVarGetInteger("gMatchRefreshRate", 0)) {
    //                UIWidgets::DisableComponent(ImGui::GetStyle().Alpha * 0.5f);
    //            }
    //
    //            if (ImGui::Button(" - ##WiiUFPS")) {
    //                fpsSlider--;
    //            }
    //            ImGui::SameLine();
    //            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 7.0f);
    //
    //            UIWidgets::Spacer(0);
    //
    //            ImGui::PushItemWidth(std::min((ImGui::GetContentRegionAvail().x - 60.0f), 260.0f));
    //            ImGui::SliderInt("##WiiUFPSSlider", &fpsSlider, 1, 3, "", ImGuiSliderFlags_AlwaysClamp);
    //            ImGui::PopItemWidth();
    //
    //            ImGui::SameLine();
    //            ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 7.0f);
    //            if (ImGui::Button(" + ##WiiUFPS")) {
    //                fpsSlider++;
    //            }
    //
    //            if (CVarGetInteger("gMatchRefreshRate", 0)) {
    //                UIWidgets::ReEnableComponent("");
    //            }
    //            if (fpsSlider > 3) {
    //                fpsSlider = 3;
    //            } else if (fpsSlider < 1) {
    //                fpsSlider = 1;
    //            }
    //
    //            if (fpsSlider == 1) {
    //                currentFps = 20;
    //            } else if (fpsSlider == 2) {
    //                currentFps = 30;
    //            } else if (fpsSlider == 3) {
    //                currentFps = 60;
    //            }
    //            CVarSetInteger("gInterpolationFPS", currentFps);
    //            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    // #else
    //            bool matchingRefreshRate =
    //                CVarGetInteger("gMatchRefreshRate", 0) &&
    //                Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend() !=
    //                Fast::WindowBackend::FAST3D_DXGI_DX11;
    //            UIWidgets::CVarSliderInt((currentFps == 20) ? "FPS: Original (20)" : "FPS: %d", "gInterpolationFPS",
    //            minFps,
    //                                     maxFps, 1, { .disabled = matchingRefreshRate });
    // #endif
    //            if (Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend() ==
    //                Fast::WindowBackend::FAST3D_DXGI_DX11) {
    //                UIWidgets::Tooltip(
    //                    "Uses Matrix Interpolation to create extra frames, resulting in smoother graphics. "
    //                    "This is purely "
    //                    "visual and does not impact game logic, execution of glitches etc.\n\n"
    //                    "A higher target FPS than your monitor's refresh rate will waste resources, and might give a
    //                    worse " "result.");
    //            } else {
    //                UIWidgets::Tooltip(
    //                    "Uses Matrix Interpolation to create extra frames, resulting in smoother graphics. This is
    //                    purely " "visual and does not impact game logic, execution of glitches etc.");
    //            }
    //        } // END FPS Slider
    //
    //        if (Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend() ==
    //        Fast::WindowBackend::FAST3D_DXGI_DX11) {
    //            UIWidgets::Spacer(0);
    //            if (ImGui::Button("Match Refresh Rate")) {
    //                int hz = Ship::Context::GetRawInstance()->GetWindow()->GetCurrentRefreshRate();
    //                if (hz >= 30 && hz <= 360) {
    //                    CVarSetInteger("gInterpolationFPS", hz);
    //                    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    //                }
    //            }
    //        } else {
    //            UIWidgets::PaddedEnhancementCheckbox("Match Refresh Rate", "gMatchRefreshRate", true, false);
    //        }
    //
    //        UIWidgets::Tooltip("Matches interpolation value to the current game's window refresh rate");
    //
    //        if (Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend() ==
    //        Fast::WindowBackend::FAST3D_DXGI_DX11) {
    //            UIWidgets::PaddedEnhancementSliderInt(
    //                CVarGetInteger("gExtraLatencyThreshold", 0) == 0 ? "Jitter fix: Off" : "Jitter fix: >= %d FPS",
    //                "##ExtraLatencyThreshold", "gExtraLatencyThreshold", 0, 360, "", 0, true, true, false);
    //            UIWidgets::Tooltip("When Interpolation FPS setting is at least this threshold, add one frame of input
    //            lag "
    //                               "(e.g. 16.6 ms for 60 FPS) in order to avoid jitter. This setting allows the CPU to
    //                               " "work on one frame while GPU works on the previous frame.\nThis setting should be
    //                               used " "when your computer is too slow to do CPU + GPU work in time.");
    //        }
    //
    //        UIWidgets::PaddedSeparator(true, true, 3.0f, 3.0f);
    //
    //        static std::unordered_map<Fast::WindowBackend, const char*> windowBackendNames = {
    //            { Fast::WindowBackend::FAST3D_DXGI_DX11, "DirectX" },
    //            { Fast::WindowBackend::FAST3D_SDL_OPENGL, "OpenGL" },
    //            { Fast::WindowBackend::FAST3D_SDL_METAL, "Metal" },
    //        };
    //
    //        ImGui::Text("Renderer API (Needs reload)");
    //        Fast::WindowBackend runningWindowBackend = Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend();
    //        Fast::WindowBackend configWindowBackend;
    //        int configWindowBackendId = Ship::Context::GetRawInstance()->GetConfig()->GetInt("Window.Backend.Id", -1);
    //        if (Ship::Context::GetRawInstance()->GetWindow()->IsAvailableWindowBackend(configWindowBackendId)) {
    //            configWindowBackend = static_cast<Fast::WindowBackend>(configWindowBackendId);
    //        } else {
    //            configWindowBackend = runningWindowBackend;
    //        }
    //
    //        if (Ship::Context::GetRawInstance()->GetWindow()->GetAvailableWindowBackends()->size() <= 1) {
    //            UIWidgets::DisableComponent(ImGui::GetStyle().Alpha * 0.5f);
    //        }
    //        if (ImGui::BeginCombo("##RApi", windowBackendNames[configWindowBackend])) {
    //            for (size_t i = 0; i <
    //            Ship::Context::GetRawInstance()->GetWindow()->GetAvailableWindowBackends()->size();
    //                 i++) {
    //                auto backend = Ship::Context::GetRawInstance()->GetWindow()->GetAvailableWindowBackends()->data()[i];
    //                if (ImGui::Selectable(windowBackendNames[backend], backend == configWindowBackend)) {
    //                    Ship::Context::GetRawInstance()->GetConfig()->SetInt("Window.Backend.Id",
    //                    static_cast<int>(backend));
    //                    Ship::Context::GetRawInstance()->GetConfig()->SetString("Window.Backend.Name",
    //                                                                         windowBackendNames[backend]);
    //                    Ship::Context::GetRawInstance()->GetConfig()->Save();
    //                }
    //            }
    //            ImGui::EndCombo();
    //        }
    //        if (Ship::Context::GetRawInstance()->GetWindow()->GetAvailableWindowBackends()->size() <= 1) {
    //            UIWidgets::ReEnableComponent("");
    //        }
    //
    //        if (Ship::Context::GetRawInstance()->GetWindow()->CanDisableVerticalSync()) {
    //            UIWidgets::PaddedEnhancementCheckbox("Enable Vsync", "gVsyncEnabled", true, false);
    //        }
    //
    //        if (Ship::Context::GetRawInstance()->GetWindow()->SupportsWindowedFullscreen()) {
    //            UIWidgets::PaddedEnhancementCheckbox("Windowed fullscreen", "gSdlWindowedFullscreen", true, false);
    //        }
    //
    //        if (Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SupportsViewports()) {
    //            UIWidgets::PaddedEnhancementCheckbox("Allow multi-windows", "gEnableMultiViewports", true, false,
    //            false, "",
    //                                                 UIWidgets::CheckboxGraphics::Cross, true);
    //            UIWidgets::Tooltip("Allows windows to be able to be dragged off of the main game window. Requires a
    //            reload "
    //                               "to take effect.");
    //        }
    //
    //        // If more filters are added to LUS, make sure to add them to the filters list here
    //        ImGui::Text("Texture Filter (Needs reload)");
    //
    //        UIWidgets::EnhancementCombobox("gTextureFilter", filters, 0);
    //
    //        UIWidgets::Spacer(0);
    //
    //        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGameOverlay()->DrawSettings();
    //
    //        ImGui::EndMenu();
    //    }
}

void DrawMenuBarIcon() {
    static bool gameIconLoaded = false;
    if (!gameIconLoaded) {
        // Ship::Context::GetRawInstance()->GetWindow()->GetGui()->LoadTexture("Game_Icon",
        // "textures/icons/gIcon.png");
        gameIconLoaded = false;
    }

    if (auto fast3dGui = std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())) {
        if (fast3dGui->GetTextureByName("Game_Icon")) {
#ifdef __SWITCH__
                ImVec2 iconSize = ImVec2(20.0f, 20.0f);
                float posScale = 1.0f;
#elif defined(__WIIU__)
                ImVec2 iconSize = ImVec2(16.0f * 2, 16.0f * 2);
                float posScale = 2.0f;
#else
                ImVec2 iconSize = ImVec2(20.0f, 20.0f);
                float posScale = 1.5f;
#endif
                ImGui::SetCursorPos(ImVec2(5, 2.5f) * posScale);
                ImGui::Image(fast3dGui->GetTextureByName("Game_Icon"), iconSize);
                ImGui::SameLine();
                ImGui::SetCursorPos(ImVec2(25, 0) * posScale);
            }
    }
}

void DrawGameMenu() {
}

void DrawEnhancementsMenu() {
    if (UIWidgets::BeginMenu("Enhancements")) {

        if (UIWidgets::BeginMenu("Gameplay")) {
            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }
}

void DrawCheatsMenu() {
}

const char* debugInfoPages[6] = {
    "Object", "Check Surface", "Map", "Stage", "Effect", "Enemy",
};

void DrawDebugMenu() {
}

void GameMenuBar::DrawElement() {
    if (ImGui::BeginMenuBar()) {
        DrawMenuBarIcon();

        DrawGameMenu();

        ImGui::SetCursorPosY(0.0f);

        // DrawSettingsMenu();

        ImGui::SetCursorPosY(0.0f);

        // DrawEnhancementsMenu();

        ImGui::SetCursorPosY(0.0f);

        // DrawCheatsMenu();

        ImGui::SetCursorPosY(0.0f);

        // DrawDebugMenu();

        ImGui::EndMenuBar();
    }
}
