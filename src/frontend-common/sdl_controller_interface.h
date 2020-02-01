#pragma once
#include <array>
#include <map>
#include <mutex>
#include <functional>
#include "core/types.h"

class HostInterface;
class System;
class Controller;

union SDL_Event;

class SDLControllerInterface
{
public:
  SDLControllerInterface();
  ~SDLControllerInterface();

  bool Initialize(HostInterface* host_interface, bool init_sdl);
  void Shutdown();

  void UpdateControllerMapping();

  void PumpSDLEvents();

  bool ProcessSDLEvent(const SDL_Event* event);

  // Input monitoring for external access.
  struct Hook
  {
    enum class Type
    {
      Axis,
      Button
    };

    enum class CallbackResult
    {
      StopMonitoring,
      ContinueMonitoring
    };

    using Callback = std::function<CallbackResult(const Hook& ei)>;

    Type type;
    int controller_index;
    int button_or_axis_number;
    float value;  // 0/1 for buttons, -1..1 for axises
  };
  void SetHook(Hook::Callback callback);
  void ClearHook();

private:
  System* GetSystem() const;
  Controller* GetController(u32 slot) const;
  bool DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number, float value);

  bool OpenGameController(int index);
  bool CloseGameController(int index);
  void CloseGameControllers();
  bool HandleControllerAxisEvent(const SDL_Event* event);
  bool HandleControllerButtonEvent(const SDL_Event* event);
  void UpdateControllerRumble();

  struct ControllerData
  {
    void* controller;
    void* haptic;
    u32 controller_index;
    float last_rumble_strength;
  };

  HostInterface* m_host_interface = nullptr;

  std::map<int, ControllerData> m_controllers;
  std::array<s32, 6> m_controller_axis_mapping{};
  std::array<s32, 15> m_controller_button_mapping{};

  std::mutex m_event_intercept_mutex;
  Hook::Callback m_event_intercept_callback;

  bool m_sdl_initialized_by_us = false;
};

extern SDLControllerInterface g_sdl_controller_interface;
