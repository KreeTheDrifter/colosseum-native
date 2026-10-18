// Colosseum Native in-game settings menu (Esc / F1 / controller View button).
// Drawn with Dolphin's on-screen ImGui layer; settings apply live where possible and are
// saved (Dolphin's GFX.ini, and [Game] in User/Config/ColosseumNative.ini).
#pragma once

#include <functional>
#include <string>

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

// Any thread: a tap at normalized surface coordinates (0..1). Returns true when the menu
// is open and took it.
bool OnTap(float x, float y);

// Any thread: open the menu (or close it when open), e.g. from an on-screen button.
void Open();

bool IsOpen();

// Render thread, inside Dolphin's ImGui frame (OnScreenUI::Finalize).
void Draw();

struct HostHooks
{
  std::function<void()> toggle_fullscreen;
  std::function<bool()> is_fullscreen;
  std::function<void()> quit;
  // Android: name of the GPU driver in use, and leaving the game to change it.
  std::function<std::string()> gpu_driver_name;
  std::function<void()> change_gpu_driver;
};
void SetHostHooks(HostHooks hooks);
}  // namespace moderngekko::menu
