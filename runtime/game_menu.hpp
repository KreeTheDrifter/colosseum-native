// Colosseum Native in-game settings menu (Esc / F1 / controller View button).
// Drawn with Dolphin's on-screen ImGui layer; settings apply live where possible and are
// saved (Dolphin's GFX.ini, and [Game] in User/Config/ColosseumNative.ini).
#pragma once

#include <functional>

namespace moderngekko::menu
{
enum class Key
{
  Up,
  Down,
  Left,
  Right,
  Accept,
  Back,
  Toggle,
};

// Window thread: keyboard input. Returns true when the key was used by the menu (always
// for Toggle; for the others only while the menu is open).
bool OnKey(Key key);

bool IsOpen();

// Render thread, inside Dolphin's ImGui frame (OnScreenUI::Finalize).
void Draw();

struct HostHooks
{
  std::function<void()> toggle_fullscreen;
  std::function<bool()> is_fullscreen;
  std::function<void()> quit;
};
void SetHostHooks(HostHooks hooks);
}  // namespace moderngekko::menu
