#pragma once

#include <libultraship/libultraship.h>

/* Online screen: host or join an online session (OnlineWindow.cpp). */
class OnlineWindow : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    void Draw() override;

  protected:
    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
};
