#include "host_interface.h"
#include "bios.h"
#include "cdrom.h"
#include "common/audio_stream.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/string_util.h"
#include "dma.h"
#include "game_list.h"
#include "gpu.h"
#include "host_display.h"
#include "mdec.h"
#include "spu.h"
#include "system.h"
#include "timers.h"
#include <cmath>
#include <cstring>
#include <imgui.h>
Log_SetChannel(HostInterface);

#ifdef WIN32
#include "common/windows_headers.h"
#include <mmsystem.h>
#endif

#if defined(ANDROID) || (defined(__GNUC__) && __GNUC__ < 8)

static std::string GetRelativePath(const std::string& path, const char* new_filename)
{
  const char* last = std::strrchr(path.c_str(), '/');
  if (!last)
    return new_filename;

  std::string new_path(path.c_str(), last - path.c_str() + 1);
  new_path += new_filename;
  return new_path;
}

#else

#include <filesystem>

static std::string GetRelativePath(const std::string& path, const char* new_filename)
{
  return std::filesystem::path(path).replace_filename(new_filename).string();
}

#endif

HostInterface::HostInterface()
{
  SetUserDirectory();
  CreateUserDirectorySubdirectories();
  SetDefaultSettings();
  m_game_list = std::make_unique<GameList>();
  m_game_list->SetCacheFilename(GetGameListCacheFileName());
  m_game_list->SetDatabaseFilename(GetGameListDatabaseFileName());
}

HostInterface::~HostInterface()
{
  // system should be shut down prior to the destructor
  Assert(!m_system && !m_audio_stream && !m_display);
}

void HostInterface::CreateAudioStream()
{
  m_audio_stream = CreateAudioStream(m_settings.audio_backend);

  if (m_audio_stream && m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BUFFER_SIZE, 4))
    return;

  ReportFormattedError("Failed to create or configure audio stream, falling back to null output.");
  m_audio_stream.reset();
  m_audio_stream = AudioStream::CreateNullAudioStream();
  m_audio_stream->Reconfigure(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BUFFER_SIZE, 4);
}

bool HostInterface::BootSystemFromFile(const char* filename)
{
  if (!AcquireHostDisplay())
  {
    ReportFormattedError("Failed to acquire host display");
    return false;
  }

  // set host display settings
  m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);

  // create the audio stream. this will never fail, since we'll just fall back to null
  CreateAudioStream();

  m_system = System::Create(this);
  if (!m_system->Boot(filename))
  {
    ReportFormattedError("System failed to boot. The log may contain more information.");
    DestroySystem();
    return false;
  }

  OnSystemCreated();

  m_paused = m_settings.start_paused;
  m_audio_stream->PauseOutput(m_paused);
  UpdateSpeedLimiterState();

  if (m_paused)
    OnSystemPaused(true);

  return true;
}

bool HostInterface::BootSystemFromBIOS()
{
  return BootSystemFromFile(nullptr);
}

void HostInterface::PauseSystem(bool paused)
{
  if (paused == m_paused)
    return;

  m_paused = paused;
  m_audio_stream->PauseOutput(m_paused);
  OnSystemPaused(paused);
  UpdateSpeedLimiterState();
}

void HostInterface::ResetSystem()
{
  m_system->Reset();
  m_system->ResetPerformanceCounters();
  AddOSDMessage("System reset.");
}

void HostInterface::DestroySystem()
{
  if (!m_system)
    return;

  SetTimerResolutionIncreased(false);

  m_paused = false;
  m_system.reset();
  m_audio_stream.reset();
  ReleaseHostDisplay();
  OnSystemDestroyed();
  OnRunningGameChanged();
}

void HostInterface::ReportError(const char* message)
{
  Log_ErrorPrint(message);
}

void HostInterface::ReportMessage(const char* message)
{
  Log_InfoPrintf(message);
}

void HostInterface::ReportFormattedError(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  ReportError(message.c_str());
}

void HostInterface::ReportFormattedMessage(const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  ReportMessage(message.c_str());
}

void HostInterface::DrawFPSWindow()
{
  const bool show_fps = true;
  const bool show_vps = true;
  const bool show_speed = true;

  if (!(show_fps | show_vps | show_speed) || !m_system)
    return;

  const ImVec2 window_size =
    ImVec2(175.0f * ImGui::GetIO().DisplayFramebufferScale.x, 16.0f * ImGui::GetIO().DisplayFramebufferScale.y);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - window_size.x, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(window_size);

  if (!ImGui::Begin("FPSWindow", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMouseInputs |
                      ImGuiWindowFlags_NoBringToFrontOnFocus))
  {
    ImGui::End();
    return;
  }

  bool first = true;
  if (show_fps)
  {
    ImGui::Text("%.2f", m_system->GetFPS());
    first = false;
  }
  if (show_vps)
  {
    if (first)
    {
      first = false;
    }
    else
    {
      ImGui::SameLine();
      ImGui::Text("/");
      ImGui::SameLine();
    }

    ImGui::Text("%.2f", m_system->GetVPS());
  }
  if (show_speed)
  {
    if (first)
    {
      first = false;
    }
    else
    {
      ImGui::SameLine();
      ImGui::Text("/");
      ImGui::SameLine();
    }

    const float speed = m_system->GetEmulationSpeed();
    const u32 rounded_speed = static_cast<u32>(std::round(speed));
    if (speed < 90.0f)
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%u%%", rounded_speed);
    else if (speed < 110.0f)
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%u%%", rounded_speed);
    else
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u%%", rounded_speed);
  }

  ImGui::End();
}

void HostInterface::AddOSDMessage(const char* message, float duration /*= 2.0f*/)
{
  OSDMessage msg;
  msg.text = message;
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void HostInterface::AddFormattedOSDMessage(float duration, const char* format, ...)
{
  std::va_list ap;
  va_start(ap, format);
  std::string message = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  OSDMessage msg;
  msg.text = std::move(message);
  msg.duration = duration;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  m_osd_messages.push_back(std::move(msg));
}

void HostInterface::DrawOSDMessages()
{
  constexpr ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;

  std::unique_lock<std::mutex> lock(m_osd_messages_lock);
  if (m_osd_messages.empty())
    return;

  const float scale = ImGui::GetIO().DisplayFramebufferScale.x;

  auto iter = m_osd_messages.begin();
  float position_x = 10.0f * scale;
  float position_y = (10.0f + (static_cast<float>(m_display->GetDisplayTopMargin()))) * scale;
  u32 index = 0;
  while (iter != m_osd_messages.end())
  {
    const OSDMessage& msg = *iter;
    const double time = msg.time.GetTimeSeconds();
    const float time_remaining = static_cast<float>(msg.duration - time);
    if (time_remaining <= 0.0f)
    {
      iter = m_osd_messages.erase(iter);
      continue;
    }

    const float opacity = std::min(time_remaining, 1.0f);
    ImGui::SetNextWindowPos(ImVec2(position_x, position_y));
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "osd_%u", index++);

    if (ImGui::Begin(buf, nullptr, window_flags))
    {
      ImGui::TextUnformatted(msg.text.c_str());
      position_y += ImGui::GetWindowSize().y + (4.0f * scale);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ++iter;
  }
}

void HostInterface::DrawDebugWindows()
{
  const Settings::DebugSettings& debug_settings = m_system->GetSettings().debugging;

  if (debug_settings.show_gpu_state)
    m_system->GetGPU()->DrawDebugStateWindow();
  if (debug_settings.show_cdrom_state)
    m_system->GetCDROM()->DrawDebugWindow();
  if (debug_settings.show_timers_state)
    m_system->GetTimers()->DrawDebugStateWindow();
  if (debug_settings.show_spu_state)
    m_system->GetSPU()->DrawDebugStateWindow();
  if (debug_settings.show_mdec_state)
    m_system->GetMDEC()->DrawDebugStateWindow();
}

std::optional<std::vector<u8>> HostInterface::GetBIOSImage(ConsoleRegion region)
{
  // Try the other default filenames in the directory of the configured BIOS.
#define TRY_FILENAME(filename)                                                                                         \
  do                                                                                                                   \
  {                                                                                                                    \
    std::string try_filename = filename;                                                                               \
    std::optional<BIOS::Image> found_image = BIOS::LoadImageFromFile(try_filename);                                    \
    BIOS::Hash found_hash = BIOS::GetHash(*found_image);                                                               \
    Log_DevPrintf("Hash for BIOS '%s': %s", try_filename.c_str(), found_hash.ToString().c_str());                      \
    if (BIOS::IsValidHashForRegion(region, found_hash))                                                                \
    {                                                                                                                  \
      Log_InfoPrintf("Using BIOS from '%s'", try_filename.c_str());                                                    \
      return found_image;                                                                                              \
    }                                                                                                                  \
  } while (0)

  // Try the configured image.
  TRY_FILENAME(m_settings.bios_path);

  // Try searching in the same folder for other region's images.
  switch (region)
  {
    case ConsoleRegion::NTSC_J:
      TRY_FILENAME(GetRelativePath(m_settings.bios_path, "scph1000.bin"));
      TRY_FILENAME(GetRelativePath(m_settings.bios_path, "scph5500.bin"));
      break;

    case ConsoleRegion::NTSC_U:
      TRY_FILENAME(GetRelativePath(m_settings.bios_path, "scph1001.bin"));
      TRY_FILENAME(GetRelativePath(m_settings.bios_path, "scph5501.bin"));
      break;

    case ConsoleRegion::PAL:
      TRY_FILENAME(GetRelativePath(m_settings.bios_path, "scph1002.bin"));
      TRY_FILENAME(GetRelativePath(m_settings.bios_path, "scph5502.bin"));
      break;

    default:
      break;
  }

#undef RELATIVE_PATH
#undef TRY_FILENAME

  // Fall back to the default image.
  Log_WarningPrintf("No suitable BIOS image for region %s could be located, using configured image '%s'. This may "
                    "result in instability.",
                    Settings::GetConsoleRegionName(region), m_settings.bios_path.c_str());
  return BIOS::LoadImageFromFile(m_settings.bios_path);
}

bool HostInterface::LoadState(const char* filename)
{
  std::unique_ptr<ByteStream> stream = FileSystem::OpenFile(filename, BYTESTREAM_OPEN_READ | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  AddFormattedOSDMessage(2.0f, "Loading state from '%s'...", filename);

  if (m_system)
  {
    if (!m_system->LoadState(stream.get()))
    {
      ReportFormattedError("Loading state from '%s' failed. Resetting.", filename);
      m_system->Reset();
      return false;
    }

    m_system->ResetPerformanceCounters();
  }
  else
  {
    if (!BootSystemFromFile(nullptr))
    {
      ReportFormattedError("Failed to boot system to load state from '%s'.", filename);
      return false;
    }

    if (!m_system->LoadState(stream.get()))
    {
      ReportFormattedError("Failed to load state. The log may contain more information. Shutting down system.");
      DestroySystem();
      return false;
    }
  }

  return true;
}

bool HostInterface::LoadState(bool global, s32 slot)
{
  if (!global && (!m_system || m_system->GetRunningCode().empty()))
  {
    ReportFormattedError("Can't save per-game state without a running game code.");
    return false;
  }

  std::string save_path =
    global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(m_system->GetRunningCode().c_str(), slot);
  return LoadState(save_path.c_str());
}

bool HostInterface::SaveState(const char* filename)
{
  std::unique_ptr<ByteStream> stream =
    FileSystem::OpenFile(filename, BYTESTREAM_OPEN_CREATE | BYTESTREAM_OPEN_WRITE | BYTESTREAM_OPEN_TRUNCATE |
                                     BYTESTREAM_OPEN_ATOMIC_UPDATE | BYTESTREAM_OPEN_STREAMED);
  if (!stream)
    return false;

  const bool result = m_system->SaveState(stream.get());
  if (!result)
  {
    ReportFormattedError("Saving state to '%s' failed.", filename);
    stream->Discard();
  }
  else
  {
    AddFormattedOSDMessage(2.0f, "State saved to '%s'.", filename);
    stream->Commit();
  }

  return result;
}

bool HostInterface::SaveState(bool global, s32 slot)
{
  const std::string& code = m_system->GetRunningCode();
  if (!global && code.empty())
  {
    ReportFormattedError("Can't save per-game state without a running game code.");
    return false;
  }

  std::string save_path = global ? GetGlobalSaveStateFileName(slot) : GetGameSaveStateFileName(code.c_str(), slot);
  return SaveState(save_path.c_str());
}

bool HostInterface::ResumeSystemFromState(const char* filename, bool boot_on_failure)
{
  if (!BootSystemFromFile(filename))
    return false;

  const bool global = m_system->GetRunningCode().empty();
  const std::string path =
    global ? GetGlobalSaveStateFileName(-1) : GetGameSaveStateFileName(m_system->GetRunningCode().c_str(), -1);
  if (FileSystem::FileExists(path.c_str()))
  {
    if (!LoadState(path.c_str()) && !boot_on_failure)
    {
      DestroySystem();
      return false;
    }
  }
  else
  {
    ReportFormattedError("Resume save state not found for '%s' ('%s').", m_system->GetRunningCode().c_str(),
                         m_system->GetRunningTitle().c_str());
    if (!boot_on_failure)
    {
      DestroySystem();
      return false;
    }
  }

  return true;
}

bool HostInterface::ResumeSystemFromMostRecentState()
{
  const std::string path = GetMostRecentResumeSaveStatePath();
  if (path.empty())
  {
    ReportError("No resume save state found.");
    return false;
  }

  return LoadState(path.c_str());
}

void HostInterface::UpdateSpeedLimiterState()
{
  m_speed_limiter_enabled = m_settings.speed_limiter_enabled && !m_speed_limiter_temp_disabled;

  const bool is_non_standard_speed = ((m_settings.emulation_speed - 1.0f) > 0.05f);
  const bool audio_sync_enabled =
    !m_system || m_paused || (m_speed_limiter_enabled && m_settings.audio_sync_enabled && !is_non_standard_speed);
  const bool video_sync_enabled =
    !m_system || m_paused || (m_speed_limiter_enabled && m_settings.video_sync_enabled && !is_non_standard_speed);
  Log_InfoPrintf("Syncing to %s%s", audio_sync_enabled ? "audio" : "",
                 (audio_sync_enabled && video_sync_enabled) ? " and video" : (video_sync_enabled ? "video" : ""));

  m_audio_stream->SetSync(audio_sync_enabled);
  if (audio_sync_enabled)
    m_audio_stream->EmptyBuffers();

  m_display->SetVSync(video_sync_enabled);

  if (m_settings.increase_timer_resolution)
    SetTimerResolutionIncreased(m_speed_limiter_enabled);

  m_system->ResetPerformanceCounters();
}

void HostInterface::OnSystemCreated() {}

void HostInterface::OnSystemPaused(bool paused)
{
  ReportFormattedMessage("System %s.", paused ? "paused" : "resumed");
}

void HostInterface::OnSystemDestroyed()
{
  ReportFormattedMessage("System shut down.");
}

void HostInterface::OnSystemPerformanceCountersUpdated() {}

void HostInterface::OnRunningGameChanged() {}

void HostInterface::OnControllerTypeChanged(u32 slot) {}

void HostInterface::SetUserDirectory()
{
#ifdef WIN32
  // On Windows, use the path to the program.
  // We might want to use My Documents in the future.
  const std::string program_path = FileSystem::GetProgramPath();
  Log_InfoPrintf("Program path: %s", program_path.c_str());

  m_user_directory = FileSystem::GetPathDirectory(program_path.c_str());
#else
#endif

  Log_InfoPrintf("User directory: %s", m_user_directory.c_str());

  // Change to the user directory so that all default/relative paths in the config are after this.
  if (!m_user_directory.empty())
  {
    if (!FileSystem::SetWorkingDirectory(m_user_directory.c_str()))
      Log_ErrorPrintf("Failed to set working directory to '%s'", m_user_directory.c_str());
  }
}

void HostInterface::CreateUserDirectorySubdirectories()
{
  bool result = true;

  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("bios").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("cache").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("savestates").c_str(), false);
  result &= FileSystem::CreateDirectory(GetUserDirectoryRelativePath("memcards").c_str(), false);

  if (!result)
    ReportError("Failed to create one or more user directories. This may cause issues at runtime.");
}

std::string HostInterface::GetUserDirectoryRelativePath(const char* format, ...) const
{
  std::va_list ap;
  va_start(ap, format);
  std::string formatted_path = StringUtil::StdStringFromFormatV(format, ap);
  va_end(ap);

  if (m_user_directory.empty())
  {
    return formatted_path;
  }
  else
  {
    return StringUtil::StdStringFromFormat("%s%c%s", m_user_directory.c_str(), FS_OSPATH_SEPERATOR_CHARACTER,
                                           formatted_path.c_str());
  }
}

std::string HostInterface::GetSettingsFileName() const
{
  return GetUserDirectoryRelativePath("settings.ini");
}

std::string HostInterface::GetGameListCacheFileName() const
{
  return GetUserDirectoryRelativePath("cache/gamelist.cache");
}

std::string HostInterface::GetGameListDatabaseFileName() const
{
  return GetUserDirectoryRelativePath("cache/redump.dat");
}

std::string HostInterface::GetGameSaveStateFileName(const char* game_code, s32 slot) const
{
  if (slot < 0)
    return GetUserDirectoryRelativePath("savestates/%s_resume.sav", game_code);
  else
    return GetUserDirectoryRelativePath("savestates/%s_%d.sav", game_code, slot);
}

std::string HostInterface::GetGlobalSaveStateFileName(s32 slot) const
{
  if (slot < 0)
    return GetUserDirectoryRelativePath("savestates/resume.sav");
  else
    return GetUserDirectoryRelativePath("savestates/savestate_%d.sav", slot);
}

std::string HostInterface::GetSharedMemoryCardPath(u32 slot) const
{
  return GetUserDirectoryRelativePath("memcards/shared_card_%d.mcd", slot + 1);
}

std::string HostInterface::GetGameMemoryCardPath(const char* game_code, u32 slot) const
{
  return GetUserDirectoryRelativePath("memcards/game_card_%s_%d.mcd", game_code, slot + 1);
}

std::vector<HostInterface::SaveStateInfo> HostInterface::GetAvailableSaveStates(const char* game_code) const
{
  std::vector<SaveStateInfo> si;
  std::string path;

  auto add_path = [&si](const std::string& path, s32 slot, bool global) {
    FILESYSTEM_STAT_DATA sd;
    if (!FileSystem::StatFile(path.c_str(), &sd))
      return;

    si.push_back(SaveStateInfo{std::move(path), sd.ModificationTime.AsUnixTimestamp(), static_cast<s32>(slot), global});
  };

  if (game_code && std::strlen(game_code) > 0)
  {
    add_path(GetGameSaveStateFileName(game_code, -1), -1, false);
    for (s32 i = 1; i <= PER_GAME_SAVE_STATE_SLOTS; i++)
      add_path(GetGameSaveStateFileName(game_code, i), i, false);
  }

  for (s32 i = 1; i <= GLOBAL_SAVE_STATE_SLOTS; i++)
    add_path(GetGlobalSaveStateFileName(i), i, true);

  return si;
}

std::string HostInterface::GetMostRecentResumeSaveStatePath() const
{
  std::vector<FILESYSTEM_FIND_DATA> files;
  if (!FileSystem::FindFiles(GetUserDirectoryRelativePath("savestates").c_str(), "*resume.sav", FILESYSTEM_FIND_FILES,
                             &files) ||
      files.empty())
  {
    return {};
  }

  FILESYSTEM_FIND_DATA* most_recent = &files[0];
  for (FILESYSTEM_FIND_DATA& file : files)
  {
    if (file.ModificationTime > most_recent->ModificationTime)
      most_recent = &file;
  }

  return std::move(most_recent->FileName);
}

void HostInterface::SetDefaultSettings()
{
  m_settings.region = ConsoleRegion::Auto;
  m_settings.cpu_execution_mode = CPUExecutionMode::Interpreter;

  m_settings.emulation_speed = 1.0f;
  m_settings.speed_limiter_enabled = true;
  m_settings.start_paused = false;
  m_settings.increase_timer_resolution = true;

  m_settings.gpu_renderer = GPURenderer::HardwareOpenGL;
  m_settings.gpu_resolution_scale = 1;
  m_settings.gpu_true_color = true;
  m_settings.gpu_texture_filtering = false;
  m_settings.gpu_force_progressive_scan = true;
  m_settings.gpu_use_debug_device = false;
  m_settings.display_linear_filtering = true;
  m_settings.display_fullscreen = false;
  m_settings.video_sync_enabled = true;

  m_settings.audio_backend = AudioBackend::Cubeb;
  m_settings.audio_sync_enabled = true;

  m_settings.bios_path = GetUserDirectoryRelativePath("bios/scph1001.bin");
  m_settings.bios_patch_tty_enable = false;
  m_settings.bios_patch_fast_boot = false;

  m_settings.controller_types[0] = ControllerType::DigitalController;
  m_settings.controller_types[1] = ControllerType::None;

  m_settings.memory_card_paths[0] = GetSharedMemoryCardPath(0);
  m_settings.memory_card_paths[1] = GetSharedMemoryCardPath(1);
}

void HostInterface::UpdateSettings(const std::function<void()>& apply_callback)
{
  const float old_emulation_speed = m_settings.emulation_speed;
  const CPUExecutionMode old_cpu_execution_mode = m_settings.cpu_execution_mode;
  const AudioBackend old_audio_backend = m_settings.audio_backend;
  const GPURenderer old_gpu_renderer = m_settings.gpu_renderer;
  const u32 old_gpu_resolution_scale = m_settings.gpu_resolution_scale;
  const bool old_gpu_true_color = m_settings.gpu_true_color;
  const bool old_gpu_texture_filtering = m_settings.gpu_texture_filtering;
  const bool old_gpu_force_progressive_scan = m_settings.gpu_force_progressive_scan;
  const bool old_vsync_enabled = m_settings.video_sync_enabled;
  const bool old_audio_sync_enabled = m_settings.audio_sync_enabled;
  const bool old_speed_limiter_enabled = m_settings.speed_limiter_enabled;
  const bool old_display_linear_filtering = m_settings.display_linear_filtering;
  std::array<ControllerType, NUM_CONTROLLER_AND_CARD_PORTS> old_controller_types = m_settings.controller_types;

  apply_callback();

  if (m_system)
  {
    if (m_settings.gpu_renderer != old_gpu_renderer)
    {
      ReportFormattedMessage("Switching to %s GPU renderer.", Settings::GetRendererName(m_settings.gpu_renderer));
      RecreateSystem();
    }

    if (m_settings.audio_backend != old_audio_backend)
    {
      ReportFormattedMessage("Switching to %s audio backend.", Settings::GetAudioBackendName(m_settings.audio_backend));
      DebugAssert(m_audio_stream);
      m_audio_stream.reset();
      CreateAudioStream();
    }

    if (m_settings.video_sync_enabled != old_vsync_enabled || m_settings.audio_sync_enabled != old_audio_sync_enabled ||
        m_settings.speed_limiter_enabled != old_speed_limiter_enabled)
    {
      UpdateSpeedLimiterState();
    }

    if (m_settings.emulation_speed != old_emulation_speed)
    {
      m_system->UpdateThrottlePeriod();
      UpdateSpeedLimiterState();
    }

    if (m_settings.cpu_execution_mode != old_cpu_execution_mode)
    {
      ReportFormattedMessage("Switching to %s CPU execution mode.",
                             Settings::GetCPUExecutionModeName(m_settings.cpu_execution_mode));
      m_system->SetCPUExecutionMode(m_settings.cpu_execution_mode);
    }

    if (m_settings.gpu_resolution_scale != old_gpu_resolution_scale ||
        m_settings.gpu_true_color != old_gpu_true_color ||
        m_settings.gpu_texture_filtering != old_gpu_texture_filtering ||
        m_settings.gpu_force_progressive_scan != old_gpu_force_progressive_scan)
    {
      m_system->UpdateGPUSettings();
    }
  }

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++)
  {
    if (m_settings.controller_types[i] != old_controller_types[i])
      OnControllerTypeChanged(i);
  }

  if (m_display && m_settings.display_linear_filtering != old_display_linear_filtering)
    m_display->SetDisplayLinearFiltering(m_settings.display_linear_filtering);
}

void HostInterface::ToggleSoftwareRendering()
{
  if (!m_system || m_settings.gpu_renderer == GPURenderer::Software)
    return;

  const GPURenderer new_renderer =
    m_system->GetGPU()->IsHardwareRenderer() ? GPURenderer::Software : m_settings.gpu_renderer;

  AddFormattedOSDMessage(2.0f, "Switching to %s renderer...", Settings::GetRendererDisplayName(new_renderer));
  m_system->RecreateGPU(new_renderer);
}

void HostInterface::ModifyResolutionScale(s32 increment)
{
  const u32 new_resolution_scale =
    std::clamp<u32>(static_cast<u32>(static_cast<s32>(m_settings.gpu_resolution_scale) + increment), 1,
                    m_settings.max_gpu_resolution_scale);
  if (new_resolution_scale == m_settings.gpu_resolution_scale)
    return;

  m_settings.gpu_resolution_scale = new_resolution_scale;
  if (m_system)
    m_system->GetGPU()->UpdateSettings();

  AddFormattedOSDMessage(2.0f, "Resolution scale set to %ux (%ux%u)", m_settings.gpu_resolution_scale,
                         GPU::VRAM_WIDTH * m_settings.gpu_resolution_scale,
                         GPU::VRAM_HEIGHT * m_settings.gpu_resolution_scale);
}

void HostInterface::RecreateSystem()
{
  std::unique_ptr<ByteStream> stream = ByteStream_CreateGrowableMemoryStream(nullptr, 8 * 1024);
  if (!m_system->SaveState(stream.get()) || !stream->SeekAbsolute(0))
  {
    ReportError("Failed to save state before system recreation. Shutting down.");
    DestroySystem();
    return;
  }

  DestroySystem();
  if (!BootSystemFromFile(nullptr))
  {
    ReportError("Failed to boot system after recreation.");
    return;
  }

  if (!m_system->LoadState(stream.get()))
  {
    ReportError("Failed to load state after system recreation. Shutting down.");
    DestroySystem();
    return;
  }

  m_system->ResetPerformanceCounters();
}

void HostInterface::SetTimerResolutionIncreased(bool enabled)
{
  if (m_timer_resolution_increased == enabled)
    return;

  m_timer_resolution_increased = enabled;

#ifdef WIN32
  if (enabled)
    timeBeginPeriod(1);
  else
    timeEndPeriod(1);
#endif
}

bool HostInterface::SaveResumeSaveState()
{
  if (!m_system)
    return false;

  const bool global = m_system->GetRunningCode().empty();
  return SaveState(global, -1);
}
