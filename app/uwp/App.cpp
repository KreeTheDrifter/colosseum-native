// Colosseum Native for Xbox (UWP, Developer Mode).
//
// A CoreWindow app with no XAML. On first launch it shows a setup screen (Dear ImGui on a small
// D3D11 swap chain) to pick the player's Pokemon Colosseum (USA) disc image from a USB drive and
// unpack it into the app's LocalState folder. Then it writes Xbox defaults, patches main.dol and
// runs the game through the ModernGekko runtime: Dolphin's D3D12 renderer on the CoreWindow,
// XAudio2 sound, Windows.Gaming.Input controllers, and the recompiled game code
// (gGC6E01_recomp.dll, shipped in the package).

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <roapi.h>
#include <windows.applicationmodel.core.h>
#include <windows.applicationmodel.h>
#include <windows.gaming.input.h>
#include <windows.storage.h>
#include <windows.ui.core.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "Common/IniFile.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "game_menu.hpp"
#include "moderngekko/runtime.hpp"

#include "disc_setup.h"
#include "winrt_extra.h"

// fileapifromapp.h (Windows SDK); MinGW has the import library entries but not the header.
extern "C" {
WINBASEAPI HANDLE WINAPI CreateFile2FromAppW(LPCWSTR file_name, DWORD desired_access,
                                             DWORD share_mode, DWORD creation_disposition,
                                             LPCREATEFILE2_EXTENDED_PARAMETERS params);
WINBASEAPI HANDLE WINAPI FindFirstFileExFromAppW(LPCWSTR file_name, FINDEX_INFO_LEVELS info_level,
                                                 LPVOID find_data, FINDEX_SEARCH_OPS search_op,
                                                 LPVOID search_filter, DWORD additional_flags);
}

// hidclass.h; referenced by the (unused on Xbox) real-Wiimote scanner.
extern "C" const GUID GUID_DEVINTERFACE_HID = {
    0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace AppCore = ABI::Windows::ApplicationModel::Core;
namespace UICore = ABI::Windows::UI::Core;
namespace WGI = ABI::Windows::Gaming::Input;

namespace
{
// ----- Shared state -----------------------------------------------------------------------

ComPtr<UICore::ICoreWindow> g_window;
std::atomic<bool> g_window_closed{false};
std::atomic<bool> g_activated{false};
UINT32 g_width = 1920;
UINT32 g_height = 1080;
FILE* g_log = nullptr;

void Log(const char* fmt, ...)
{
  if (!g_log)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(g_log, fmt, args);
  va_end(args);
  fputc('\n', g_log);
  fflush(g_log);
}

std::string Narrow(HSTRING s)
{
  UINT32 len = 0;
  const wchar_t* w = WindowsGetStringRawBuffer(s, &len);
  return WStringToUTF8(std::wstring(w, len));
}

void PumpEvents()
{
  ComPtr<UICore::ICoreDispatcher> dispatcher;
  if (g_window && SUCCEEDED(g_window->get_Dispatcher(&dispatcher)))
    dispatcher->ProcessEvents(UICore::CoreProcessEventsOption_ProcessAllIfPresent);
}

// ----- Minimal COM helpers ----------------------------------------------------------------

// A delegate (event handler / async callback): IUnknown + Invoke(sender, args). WinRT may ask
// for the delegate's own parameterized IID, which we answer with ourselves.
class Delegate final : public IUnknown
{
public:
  using Fn = std::function<HRESULT(IInspectable*, IInspectable*)>;
  explicit Delegate(Fn fn) : m_fn(std::move(fn)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
  {
    if (iid == __uuidof(IMarshal))
    {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    *out = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG r = --m_refs;
    if (r == 0)
      delete this;
    return r;
  }
  virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args)
  {
    return m_fn(sender, args);
  }

private:
  std::atomic<ULONG> m_refs{1};
  Fn m_fn;
};

template <typename Interface>
class Inspectable : public Interface
{
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
  {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(Interface) ||
        iid == __uuidof(IAgileObject))
    {
      *out = static_cast<Interface*>(this);
      this->AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const ULONG r = --m_refs;
    if (r == 0)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override
  {
    *count = 0;
    *iids = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override
  {
    *name = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override
  {
    *level = BaseTrust;
    return S_OK;
  }
  virtual ~Inspectable() = default;

private:
  std::atomic<ULONG> m_refs{1};
};

template <typename T>
ComPtr<T> Factory(const wchar_t* runtime_class)
{
  ComPtr<T> factory;
  RoGetActivationFactory(HStringReference(runtime_class).Get(), __uuidof(T), &factory);
  return factory;
}

// Waits for an IAsyncOperation while keeping the window and a frame callback going.
bool WaitAsync(IInspectable* op, const std::function<void()>& frame)
{
  ComPtr<xwinrt::IAsyncInfo> info;
  if (FAILED(op->QueryInterface(__uuidof(xwinrt::IAsyncInfo), &info)))
    return false;
  for (;;)
  {
    INT32 status = 0;
    info->get_Status(&status);
    if (status != 0)
      return status == 1;
    PumpEvents();
    if (frame)
      frame();
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_window_closed)
      return false;
  }
}

fs::path ItemPath(IInspectable* item)
{
  ComPtr<ABI::Windows::Storage::IStorageItem> storage_item;
  HSTRING path = nullptr;
  if (!item || FAILED(item->QueryInterface(IID_PPV_ARGS(&storage_item))) ||
      FAILED(storage_item->get_Path(&path)))
    return {};
  const std::string s = Narrow(path);
  WindowsDeleteString(path);
  return fs::u8path(s);
}

fs::path LocalStatePath()
{
  auto statics = Factory<ABI::Windows::Storage::IApplicationDataStatics>(
      RuntimeClass_Windows_Storage_ApplicationData);
  ComPtr<ABI::Windows::Storage::IApplicationData> data;
  ComPtr<ABI::Windows::Storage::IStorageFolder> folder;
  if (!statics || FAILED(statics->get_Current(&data)) || FAILED(data->get_LocalFolder(&folder)))
    return {};
  return ItemPath(folder.Get());
}

fs::path InstallPath()
{
  auto statics = Factory<ABI::Windows::ApplicationModel::IPackageStatics>(
      RuntimeClass_Windows_ApplicationModel_Package);
  ComPtr<ABI::Windows::ApplicationModel::IPackage> package;
  ComPtr<ABI::Windows::Storage::IStorageFolder> folder;
  if (!statics || FAILED(statics->get_Current(&package)) ||
      FAILED(package->get_InstalledLocation(&folder)))
    return {};
  return ItemPath(folder.Get());
}

void ComputeOutputSize()
{
  // The TV's mode (e.g. 3840x2160) on Xbox; the window bounds elsewhere.
  if (auto hdmi = Factory<xwinrt::IHdmiDisplayInformationStatics>(
          L"Windows.Graphics.Display.Core.HdmiDisplayInformation"))
  {
    ComPtr<IInspectable> info_insp, mode_insp;
    ComPtr<xwinrt::IHdmiDisplayInformation> info;
    ComPtr<xwinrt::IHdmiDisplayMode> mode;
    if (SUCCEEDED(hdmi->GetForCurrentView(&info_insp)) && info_insp &&
        SUCCEEDED(info_insp.As(&info)) && SUCCEEDED(info->GetCurrentDisplayMode(&mode_insp)) &&
        mode_insp && SUCCEEDED(mode_insp.As(&mode)))
    {
      UINT32 w = 0, h = 0;
      mode->get_ResolutionWidthInRawPixels(&w);
      mode->get_ResolutionHeightInRawPixels(&h);
      if (w >= 640 && h >= 480)
      {
        g_width = w;
        g_height = h;
        Log("display mode %ux%u", w, h);
        return;
      }
    }
  }
  ABI::Windows::Foundation::Rect bounds{};
  if (g_window && SUCCEEDED(g_window->get_Bounds(&bounds)) && bounds.Width > 0)
  {
    g_width = UINT32(bounds.Width);
    g_height = UINT32(bounds.Height);
  }
  Log("window %ux%u", g_width, g_height);
}

// ----- Controller (setup screen) ----------------------------------------------------------

struct PadState
{
  UINT32 buttons = 0;
  double lx = 0, ly = 0;
};

PadState ReadPads()
{
  static ComPtr<WGI::IGamepadStatics> statics =
      Factory<WGI::IGamepadStatics>(RuntimeClass_Windows_Gaming_Input_Gamepad);
  PadState state;
  ComPtr<ABI::Windows::Foundation::Collections::IVectorView<WGI::Gamepad*>> pads;
  if (!statics || FAILED(statics->get_Gamepads(&pads)) || !pads)
    return state;
  unsigned count = 0;
  pads->get_Size(&count);
  for (unsigned i = 0; i < count; ++i)
  {
    ComPtr<WGI::IGamepad> pad;
    WGI::GamepadReading r{};
    if (SUCCEEDED(pads->GetAt(i, &pad)) && pad && SUCCEEDED(pad->GetCurrentReading(&r)))
    {
      state.buttons |= static_cast<UINT32>(r.Buttons);
      if (std::abs(r.LeftThumbstickX) > std::abs(state.lx))
        state.lx = r.LeftThumbstickX;
      if (std::abs(r.LeftThumbstickY) > std::abs(state.ly))
        state.ly = r.LeftThumbstickY;
    }
  }
  return state;
}

// ----- Setup screen (Dear ImGui on D3D11) -------------------------------------------------

class SetupScreen
{
public:
  bool Init()
  {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
                                   &m_device, nullptr, &m_context);
    if (FAILED(hr))
    {
      Log("D3D11CreateDevice failed %08x", unsigned(hr));
      return false;
    }
    ComPtr<IDXGIDevice1> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    m_device.As(&dxgi_device);
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = g_width;
    desc.Height = g_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    hr = factory->CreateSwapChainForCoreWindow(m_device.Get(), g_window.Get(), &desc, nullptr,
                                               &m_swap);
    if (FAILED(hr))
    {
      Log("CreateSwapChainForCoreWindow (setup) failed %08x", unsigned(hr));
      return false;
    }
    ComPtr<ID3D11Texture2D> back;
    m_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    m_device->CreateRenderTargetView(back.Get(), nullptr, &m_rtv);

    IMGUI_CHECKVERSION();
    m_imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.DisplaySize = ImVec2(float(g_width), float(g_height));
    const float scale = float(g_height) / 540.0f;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale * 1.15f;
    style.WindowRounding = 6.0f * scale;
    style.FrameRounding = 4.0f * scale;
    io.Fonts->AddFontDefault();
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());
    m_last = std::chrono::steady_clock::now();
    return true;
  }

  void Shutdown()
  {
    if (m_imgui)
    {
      ImGui_ImplDX11_Shutdown();
      ImGui::DestroyContext(m_imgui);
      m_imgui = nullptr;
    }
    m_rtv.Reset();
    if (m_context)
    {
      m_context->ClearState();
      m_context->Flush();
    }
    m_swap.Reset();
    m_context.Reset();
    m_device.Reset();
  }

  // Draws one frame; `ui` builds the window contents.
  void Frame(const std::function<void()>& ui)
  {
    ImGuiIO& io = ImGui::GetIO();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(1e-4f, std::chrono::duration<float>(now - m_last).count());
    m_last = now;

    const PadState pad = ReadPads();
    auto key = [&](ImGuiKey k, bool down) { io.AddKeyEvent(k, down); };
    key(ImGuiKey_GamepadDpadUp, (pad.buttons & WGI::GamepadButtons_DPadUp) || pad.ly > 0.5);
    key(ImGuiKey_GamepadDpadDown, (pad.buttons & WGI::GamepadButtons_DPadDown) || pad.ly < -0.5);
    key(ImGuiKey_GamepadDpadLeft, (pad.buttons & WGI::GamepadButtons_DPadLeft) || pad.lx < -0.5);
    key(ImGuiKey_GamepadDpadRight, (pad.buttons & WGI::GamepadButtons_DPadRight) || pad.lx > 0.5);
    key(ImGuiKey_GamepadFaceDown, pad.buttons & WGI::GamepadButtons_A);
    key(ImGuiKey_GamepadFaceRight, pad.buttons & WGI::GamepadButtons_B);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    const float w = float(g_width), h = float(g_height);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w * 0.62f, 0), ImGuiCond_Always);
    ImGui::Begin("##setup", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    ui();
    ImGui::End();
    ImGui::Render();

    const float clear[4] = {0.07f, 0.08f, 0.11f, 1.0f};
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_swap->Present(1, 0);
  }

private:
  ComPtr<ID3D11Device> m_device;
  ComPtr<ID3D11DeviceContext> m_context;
  ComPtr<IDXGISwapChain1> m_swap;
  ComPtr<ID3D11RenderTargetView> m_rtv;
  ImGuiContext* m_imgui = nullptr;
  std::chrono::steady_clock::time_point m_last;
};

// ----- Disc sources -----------------------------------------------------------------------

disc_setup::ReadAt HandleReader(HANDLE file)
{
  return [file](uint64_t offset, void* out, size_t size) {
    auto* dst = static_cast<uint8_t*>(out);
    while (size)
    {
      OVERLAPPED ov{};
      ov.Offset = DWORD(offset);
      ov.OffsetHigh = DWORD(offset >> 32);
      DWORD got = 0;
      const DWORD want = DWORD(std::min<size_t>(size, 1u << 24));
      if (!ReadFile(file, dst, want, &got, &ov) || got == 0)
        return false;
      dst += got;
      offset += got;
      size -= got;
    }
    return true;
  };
}

// Disc images in the root (or one folder down) of the console's other drives: USB drives show
// up as drive letters (usually E: and up) to apps with removable-storage access.
std::vector<fs::path> ScanDrives()
{
  std::vector<fs::path> found;
  auto is_image = [](const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (const wchar_t* ext : {L".iso", L".gcm", L".ciso"})
    {
      const size_t n = wcslen(ext);
      if (lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0)
        return true;
    }
    return false;
  };
  std::function<void(const std::wstring&, int)> scan = [&](const std::wstring& dir, int depth) {
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileExFromAppW((dir + L"*").c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
      return;
    do
    {
      const std::wstring name = data.cFileName;
      if (name == L"." || name == L"..")
        continue;
      if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      {
        if (depth < 1)
          scan(dir + name + L"\\", depth + 1);
      }
      else if (is_image(name) && found.size() < 12)
      {
        found.push_back(fs::path(dir + name));
      }
    } while (FindNextFileW(h, &data));
    FindClose(h);
  };
  for (wchar_t letter = L'D'; letter <= L'Z'; ++letter)
    scan(std::wstring(1, letter) + L":\\", 0);
  return found;
}

// ----- Settings ---------------------------------------------------------------------------

void EnsureIni(const fs::path& path, const std::string& section, const std::string& key,
               const std::string& value, bool overwrite)
{
  Common::IniFile ini;
  ini.Load(path.string());
  auto* s = ini.GetOrCreateSection(section);
  if (!overwrite && s->Exists(key))
    return;
  s->Set(key, value);
  ini.Save(path.string());
}

std::string ReadIni(const fs::path& path, const std::string& section, const std::string& key,
                    const std::string& fallback)
{
  Common::IniFile ini;
  ini.Load(path.string());
  std::string value;
  ini.GetOrCreateSection(section)->Get(key, &value, fallback);
  return value;
}

void WriteXboxDefaults(const fs::path& user)
{
  std::error_code ec;
  fs::create_directories(user / "Config", ec);
  fs::create_directories(user / "GC", ec);
  fs::create_directories(user / "StateSaves", ec);
  const fs::path dolphin = user / "Config" / "Dolphin.ini";
  const fs::path gfx = user / "Config" / "GFX.ini";
  const fs::path game = user / "Config" / "ColosseumNative.ini";

  const bool first_run = !fs::exists(gfx, ec);
  if (first_run)
  {
    // Series X/S: 3x internal resolution (1920x1584), 2x MSAA, 8x anisotropic, per-pixel
    // lighting, hybrid ubershaders. The in-game menu (View button) has presets.
    std::ofstream(gfx) << "[Settings]\n"
                          "InternalResolution = 3\n"
                          "MSAA = 0x00000002\n"
                          "SSAA = False\n"
                          "EnablePixelLighting = True\n"
                          "ShaderCompilationMode = 2\n"
                          "WaitForShadersBeforeStarting = False\n"
                          "HiresTextures = True\n"
                          "ShowFPS = False\n"
                          "[Enhancements]\n"
                          "MaxAnisotropy = 3\n"
                          "ArbitraryMipmapDetection = True\n"
                          "ForceTrueColor = True\n"
                          "DisableCopyFilter = True\n";
  }
  // Always: the game renders 16:9 itself (patched), fonts need the safe texture cache, and
  // CPU culling helps Shadow Pokemon cutscenes.
  EnsureIni(gfx, "Settings", "AspectRatio", "1", false);
  EnsureIni(gfx, "Settings", "wideScreenHack", "False", true);
  EnsureIni(gfx, "Settings", "SafeTextureCacheColorSamples", "0", false);
  EnsureIni(gfx, "Settings", "CPUCull", "True", false);

  EnsureIni(dolphin, "Core", "SlotA", "1", false);
  EnsureIni(dolphin, "Core", "SlotB", "255", false);
  EnsureIni(dolphin, "Core", "MemcardAPath", (user / "GC" / "MemoryCardA.USA.raw").string(),
            true);
  EnsureIni(dolphin, "Core", "CPUThread", "True", false);
  EnsureIni(dolphin, "Core", "StaticRecompIdlePC", "2148527004", true);
  EnsureIni(dolphin, "Interface", "UsePanicHandlers", "False", false);

  EnsureIni(game, "Game", "FrameRate", "60", false);
  EnsureIni(game, "Game", "AspectRatio", "16:9", false);

  // Controller: the Xbox pad as a GameCube controller (same layout as the PC version).
  const fs::path pad = user / "Config" / "GCPadNew.ini";
  if (!fs::exists(pad, ec))
  {
    std::ofstream(pad) << "[GCPad1]\n"
                          "Device = XInput/0/Gamepad\n"
                          "Buttons/A = `Button A`\n"
                          "Buttons/B = `Button X`\n"
                          "Buttons/X = `Button B`\n"
                          "Buttons/Y = `Button Y`\n"
                          "Buttons/Z = `Shoulder R`\n"
                          "Buttons/Start = `Start`\n"
                          "Main Stick/Up = `Left Y+`\n"
                          "Main Stick/Down = `Left Y-`\n"
                          "Main Stick/Left = `Left X-`\n"
                          "Main Stick/Right = `Left X+`\n"
                          "Main Stick/Dead Zone = 10.0\n"
                          "C-Stick/Up = `Right Y+`\n"
                          "C-Stick/Down = `Right Y-`\n"
                          "C-Stick/Left = `Right X-`\n"
                          "C-Stick/Right = `Right X+`\n"
                          "C-Stick/Dead Zone = 10.0\n"
                          "Triggers/L = `Trigger L`\n"
                          "Triggers/R = `Trigger R`\n"
                          "Triggers/L-Analog = `Trigger L`\n"
                          "Triggers/R-Analog = `Trigger R`\n"
                          "D-Pad/Up = `Pad N`\n"
                          "D-Pad/Down = `Pad S`\n"
                          "D-Pad/Left = `Pad W`\n"
                          "D-Pad/Right = `Pad E`\n"
                          "Rumble/Motor = `Motor L` | `Motor R`\n";
  }
}

// ----- Platform for the runtime ----------------------------------------------------------

class PlatformUWP final : public Platform
{
public:
  bool Init() override
  {
    moderngekko::menu::HostHooks hooks;
    hooks.quit = [this] { RequestShutdown(); };
    moderngekko::menu::SetHostHooks(std::move(hooks));
    return true;
  }
  void SetTitle(const std::string&) override {}
  void MainLoop() override
  {
    while (IsRunning())
    {
      UpdateRunningFlag();
      Core::HostDispatchJobs(Core::System::GetInstance());
      PumpEvents();
      if (g_window_closed)
        RequestShutdown();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  WindowSystemInfo GetWindowSystemInfo() const override
  {
    WindowSystemInfo wsi;
    wsi.type = WindowSystemType::UWP;
    wsi.render_window = static_cast<void*>(g_window.Get());
    wsi.render_surface = static_cast<void*>(static_cast<IUnknown*>(g_window.Get()));
    wsi.render_width = g_width;
    wsi.render_height = g_height;
    return wsi;
  }
};

// ----- The app ----------------------------------------------------------------------------

enum class Stage
{
  Welcome,
  Picking,
  Unpacking,
  Error,
  Done,
};

class Game
{
public:
  void Run()
  {
    const fs::path local = LocalStatePath();
    const fs::path install = InstallPath();
    g_log = _wfopen((local / "ColosseumNative.log").wstring().c_str(), L"w");
    Log("local %s", local.string().c_str());
    Log("install %s", install.string().c_str());
    m_game = local / "Game";
    m_user = local / "User";

    if (!disc_setup::GameReady(m_game) && !RunSetup())
      return;

    WriteXboxDefaults(m_user);
    const fs::path game_ini = m_user / "Config" / "ColosseumNative.ini";
    const std::string fps = ReadIni(game_ini, "Game", "FrameRate", "60");
    const std::string aspect = ReadIni(game_ini, "Game", "AspectRatio", "16:9");
    const std::string error = disc_setup::PrepareDol(m_game, fps != "30", aspect);
    if (!error.empty())
    {
      Log("prepare: %s", error.c_str());
      m_error = error + " Choose your disc image again.";
      if (!RunSetup(true))
        return;
      disc_setup::PrepareDol(m_game, fps != "30", aspect);
    }

    moderngekko::RuntimeConfig config;
    config.game_root = m_game;
    config.user_directory = m_user;
    config.module = moderngekko::ModuleSource::DynamicPath(install / "gGC6E01_recomp.dll");
    config.graphics.backend = ReadIni(game_ini, "Xbox", "Backend", "D3D12");
    config.audio.backend = "XAudio2";
    config.fullscreen = true;
    config.show_fps_in_title = false;
    config.window_title = "Pokemon Colosseum";
    auto created = moderngekko::Runtime::Create(std::move(config));
    if (!created)
    {
      Log("runtime: %s", created.error ? created.error->message.c_str() : "?");
      ShowError("The game could not start: " +
                (created.error ? created.error->message : std::string("unknown error")));
      return;
    }
    const auto result = created.runtime->Run();
    if (result.error)
      Log("run: %s", result.error->message.c_str());
    created.runtime.reset();
    Log("exit");
  }

private:
  bool RunSetup(bool show_error = false)
  {
    SetupScreen screen;
    if (!screen.Init())
      return false;
    m_stage = show_error ? Stage::Error : Stage::Welcome;
    m_found = ScanDrives();
    std::thread worker;
    bool pick_requested = false;
    fs::path chosen_path;

    while (!g_window_closed && m_stage != Stage::Done)
    {
      PumpEvents();
      screen.Frame([&] { DrawSetup(pick_requested, chosen_path); });

      if (pick_requested)
      {
        pick_requested = false;
        HANDLE file = PickWithPicker(screen);
        if (file != INVALID_HANDLE_VALUE)
          StartUnpack(worker, file);
        else if (m_stage == Stage::Picking)
          m_stage = Stage::Welcome;
      }
      if (!chosen_path.empty())
      {
        HANDLE file = CreateFile2FromAppW(chosen_path.wstring().c_str(), GENERIC_READ,
                                          FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        chosen_path.clear();
        if (file == INVALID_HANDLE_VALUE)
        {
          m_error = "That file could not be opened.";
          m_stage = Stage::Error;
        }
        else
        {
          StartUnpack(worker, file);
        }
      }
      if (m_stage == Stage::Unpacking && m_unpack_done)
      {
        worker.join();
        m_unpack_done = false;
        std::lock_guard lock(m_mutex);
        m_stage = m_error.empty() ? Stage::Done : Stage::Error;
      }
    }
    if (worker.joinable())
      worker.join();
    screen.Shutdown();
    return m_stage == Stage::Done;
  }

  void StartUnpack(std::thread& worker, HANDLE file)
  {
    m_stage = Stage::Unpacking;
    m_progress = 0;
    m_error.clear();
    worker = std::thread([this, file] {
      const std::string error = disc_setup::SetupGame(HandleReader(file), m_game, m_progress);
      CloseHandle(file);
      Log("setup: %s", error.empty() ? "ok" : error.c_str());
      {
        std::lock_guard lock(m_mutex);
        m_error = error;
      }
      m_unpack_done = true;
    });
  }

  HANDLE PickWithPicker(SetupScreen& screen)
  {
    ComPtr<IInspectable> insp;
    if (FAILED(RoActivateInstance(
            HStringReference(L"Windows.Storage.Pickers.FileOpenPicker").Get(), &insp)))
    {
      m_error = "The file picker isn't available.";
      m_stage = Stage::Error;
      return INVALID_HANDLE_VALUE;
    }
    ComPtr<xwinrt::IFileOpenPicker> picker;
    insp.As(&picker);
    picker->put_ViewMode(0);
    ComPtr<xwinrt::IVectorHStringRaw> filter;
    picker->get_FileTypeFilter(&filter);
    for (const wchar_t* ext : {L".iso", L".gcm", L".ciso"})
      filter->Append(HStringReference(ext).Get());
    ComPtr<xwinrt::IAsyncOperationRaw> op;
    if (FAILED(picker->PickSingleFileAsync(&op)) || !op)
    {
      m_error = "The file picker could not be opened.";
      m_stage = Stage::Error;
      return INVALID_HANDLE_VALUE;
    }
    m_stage = Stage::Picking;
    bool dummy_pick = false;
    fs::path dummy_path;
    if (!WaitAsync(op.Get(), [&] { screen.Frame([&] { DrawSetup(dummy_pick, dummy_path); }); }))
      return INVALID_HANDLE_VALUE;
    ComPtr<IInspectable> file;
    if (FAILED(op->GetResults(&file)) || !file)
      return INVALID_HANDLE_VALUE;  // canceled
    Log("picked %s", ItemPath(file.Get()).string().c_str());
    ComPtr<xwinrt::IStorageItemHandleAccess> access;
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (FAILED(file.As(&access)) ||
        FAILED(access->Create(xwinrt::HAO_READ, xwinrt::HSO_SHARE_READ, xwinrt::HO_RANDOM_ACCESS,
                              nullptr, &handle)))
    {
      m_error = "That file could not be opened.";
      m_stage = Stage::Error;
      return INVALID_HANDLE_VALUE;
    }
    return handle;
  }

  void DrawSetup(bool& pick, fs::path& chosen)
  {
    ImGui::TextUnformatted("Pokemon Colosseum");
    ImGui::Separator();
    switch (m_stage)
    {
    case Stage::Welcome:
    case Stage::Error:
      if (m_stage == Stage::Error)
      {
        std::lock_guard lock(m_mutex);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
      }
      ImGui::TextWrapped("This needs your own Pokemon Colosseum (USA) disc image: an ISO, GCM or "
                         "CISO file. Put it on a USB drive, plug the drive into the Xbox, and "
                         "choose it below. It is unpacked once (about 1.4 GB).");
      ImGui::Spacing();
      for (const fs::path& p : m_found)
      {
        if (ImGui::Button(("Use " + p.string()).c_str(), ImVec2(-1, 0)))
          chosen = p;
      }
      if (ImGui::Button("Browse for the disc image...", ImVec2(-1, 0)))
        pick = true;
      if (ImGui::Button("Look for USB drives again", ImVec2(-1, 0)))
        m_found = ScanDrives();
      ImGui::Spacing();
      ImGui::TextDisabled("D-pad to move, A to select.");
      break;
    case Stage::Picking:
      ImGui::TextWrapped("Choose the disc image in the file picker.");
      break;
    case Stage::Unpacking:
      ImGui::TextWrapped("Unpacking the game. This takes a minute or two.");
      ImGui::ProgressBar(m_progress / 100.0f, ImVec2(-1, 0));
      break;
    case Stage::Done:
      break;
    }
  }

  void ShowError(const std::string& message)
  {
    SetupScreen screen;
    if (!screen.Init())
      return;
    bool close = false;
    while (!g_window_closed && !close)
    {
      PumpEvents();
      screen.Frame([&] {
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Details are in LocalState\\ColosseumNative.log (Device Portal, "
                           "File explorer).");
        if (ImGui::Button("Exit", ImVec2(-1, 0)))
          close = true;
      });
    }
    screen.Shutdown();
  }

  fs::path m_game, m_user;
  Stage m_stage = Stage::Welcome;
  std::vector<fs::path> m_found;
  std::atomic<int> m_progress{0};
  std::atomic<bool> m_unpack_done{false};
  std::mutex m_mutex;
  std::string m_error;
};

// ----- CoreApplication plumbing -----------------------------------------------------------

class View final : public Inspectable<AppCore::IFrameworkView>
{
public:
  HRESULT STDMETHODCALLTYPE Initialize(AppCore::ICoreApplicationView* view) override
  {
    auto* on_activated = new Delegate([](IInspectable*, IInspectable*) {
      g_activated = true;
      if (g_window)
        g_window->Activate();
      return S_OK;
    });
    EventRegistrationToken token;
    view->add_Activated(reinterpret_cast<ABI::Windows::Foundation::ITypedEventHandler<
                            AppCore::CoreApplicationView*,
                            ABI::Windows::ApplicationModel::Activation::IActivatedEventArgs*>*>(
                            on_activated),
                        &token);
    on_activated->Release();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetWindow(UICore::ICoreWindow* window) override
  {
    g_window = window;
    auto* on_closed = new Delegate([](IInspectable*, IInspectable*) {
      g_window_closed = true;
      return S_OK;
    });
    EventRegistrationToken token;
    window->add_Closed(reinterpret_cast<ABI::Windows::Foundation::ITypedEventHandler<
                           UICore::CoreWindow*, UICore::CoreWindowEventArgs*>*>(on_closed),
                       &token);
    on_closed->Release();

    // The Xbox B button is "back"; keep it from leaving the app.
    if (auto statics = Factory<xwinrt::ISystemNavigationManagerStatics>(
            L"Windows.UI.Core.SystemNavigationManager"))
    {
      ComPtr<IInspectable> insp;
      ComPtr<xwinrt::ISystemNavigationManager> nav;
      if (SUCCEEDED(statics->GetForCurrentView(&insp)) && insp && SUCCEEDED(insp.As(&nav)))
      {
        auto* on_back = new Delegate([](IInspectable*, IInspectable* args) {
          ComPtr<xwinrt::IBackRequestedEventArgs> back;
          if (args && SUCCEEDED(args->QueryInterface(__uuidof(xwinrt::IBackRequestedEventArgs),
                                                     &back)))
            back->put_Handled(true);
          return S_OK;
        });
        INT64 back_token = 0;
        nav->add_BackRequested(on_back, &back_token);
        on_back->Release();
      }
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Load(HSTRING) override { return S_OK; }

  HRESULT STDMETHODCALLTYPE Run() override
  {
    if (g_window)
      g_window->Activate();
    for (int i = 0; i < 200 && !g_activated && !g_window_closed; ++i)
    {
      PumpEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ComputeOutputSize();
    Game game;
    game.Run();
    if (g_log)
      fclose(g_log);
    if (auto exit = Factory<AppCore::ICoreApplicationExit>(
            RuntimeClass_Windows_ApplicationModel_Core_CoreApplication))
      exit->Exit();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Uninitialize() override { return S_OK; }
};

class ViewSource final : public Inspectable<AppCore::IFrameworkViewSource>
{
public:
  HRESULT STDMETHODCALLTYPE CreateView(AppCore::IFrameworkView** view) override
  {
    *view = new View();
    return S_OK;
  }
};
}  // namespace

std::unique_ptr<Platform> Platform::CreateUWPPlatform()
{
  return std::make_unique<PlatformUWP>();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  if (FAILED(RoInitialize(RO_INIT_MULTITHREADED)))
    return 1;
  auto app = Factory<AppCore::ICoreApplication>(
      RuntimeClass_Windows_ApplicationModel_Core_CoreApplication);
  if (!app)
    return 1;
  auto* source = new ViewSource();
  app->Run(source);
  source->Release();
  RoUninitialize();
  return 0;
}
