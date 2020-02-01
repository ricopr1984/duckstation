#include "sdl_controller_interface.h"
#include "common/assert.h"
#include "common/log.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include "core/system.h"
#include <SDL.h>
Log_SetChannel(SDLControllerInterface);

SDLControllerInterface g_sdl_controller_interface;

SDLControllerInterface::SDLControllerInterface() = default;

SDLControllerInterface::~SDLControllerInterface()
{
  Assert(m_controllers.empty());
}

bool SDLControllerInterface::Initialize(HostInterface* host_interface, bool init_sdl)
{
  if (init_sdl)
  {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
    {
      Log_ErrorPrintf("SDL_Init() failed");
      return false;
    }
  }

  // we should open the controllers as the connected events come in, so no need to do any more here
  m_host_interface = host_interface;
  UpdateControllerMapping();
  return true;
}

void SDLControllerInterface::Shutdown()
{
  while (!m_controllers.empty())
    CloseGameController(m_controllers.begin()->first);

  if (m_sdl_initialized_by_us)
  {
    SDL_Quit();
    m_sdl_initialized_by_us = false;
  }

  m_host_interface = nullptr;
}

void SDLControllerInterface::PumpSDLEvents()
{
  for (;;)
  {
    SDL_Event ev;
    if (SDL_PollEvent(&ev))
      ProcessSDLEvent(&ev);
    else
      break;
  }
}

bool SDLControllerInterface::ProcessSDLEvent(const SDL_Event* event)
{
  switch (event->type)
  {
    case SDL_CONTROLLERDEVICEADDED:
    {
      Log_InfoPrintf("Controller %d inserted", event->cdevice.which);
      OpenGameController(event->cdevice.which);
      return true;
    }

    case SDL_CONTROLLERDEVICEREMOVED:
    {
      Log_InfoPrintf("Controller %d removed", event->cdevice.which);
      CloseGameController(event->cdevice.which);
      return true;
    }

    case SDL_CONTROLLERAXISMOTION:
      return HandleControllerAxisEvent(event);

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
      return HandleControllerButtonEvent(event);

    default:
      return false;
  }
}

System* SDLControllerInterface::GetSystem() const
{
  return m_host_interface->GetSystem();
}

Controller* SDLControllerInterface::GetController(u32 slot) const
{
  System* system = GetSystem();
  return system ? system->GetController(slot) : nullptr;
}

void SDLControllerInterface::SetHook(Hook::Callback callback)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  Assert(!m_event_intercept_callback);
  m_event_intercept_callback = std::move(callback);
}

void SDLControllerInterface::ClearHook()
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (m_event_intercept_callback)
    m_event_intercept_callback = {};
}

bool SDLControllerInterface::DoEventHook(Hook::Type type, int controller_index, int button_or_axis_number, float value)
{
  std::unique_lock<std::mutex> lock(m_event_intercept_mutex);
  if (!m_event_intercept_callback)
    return false;

  const Hook ei{type, controller_index, button_or_axis_number, value};
  const Hook::CallbackResult action = m_event_intercept_callback(ei);
  if (action == Hook::CallbackResult::StopMonitoring)
    m_event_intercept_callback = {};

  return true;
}

bool SDLControllerInterface::OpenGameController(int index)
{
  if (m_controllers.find(index) != m_controllers.end())
    CloseGameController(index);

  SDL_GameController* gcontroller = SDL_GameControllerOpen(index);
  if (!gcontroller)
  {
    Log_WarningPrintf("Failed to open controller %d", index);
    return false;
  }

  Log_InfoPrintf("Opened controller %d: %s", index, SDL_GameControllerName(gcontroller));

  ControllerData cd = {};
  cd.controller = gcontroller;

  SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gcontroller);
  if (joystick)
  {
    SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(joystick);
    if (SDL_HapticRumbleSupported(haptic) && SDL_HapticRumbleInit(haptic) == 0)
      cd.haptic = haptic;
    else
      SDL_HapticClose(haptic);
  }

  if (cd.haptic)
    Log_InfoPrintf("Rumble is supported on '%s'", SDL_GameControllerName(gcontroller));
  else
    Log_WarningPrintf("Rumble is not supported on '%s'", SDL_GameControllerName(gcontroller));

  m_controllers.emplace(index, cd);
  return true;
}

void SDLControllerInterface::CloseGameControllers()
{
  while (!m_controllers.empty())
    CloseGameController(m_controllers.begin()->first);
}

bool SDLControllerInterface::CloseGameController(int index)
{
  auto it = m_controllers.find(index);
  if (it == m_controllers.end())
    return false;

  if (it->second.haptic)
    SDL_HapticClose(static_cast<SDL_Haptic*>(it->second.haptic));

  SDL_GameControllerClose(static_cast<SDL_GameController*>(it->second.controller));
  m_controllers.erase(it);
  return true;
}

void SDLControllerInterface::UpdateControllerMapping()
{
  m_controller_axis_mapping.fill(-1);
  m_controller_button_mapping.fill(-1);

  const ControllerType type = m_host_interface->GetSettings().controller_types[0];
  if (type == ControllerType::None)
    return;

#define SET_AXIS_MAP(axis, name)                                                                                       \
  m_controller_axis_mapping[axis] = Controller::GetAxisCodeByName(type, name).value_or(-1)
#define SET_BUTTON_MAP(button, name)                                                                                   \
  m_controller_button_mapping[button] = Controller::GetButtonCodeByName(type, name).value_or(-1)

  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_LEFTX, "LeftX");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_LEFTY, "LeftY");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_RIGHTX, "RightX");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_RIGHTY, "RightY");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_TRIGGERLEFT, "LeftTrigger");
  SET_AXIS_MAP(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, "RightTrigger");

  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_UP, "Up");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_DOWN, "Down");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_LEFT, "Left");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "Right");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_Y, "Triangle");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_A, "Cross");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_X, "Square");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_B, "Circle");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_START, "Start");
  SET_BUTTON_MAP(SDL_CONTROLLER_BUTTON_BACK, "Select");

#undef SET_AXIS_MAP
#undef SET_BUTTON_MAP
}

bool SDLControllerInterface::HandleControllerAxisEvent(const SDL_Event* ev)
{
  const float value = static_cast<float>(ev->caxis.value) / (ev->caxis.value < 0 ? 32768.0f : 32767.0f);
  if (DoEventHook(Hook::Type::Axis, ev->caxis.which, ev->caxis.axis, value))
    return true;

  // Log_DevPrintf("axis %d %d", ev->caxis.axis, ev->caxis.value);
  Controller* controller = GetController(0);
  if (!controller)
    return false;

  // proper axis mapping
  if (m_controller_axis_mapping[ev->caxis.axis] >= 0)
  {
    const float value = static_cast<float>(ev->caxis.value) / (ev->caxis.value < 0 ? 32768.0f : 32767.0f);
    controller->SetAxisState(m_controller_axis_mapping[ev->caxis.axis], value);
    return true;
  }

  // axis-as-button mapping
  static constexpr float deadzone = 8192.0f;
  const bool negative = (value < 0);
  const bool active = (std::abs(value) >= deadzone);

  // FIXME
  if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
  {
    auto button = controller->GetButtonCodeByName("L2");
    if (button)
      controller->SetButtonState(button.value(), active);
  }
  else if (ev->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
  {
    auto button = controller->GetButtonCodeByName("R2");
    if (button)
      controller->SetButtonState(button.value(), active);
  }
  else
  {
    SDL_GameControllerButton negative_button, positive_button;
    if (ev->caxis.axis & 1)
    {
      negative_button = SDL_CONTROLLER_BUTTON_DPAD_UP;
      positive_button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    }
    else
    {
      negative_button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
      positive_button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    }

    if (m_controller_button_mapping[negative_button] >= 0)
      controller->SetButtonState(m_controller_button_mapping[negative_button], negative && active);
    if (m_controller_button_mapping[positive_button] >= 0)
      controller->SetButtonState(m_controller_button_mapping[positive_button], !negative && active);
  }

  return true;
}

bool SDLControllerInterface::HandleControllerButtonEvent(const SDL_Event* ev)
{
  // Log_DevPrintf("button %d %s", ev->cbutton.button, ev->cbutton.state == SDL_PRESSED ? "pressed" : "released");
  const bool pressed = (ev->cbutton.state == SDL_PRESSED);
  if (DoEventHook(Hook::Type::Button, ev->cbutton.which, ev->cbutton.button, pressed ? 1.0f : 0.0f))
    return true;

  Controller* controller = GetController(0);
  if (!controller)
    return false;

  if (m_controller_button_mapping[ev->cbutton.button] >= 0)
    controller->SetButtonState(m_controller_button_mapping[ev->cbutton.button], ev->cbutton.state == SDL_PRESSED);

  return true;
}

void SDLControllerInterface::UpdateControllerRumble()
{
  for (auto& it : m_controllers)
  {
    ControllerData& cd = it.second;
    if (!cd.haptic)
      continue;

    float new_strength = 0.0f;
    Controller* controller = GetController(cd.controller_index);
    if (controller)
    {
      const u32 motor_count = controller->GetVibrationMotorCount();
      for (u32 i = 0; i < motor_count; i++)
        new_strength = std::max(new_strength, controller->GetVibrationMotorStrength(i));
    }

    if (cd.last_rumble_strength == new_strength)
      continue;

    if (new_strength > 0.01f)
      SDL_HapticRumblePlay(static_cast<SDL_Haptic*>(cd.haptic), new_strength, 100000);
    else
      SDL_HapticRumbleStop(static_cast<SDL_Haptic*>(cd.haptic));

    cd.last_rumble_strength = new_strength;
  }
}
