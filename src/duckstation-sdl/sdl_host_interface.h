#pragma once
#include "common/gl/program.h"
#include "common/gl/texture.h"
#include "core/host_display.h"
#include "core/host_interface.h"
#include <SDL.h>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class System;
class AudioStream;

class Controller;

class SDLHostInterface final : public HostInterface
{
public:
  SDLHostInterface();
  ~SDLHostInterface();

  static std::unique_ptr<SDLHostInterface> Create(const char* filename = nullptr, const char* exp1_filename = nullptr,
                                                  const char* save_state_filename = nullptr);

  static std::string GetSaveStateFilename(u32 index);

  void ReportError(const char* message) override;
  void ReportMessage(const char* message) override;

  void Run();

private:
  enum class KeyboardControllerAction
  {
    Up,
    Down,
    Left,
    Right,
    Triangle,
    Cross,
    Square,
    Circle,
    L1,
    R1,
    L2,
    R2,
    Start,
    Select,
    Count
  };

  using KeyboardControllerActionMap = std::array<s32, static_cast<int>(KeyboardControllerAction::Count)>;

  struct ControllerData
  {
    SDL_GameController* controller;
    SDL_Haptic* haptic;
    u32 controller_index;
    float last_rumble_strength;
  };

  static constexpr u32 NUM_QUICK_SAVE_STATES = 10;
  static constexpr char RESUME_SAVESTATE_FILENAME[] = "savestate_resume.bin";

  bool HasSystem() const { return static_cast<bool>(m_system); }

#ifdef WIN32
  bool UseOpenGLRenderer() const { return m_settings.gpu_renderer == GPURenderer::HardwareOpenGL; }
#else
  bool UseOpenGLRenderer() const { return true; }
#endif

  bool CreateSDLWindow();
  void DestroySDLWindow();
  bool CreateDisplay();
  void DestroyDisplay();
  void CreateImGuiContext();
  void CreateAudioStream();

  void SaveSettings();

  void QueueSwitchGPURenderer();
  void SwitchGPURenderer();
  void SwitchAudioBackend();
  void UpdateFullscreen();
  void UpdateControllerMapping();

  // We only pass mouse input through if it's grabbed
  void DrawImGui();
  void DoPowerOff();
  void DoResume();
  void DoStartDisc();
  void DoStartBIOS();
  void DoChangeDisc();
  void DoLoadState(u32 index);
  void DoSaveState(u32 index);
  void DoTogglePause();
  void DoFrameStep();
  void DoToggleFullscreen();

  void HandleSDLEvent(const SDL_Event* event);
  void HandleSDLKeyEvent(const SDL_Event* event);

  void UpdateKeyboardControllerMapping();
  bool HandleSDLKeyEventForController(const SDL_Event* event);

  bool OpenGameController(int index);
  bool CloseGameController(int index);
  void CloseGameControllers();
  void UpdateControllerControllerMapping();
  void HandleSDLControllerAxisEventForController(const SDL_Event* event);
  void HandleSDLControllerButtonEventForController(const SDL_Event* event);
  void UpdateControllerRumble();

  void DrawMainMenuBar();
  void DrawQuickSettingsMenu();
  void DrawDebugMenu();
  void DrawPoweredOffWindow();
  void DrawSettingsWindow();
  void DrawAboutWindow();
  bool DrawFileChooser(const char* label, std::string* path, const char* filter = nullptr);

  SDL_Window* m_window = nullptr;
  std::unique_ptr<HostDisplayTexture> m_app_icon_texture;

  KeyboardControllerActionMap m_keyboard_button_mapping;

  std::map<int, ControllerData> m_sdl_controllers;
  std::array<s32, SDL_CONTROLLER_AXIS_MAX> m_controller_axis_mapping;
  std::array<s32, SDL_CONTROLLER_BUTTON_MAX> m_controller_button_mapping;

  u32 m_switch_gpu_renderer_event_id = 0;

  bool m_quit_request = false;
  bool m_frame_step_request = false;
  bool m_focus_main_menu_bar = false;
  bool m_settings_window_open = false;
  bool m_about_window_open = false;
};
