// Colosseum Native in-game settings menu. See game_menu.hpp.
//
// The menu is a single list of settings, drawn with Dolphin's on-screen ImGui layer.
// Keyboard: arrows, Enter, Esc/F1. Controller (XInput): D-pad/left stick, A, B, View.
// While it is open, Host_UIBlocksControllerState() closes the input gate, so the game
// doesn't see the buttons used to navigate it.

#include "game_menu.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/State.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace moderngekko::menu
{
namespace
{
// ---------------------------------------------------------------------------------------
// Pokemon Colosseum (GC6E01) settings block, patched into main.dol by tools/disc.py:
// +0 u8 frame-rate mask (0x00 = 60 fps, 0xFF = original 30/60), +4 f32 aspect multiplier.
constexpr u32 kSettingsAddr = 0x80266370;

struct AspectChoice
{
  const char* name;
  float multiplier;
  int width, height;
};
constexpr std::array<AspectChoice, 5> kAspects{{
    {"4:3", 1.0f, 4, 3},
    {"16:9", 4.0f / 3.0f, 16, 9},
    {"16:10", 1.2f, 16, 10},
    {"21:9", 1.75f, 21, 9},
    {"32:9", 8.0f / 3.0f, 32, 9},
}};

struct ShaderChoice
{
  const char* label;
  const char* file;  // Sys/Shaders/<file>.glsl, "" = none
};
constexpr std::array<ShaderChoice, 4> kShaders{{
    {"Off", ""},
    {"AMD FidelityFX CAS (sharpen)", "FidelityFX_CAS"},
    {"AMD FidelityFX FSR 1 (upscale)", "FidelityFX_FSR1"},
    {"FXAA", "FXAA"},
}};

struct AaChoice
{
  const char* label;
  u32 samples;
  bool ssaa;
};
constexpr std::array<AaChoice, 7> kAa{{
    {"Off", 1, false},
    {"2x MSAA", 2, false},
    {"4x MSAA", 4, false},
    {"8x MSAA", 8, false},
    {"2x SSAA", 2, true},
    {"4x SSAA", 4, true},
    {"8x SSAA", 8, true},
}};

// ---------------------------------------------------------------------------------------
std::mutex s_mutex;
std::deque<Key> s_keys;
std::atomic<bool> s_open{false};
HostHooks s_hooks;
int s_selected = 0;
int s_state_slot = 1;
bool s_dirty = false;  // settings changed since the menu opened
bool s_hint_shown = false;

std::string s_status;  // short message shown under the list
std::chrono::steady_clock::time_point s_status_until;

void SetStatus(std::string text)
{
  s_status = std::move(text);
  s_status_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

template <typename T>
int Wrap(int value, const T& list)
{
  const int n = static_cast<int>(list.size());
  return ((value % n) + n) % n;
}

// ----- Colosseum Native game options ([Game] in ColosseumNative.ini) -------------------
std::string GameIniPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "ColosseumNative.ini";
}

std::string ReadGameOption(const std::string& key, const std::string& fallback)
{
  Common::IniFile ini;
  ini.Load(GameIniPath());
  std::string value;
  if (!ini.GetOrCreateSection("Game")->Get(key, &value) || value.empty())
    return fallback;
  if (const auto comment = value.find(';'); comment != std::string::npos)
    value.resize(comment);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
    value.pop_back();
  return value;
}

void WriteGameOption(const std::string& key, const std::string& value)
{
  Common::IniFile ini;
  ini.Load(GameIniPath());
  ini.GetOrCreateSection("Game")->Set(key, value);
  ini.Save(GameIniPath());
}

bool s_fps60 = true;
int s_aspect = 1;  // index into kAspects

void LoadGameOptions()
{
  s_fps60 = ReadGameOption("FrameRate", "60") != "30";
  const std::string aspect = ReadGameOption("AspectRatio", "16:9");
  for (size_t i = 0; i < kAspects.size(); ++i)
    if (aspect == kAspects[i].name)
      s_aspect = static_cast<int>(i);
}

// Write the settings bytes straight into game memory so they take effect immediately;
// the launcher writes the same bytes into main.dol at the next start.
void PokeGameSettings()
{
  auto& system = Core::System::GetInstance();
  if (Core::GetState(system) == Core::State::Uninitialized)
    return;
  const Core::CPUThreadGuard guard(system);
  u8* p = system.GetMemory().GetPointerForRange(kSettingsAddr, 8);
  if (!p)
    return;
  p[0] = s_fps60 ? 0x00 : 0xFF;
  u32 bits;
  std::memcpy(&bits, &kAspects[s_aspect].multiplier, 4);
  p[4] = static_cast<u8>(bits >> 24);
  p[5] = static_cast<u8>(bits >> 16);
  p[6] = static_cast<u8>(bits >> 8);
  p[7] = static_cast<u8>(bits);
}

void ApplyDolphinAspect()
{
  const AspectChoice& a = kAspects[s_aspect];
  if (std::string_view(a.name) == "4:3")
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::ForceStandard);
  else if (std::string_view(a.name) == "16:9")
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::ForceWide);
  else
  {
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::Custom);
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH, a.width);
    Config::SetBase(Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT, a.height);
  }
  Config::SetBase(Config::GFX_WIDESCREEN_HACK, false);
}

// ----- ModernGekko's own config.ini sets the internal resolution at boot ---------------
void SaveRuntimeResolution(int scale)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "config.ini";
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);)
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.rfind("resolution=", 0) == 0)
      line = fmt::format("resolution={}x{}", 640 * scale, 528 * scale);
    lines.push_back(line);
  }
  in.close();
  std::ofstream out(path, std::ios::trunc);
  for (const auto& line : lines)
    out << line << '\n';
}

// ----- Settings list ---------------------------------------------------------------------
struct Item
{
  std::string label;
  std::string value;       // empty for actions and headers
  bool header = false;
  bool action = false;
  std::function<void(int)> change;  // -1/+1 from Left/Right, +1 from Accept
};

int CurrentAa()
{
  const u32 samples = Config::Get(Config::GFX_MSAA);
  const bool ssaa = Config::Get(Config::GFX_SSAA);
  for (size_t i = 0; i < kAa.size(); ++i)
    if (kAa[i].samples == samples && (kAa[i].ssaa == ssaa || samples <= 1))
      return static_cast<int>(i);
  return 0;
}

int CurrentShader()
{
  const std::string shader = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  for (size_t i = 0; i < kShaders.size(); ++i)
    if (shader == kShaders[i].file)
      return static_cast<int>(i);
  return 0;
}

float CasSharpness()
{
  if (!g_presenter || !g_presenter->GetPostProcessor())
    return 0.5f;
  auto* config = g_presenter->GetPostProcessor()->GetConfig();
  auto& options = config->GetOptions();
  const auto it = options.find("CAS_SHARPNESS");
  if (it == options.end() || it->second.m_float_values.empty())
    return 0.5f;
  return it->second.m_float_values[0];
}

void SetCasSharpness(float value)
{
  if (!g_presenter || !g_presenter->GetPostProcessor())
    return;
  auto* config = g_presenter->GetPostProcessor()->GetConfig();
  config->SetOptionf("CAS_SHARPNESS", 0, value);
  config->SaveOptionsConfiguration();
}

std::vector<Item> BuildItems()
{
  std::vector<Item> items;
  auto header = [&](const char* text) { items.push_back({text, "", true, false, nullptr}); };
  auto toggle = [&](const char* label, const Config::Info<bool>& info) {
    items.push_back({label, Config::Get(info) ? "On" : "Off", false, false, [&info](int) {
                       Config::SetBase(info, !Config::Get(info));
                       s_dirty = true;
                     }});
  };

  header("Graphics");
  {
    const int scale = std::max(1, Config::Get(Config::GFX_EFB_SCALE));
    items.push_back({"Internal resolution",
                     fmt::format("{}x  ({}x{})", scale, 640 * scale, 528 * scale), false, false,
                     [scale](int dir) {
                       const int next = std::clamp(scale + dir, 1, 8);
                       Config::SetBase(Config::GFX_EFB_SCALE, next);
                       SaveRuntimeResolution(next);
                       s_dirty = true;
                     }});
  }
  {
    const int aa = CurrentAa();
    items.push_back({"Anti-aliasing", kAa[aa].label, false, false, [aa](int dir) {
                       const AaChoice& next = kAa[Wrap(aa + dir, kAa)];
                       Config::SetBase(Config::GFX_MSAA, next.samples);
                       Config::SetBase(Config::GFX_SSAA, next.ssaa);
                       s_dirty = true;
                     }});
  }
  {
    const int af = static_cast<int>(Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY));
    const std::string label = af < 0 ? "Game default" : fmt::format("{}x", 1 << af);
    items.push_back({"Anisotropic filtering", label, false, false, [af](int dir) {
                       const int next = std::clamp(af + dir, 0, 4);
                       Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                                       static_cast<AnisotropicFilteringMode>(next));
                       s_dirty = true;
                     }});
  }
  {
    const int sh = CurrentShader();
    items.push_back({"Sharpening / upscaling", kShaders[sh].label, false, false, [sh](int dir) {
                       Config::SetBase(Config::GFX_ENHANCE_POST_SHADER,
                                       std::string(kShaders[Wrap(sh + dir, kShaders)].file));
                       s_dirty = true;
                     }});
    if (std::string_view(kShaders[sh].file) == "FidelityFX_CAS")
    {
      const float sharp = CasSharpness();
      items.push_back({"    CAS sharpness", fmt::format("{:.0f}%", sharp * 100.0f), false, false,
                       [sharp](int dir) {
                         SetCasSharpness(std::clamp(sharp + 0.1f * dir, 0.0f, 1.0f));
                       }});
    }
  }
  toggle("Per-pixel lighting", Config::GFX_ENABLE_PIXEL_LIGHTING);
  toggle("HD texture packs", Config::GFX_HIRES_TEXTURES);
  items.push_back({"Dump textures for HD pack", Config::Get(Config::GFX_DUMP_TEXTURES) ? "On" : "Off",
                   false, false, [](int) {
                     const bool on = !Config::Get(Config::GFX_DUMP_TEXTURES);
                     Config::SetBase(Config::GFX_DUMP_TEXTURES, on);
                     // Dolphin turns dumping off at every start; remember it ourselves.
                     WriteGameOption("DumpTextures", on ? "1" : "0");
                     Config::SetBase(Config::GFX_DUMP_BASE_TEXTURES, true);
                     Config::SetBase(Config::GFX_DUMP_MIP_TEXTURES, false);
                     if (on)
                       SetStatus("Play normally; then run UpscaleTextures.exe");
                     s_dirty = true;
                   }});

  header("Game");
  items.push_back({"Frame rate", s_fps60 ? "60 fps" : "30 fps (original)", false, false, [](int) {
                     s_fps60 = !s_fps60;
                     WriteGameOption("FrameRate", s_fps60 ? "60" : "30");
                     PokeGameSettings();
                   }});
  items.push_back({"Aspect ratio", kAspects[s_aspect].name, false, false, [](int dir) {
                     s_aspect = Wrap(s_aspect + dir, kAspects);
                     WriteGameOption("AspectRatio", kAspects[s_aspect].name);
                     PokeGameSettings();
                     ApplyDolphinAspect();
                     s_dirty = true;
                   }});

  header("Display");
  if (s_hooks.toggle_fullscreen)
  {
    const bool fs = s_hooks.is_fullscreen ? s_hooks.is_fullscreen() : false;
    items.push_back({"Fullscreen", fs ? "On" : "Off", false, false,
                     [](int) { s_hooks.toggle_fullscreen(); }});
  }
  toggle("VSync", Config::GFX_VSYNC);
  toggle("Show FPS", Config::GFX_SHOW_FPS);

  header("Game state");
  items.push_back({"State slot", fmt::format("{}", s_state_slot), false, false,
                   [](int dir) { s_state_slot = std::clamp(s_state_slot + dir, 1, 9); }});
  items.push_back({"Save state", "", false, true, [](int) {
                     State::Save(Core::System::GetInstance(), s_state_slot);
                     SetStatus(fmt::format("Saved state {}", s_state_slot));
                   }});
  items.push_back({"Load state", "", false, true, [](int) {
                     State::Load(Core::System::GetInstance(), s_state_slot);
                     SetStatus(fmt::format("Loaded state {}", s_state_slot));
                     s_open = false;
                   }});
  items.push_back({"Resume", "", false, true, [](int) { s_open = false; }});
  if (s_hooks.quit)
    items.push_back({"Quit game", "", false, true, [](int) { s_hooks.quit(); }});
  return items;
}

void Close()
{
  s_open = false;
  if (s_dirty)
  {
    Config::Save();
    s_dirty = false;
  }
}

void HandleKey(Key key, std::vector<Item>& items)
{
  auto selectable = [&](int i) { return i >= 0 && i < static_cast<int>(items.size()) && !items[i].header; };
  auto move = [&](int dir) {
    int i = s_selected;
    for (size_t n = 0; n < items.size(); ++n)
    {
      i = Wrap(i + dir, items);
      if (selectable(i))
      {
        s_selected = i;
        return;
      }
    }
  };
  if (!selectable(s_selected))
    move(1);

  switch (key)
  {
  case Key::Toggle:
  case Key::Back:
    Close();
    break;
  case Key::Up:
    move(-1);
    break;
  case Key::Down:
    move(1);
    break;
  case Key::Left:
  case Key::Right:
    if (selectable(s_selected) && items[s_selected].change && !items[s_selected].action)
      items[s_selected].change(key == Key::Left ? -1 : 1);
    break;
  case Key::Accept:
    if (selectable(s_selected) && items[s_selected].change)
      items[s_selected].change(1);
    break;
  }
}

// ----- Controller navigation (XInput, read directly so the closed input gate doesn't
// block it) --------------------------------------------------------------------------------
struct PadButton
{
  const char* input;
  Key key;
  bool repeat;
  bool down = false;
  std::chrono::steady_clock::time_point next;
};
std::array<PadButton, 7> s_pad{{
    {"Pad N", Key::Up, true},
    {"Pad S", Key::Down, true},
    {"Pad W", Key::Left, true},
    {"Pad E", Key::Right, true},
    {"Button A", Key::Accept, false},
    {"Button B", Key::Back, false},
    {"Back", Key::Toggle, false},
}};
bool s_stick_down[4] = {};
std::chrono::steady_clock::time_point s_stick_next[4];

void PollPad(std::vector<Key>& out)
{
  ciface::Core::DeviceQualifier q;
  q.FromString("XInput/0/Gamepad");
  const auto device = g_controller_interface.FindDevice(q);
  if (!device)
    return;
  const auto now = std::chrono::steady_clock::now();
  using namespace std::chrono_literals;

  auto edge = [&](bool pressed, bool& down, std::chrono::steady_clock::time_point& next, bool repeat,
                  Key key) {
    if (pressed && !down)
    {
      out.push_back(key);
      next = now + 400ms;
    }
    else if (pressed && repeat && now >= next)
    {
      out.push_back(key);
      next = now + 110ms;
    }
    down = pressed;
  };

  for (auto& b : s_pad)
  {
    const auto* input = device->FindInput(b.input);
    edge(input && input->GetState() > 0.5, b.down, b.next, b.repeat, b.key);
  }
  static constexpr const char* sticks[4] = {"Left Y+", "Left Y-", "Left X-", "Left X+"};
  static constexpr Key stick_keys[4] = {Key::Up, Key::Down, Key::Left, Key::Right};
  for (int i = 0; i < 4; ++i)
  {
    const auto* input = device->FindInput(sticks[i]);
    edge(input && input->GetState() > 0.6, s_stick_down[i], s_stick_next[i], true, stick_keys[i]);
  }
}
}  // namespace

bool OnKey(Key key)
{
  if (key != Key::Toggle && !s_open)
    return false;
  std::lock_guard lock(s_mutex);
  s_keys.push_back(key);
  return true;
}

bool IsOpen()
{
  return s_open;
}

void SetHostHooks(HostHooks hooks)
{
  std::lock_guard lock(s_mutex);
  s_hooks = std::move(hooks);
}

void Draw()
{
  if (!s_hint_shown)
  {
    s_hint_shown = true;
    LoadGameOptions();
    if (ReadGameOption("DumpTextures", "0") == "1")
    {
      Config::SetBase(Config::GFX_DUMP_TEXTURES, true);
      Config::SetBase(Config::GFX_DUMP_BASE_TEXTURES, true);
      Config::SetBase(Config::GFX_DUMP_MIP_TEXTURES, false);
    }
    OSD::AddMessage("Settings: Esc or F1 (controller: View button)", 8000);
  }

  std::vector<Key> keys;
  {
    std::lock_guard lock(s_mutex);
    keys.assign(s_keys.begin(), s_keys.end());
    s_keys.clear();
  }
  PollPad(keys);

  for (Key key : keys)
  {
    if (!s_open)
    {
      if (key == Key::Toggle)
      {
        s_open = true;
        LoadGameOptions();
      }
      continue;
    }
    std::vector<Item> items = BuildItems();
    HandleKey(key, items);
  }
  if (!s_open)
    return;

  const std::vector<Item> items = BuildItems();
  const ImGuiIO& io = ImGui::GetIO();
  const float scale = std::max(1.0f, io.DisplaySize.y / 600.0f);

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(620.0f * scale, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale, 14.0f * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
  if (ImGui::Begin("Colosseum Native", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing))
  {
    ImGui::SetWindowFontScale(scale);
    if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingStretchProp))
    {
      for (size_t i = 0; i < items.size(); ++i)
      {
        const Item& item = items[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (item.header)
        {
          ImGui::Spacing();
          ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "%s", item.label.c_str());
          continue;
        }
        const bool selected = static_cast<int>(i) == s_selected;
        if (selected)
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                 ImGui::GetColorU32(ImVec4(0.25f, 0.45f, 0.85f, 0.55f)));
        ImGui::TextUnformatted(item.label.c_str());
        ImGui::TableSetColumnIndex(1);
        if (!item.value.empty())
          ImGui::Text(selected ? "<  %s  >" : "   %s", item.value.c_str());
      }
      ImGui::EndTable();
    }
    ImGui::Separator();
    if (!s_status.empty() && std::chrono::steady_clock::now() < s_status_until)
      ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s", s_status.c_str());
    ImGui::TextDisabled("Up/Down select   Left/Right change   Enter/A choose   Esc/B close");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}
}  // namespace moderngekko::menu
