/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string>
#include <regex>

#include "AMLUtils.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "windowing/GraphicContext.h"
#include "utils/RegExp.h"
#include "filesystem/SpecialProtocol.h"
#include "rendering/RenderSystem.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"

#include "platform/linux/SysfsPath.h"

#include "linux/fb.h"
#include <sys/ioctl.h>
#include <amcodec/codec.h>

#define FLAG_FORCE_DV_LL        (unsigned int)(0x4000)
#define DOLBY_VISION_LL_DISABLE (unsigned int)(0)
#define DOLBY_VISION_LL_YUV422  (unsigned int)(1)

#define DOLBY_VISION_FOLLOW_SOURCE     (unsigned int)(1)
#define DOLBY_VISION_FORCE_OUTPUT_MODE (unsigned int)(2)

#define DOLBY_VISION_OUTPUT_MODE_IPT        (unsigned int)(0)
#define DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL (unsigned int)(1)
#define DOLBY_VISION_OUTPUT_MODE_BYPASS     (unsigned int)(5)

int aml_get_cpufamily_id()
{
  static int aml_cpufamily_id = -1;
  if (aml_cpufamily_id == -1)
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::regex re(".*: (.*)$");

    for (std::string line; std::getline(cpuinfo, line);)
    {
      if (line.find("Serial") != std::string::npos)
      {
        std::smatch match;

        if (std::regex_match(line, match, re) && match.size() == 2)
        {
          std::ssub_match value = match[1];
          std::string cpu_family = value.str().substr(0, 2);
          aml_cpufamily_id = std::stoi(cpu_family, nullptr, 16);
          break;
        }
      }
    }
  }
  return aml_cpufamily_id;
}

bool aml_display_support_dv()
{
  int support_dv = 0;
  CRegExp regexp;
  regexp.RegComp("The Rx don't support DolbyVision");
  std::string valstr;
  CSysfsPath dv_cap{"/sys/devices/virtual/amhdmitx/amhdmitx0/dv_cap"};
  if (dv_cap.Exists())
  {
    valstr = dv_cap.Get<std::string>().value();
    support_dv = (regexp.RegFind(valstr) >= 0) ? 0 : 1;
  }

  return support_dv;
}

bool aml_dv_support_ll()
{
  int support_ll = 0;
  CRegExp regexp;
  regexp.RegComp("YCbCr_422_12BIT");
  std::string valstr;
  CSysfsPath dv_cap{"/sys/devices/virtual/amhdmitx/amhdmitx0/dv_cap"};
  if (dv_cap.Exists())
  {
    valstr = dv_cap.Get<std::string>().value();
    support_ll = (regexp.RegFind(valstr) >= 0) ? 1 : 0;
  }

  return support_ll;
}

bool aml_dv_support_std()
{
  int support_std = 0;
  CRegExp regexp;
  regexp.RegComp("DV_RGB_444_8BIT");
  std::string valstr;
  CSysfsPath dv_cap{"/sys/devices/virtual/amhdmitx/amhdmitx0/dv_cap"};
  if (dv_cap.Exists())
  {
    valstr = dv_cap.Get<std::string>().value();
    support_std = (regexp.RegFind(valstr) >= 0) ? 1 : 0;
  }
  return support_std;
}

bool aml_display_support_3d()
{
  static int support_3d = -1;

  if (support_3d == -1)
  {
    CSysfsPath amhdmitx0_support_3d{"/sys/class/amhdmitx/amhdmitx0/support_3d"};
    if (amhdmitx0_support_3d.Exists())
      support_3d = amhdmitx0_support_3d.Get<int>().value();
    else
      support_3d = 0;

    CLog::Log(LOGDEBUG, "AMLUtils: display support 3D: {}", bool(!!support_3d));
  }

  return (support_3d == 1);
}

static bool aml_support_vcodec_profile(const char *regex)
{
  int profile = 0;
  CRegExp regexp;
  regexp.RegComp(regex);
  std::string valstr;
  CSysfsPath vcodec_profile{"/sys/class/amstream/vcodec_profile"};
  if (vcodec_profile.Exists())
  {
    valstr = vcodec_profile.Get<std::string>().value();
    profile = (regexp.RegFind(valstr) >= 0) ? 1 : 0;
  }

  return profile;
}

bool aml_support_hevc()
{
  static int has_hevc = -1;

  if (has_hevc == -1)
      has_hevc = aml_support_vcodec_profile("\\bhevc\\b:");

  return (has_hevc == 1);
}

bool aml_support_hevc_4k2k()
{
  static int has_hevc_4k2k = -1;

  if (has_hevc_4k2k == -1)
    has_hevc_4k2k = aml_support_vcodec_profile("\\bhevc\\b:(?!\\;).*(4k|8k)");

  return (has_hevc_4k2k == 1);
}

bool aml_support_hevc_8k4k()
{
  static int has_hevc_8k4k = -1;

  if (has_hevc_8k4k == -1)
    has_hevc_8k4k = aml_support_vcodec_profile("\\bhevc\\b:(?!\\;).*8k");

  return (has_hevc_8k4k == 1);
}

bool aml_support_hevc_10bit()
{
  static int has_hevc_10bit = -1;

  if (has_hevc_10bit == -1)
    has_hevc_10bit = aml_support_vcodec_profile("\\bhevc\\b:(?!\\;).*10bit");

  return (has_hevc_10bit == 1);
}

AML_SUPPORT_H264_4K2K aml_support_h264_4k2k()
{
  static AML_SUPPORT_H264_4K2K has_h264_4k2k = AML_SUPPORT_H264_4K2K_UNINIT;

  if (has_h264_4k2k == AML_SUPPORT_H264_4K2K_UNINIT)
  {
    has_h264_4k2k = AML_NO_H264_4K2K;

    if (aml_support_vcodec_profile("\\bh264\\b:4k"))
      has_h264_4k2k = AML_HAS_H264_4K2K_SAME_PROFILE;
    else if (aml_support_vcodec_profile("\\bh264_4k2k\\b:"))
      has_h264_4k2k = AML_HAS_H264_4K2K;
  }
  return has_h264_4k2k;
}

bool aml_support_vp9()
{
  static int has_vp9 = -1;

  if (has_vp9 == -1)
    has_vp9 = aml_support_vcodec_profile("\\bvp9\\b:(?!\\;).*compressed");

  return (has_vp9 == 1);
}

bool aml_support_av1()
{
  static int has_av1 = -1;

  if (has_av1 == -1)
    has_av1 = aml_support_vcodec_profile("\\bav1\\b:(?!\\;).*compressed");

  return (has_av1 == 1);
}

bool aml_support_dolby_vision()
{
  static int support_dv = -1;

  if (support_dv == -1)
  {
    CSysfsPath support_info{"/sys/class/amdolby_vision/support_info"};
    support_dv = 0;
    if (support_info.Exists())
    {
      support_dv = (int)((support_info.Get<int>().value() & 7) == 7);
      if (support_dv == 1) {
        CSysfsPath ko_info{"/sys/class/amdolby_vision/ko_info"};
        if (ko_info.Exists())
          CLog::Log(LOGDEBUG, "Amlogic Dolby Vision info: {}", ko_info.Get<std::string>().value().c_str());
      }
    }
  }

  return (support_dv == 1);
}

bool aml_dolby_vision_enabled()
{
  static int dv_enabled = -1;
  bool dv_user_enabled(CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE) != 2);

  if (dv_enabled == -1)
    dv_enabled = (!!aml_support_dolby_vision());

  return ((dv_enabled && !!dv_user_enabled) == 1);
}

void aml_dv_on(unsigned int mode, bool enable)
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  enum DV_TYPE dv_type(static_cast<DV_TYPE>(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE)));

  // Set the Dolby VSVDB parameter to latest value from user.
  bool dv_dolby_vsvdb_inject(settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_INJECT));
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_dolby_vsvdb_inject", dv_dolby_vsvdb_inject ? 1 : 0);

  std::string dv_dolby_vsvdb_payload(settings->GetString(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB));
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_dolby_vsvdb_payload", dv_dolby_vsvdb_payload);

  // Set the Colorimetery to latest value from user.
  int colorimetry(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_COLORIMETRY_FOR_STD));
  CSysfsPath("/sys/module/hdmitx20/parameters/dovi_tv_led_bt2020", (colorimetry == DV_COLORIMETRY_BT2020NC) ? 'Y' : 'N');
  CSysfsPath("/sys/module/hdmitx20/parameters/dovi_tv_led_no_colorimetry", (colorimetry == DV_COLORIMETRY_REMOVE) ? 'Y' : 'N');

  // Set the HDR for LLDV if DV_TYPE_PLAYER_LED_HDR.
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_hdr_for_lldv", (dv_type == DV_TYPE_PLAYER_LED_HDR) ? 'Y' : 'N'); 

  // force player led mode when enabled
  CSysfsPath dolby_vision_flags{"/sys/module/amdolby_vision/parameters/dolby_vision_flags"};
  CSysfsPath dolby_vision_ll_policy{"/sys/module/amdolby_vision/parameters/dolby_vision_ll_policy"};

  if (dolby_vision_flags.Exists() && dolby_vision_ll_policy.Exists())
  {
    if (dv_type == DV_TYPE_DISPLAY_LED) // Display Led (DV-Std)
    {
      dolby_vision_flags.Set(dolby_vision_flags.Get<unsigned int>().value() & ~(FLAG_FORCE_DV_LL));
      dolby_vision_ll_policy.Set(DOLBY_VISION_LL_DISABLE);
    }
    else // Player Led (DV-LL and HDR)
    {
      dolby_vision_flags.Set(dolby_vision_flags.Get<unsigned int>().value() | FLAG_FORCE_DV_LL);
      dolby_vision_ll_policy.Set(DOLBY_VISION_LL_YUV422);
    }
  }

  // Convert to non tunnel if not DV_TYPE_DISPLAY_LED.
	if ((mode == DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL) && (dv_type != DV_TYPE_DISPLAY_LED)) 
    mode = DOLBY_VISION_OUTPUT_MODE_IPT;

	CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_mode", mode);
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_policy", DOLBY_VISION_FORCE_OUTPUT_MODE);

  if (enable) aml_dv_enable();
}

void aml_dv_off(bool disable)
{
  // Reset DV Paremters - will do mode change.
  CSysfsPath("/sys/class/amdolby_vision/debug", "enable_fel 0");
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_mode", DOLBY_VISION_OUTPUT_MODE_BYPASS);
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_policy", DOLBY_VISION_FOLLOW_SOURCE);
  
  // If diable wait for mode change and then disable.
  if (disable) {
    CSysfsPath dolby_vision_target_mode{"/sys/module/amdolby_vision/parameters/dolby_vision_target_mode"};
    if (dolby_vision_target_mode.Exists())
    {
      std::chrono::time_point<std::chrono::system_clock> now(std::chrono::system_clock::now());
      while(dolby_vision_target_mode.Get<unsigned int>().value() != DOLBY_VISION_OUTPUT_MODE_BYPASS && 
            (std::chrono::system_clock::now() - now) < std::chrono::seconds(3))
        usleep(10000); // wait 10ms
    }
    usleep(400000); // TODO: is there a better way? - currently wait 400ms to make sure it has switched over before switching off - even after waiting for target mode.
    aml_dv_disable();
  }
}

void aml_dv_enable()
{
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", "Y");
}

void aml_dv_disable()
{
  CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_enable", "N");
}

bool aml_is_dv_enable()
{
  CSysfsPath dolby_vision_enable{"/sys/module/amdolby_vision/parameters/dolby_vision_enable"};
  return (dolby_vision_enable.Exists() && StringUtils::EqualsNoCase(dolby_vision_enable.Get<std::string>().value(), "Y"));
}

void aml_dv_display_trigger()
{
  if (aml_is_dv_enable()) {
    CSysfsPath display_mode{"/sys/class/display/mode"};
    if (display_mode.Exists()) display_mode.Set(display_mode.Get<std::string>().value());
  }
}

void aml_dv_start()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  enum DV_MODE dv_mode(static_cast<DV_MODE>(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE)));
  if (dv_mode == DV_MODE_ON) {
    aml_dv_off(true);
    aml_dv_on(DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL, true);
  }
}

bool aml_has_frac_rate_policy()
{
  static int has_frac_rate_policy = -1;

  if (has_frac_rate_policy == -1)
  {
    CSysfsPath amhdmitx0_frac_rate_policy{"/sys/class/amhdmitx/amhdmitx0/frac_rate_policy"};
    has_frac_rate_policy = static_cast<int>(amhdmitx0_frac_rate_policy.Exists());
  }

  return (has_frac_rate_policy == 1);
}

bool aml_video_started()
{
  CSysfsPath videostarted{"/sys/class/tsync/videostarted"};
  return (StringUtils::EqualsNoCase(videostarted.Get<std::string>().value(), "0x1"));
}

void aml_video_mute(bool mute)
{
  static int _mute = -1;

  if (_mute == -1 || (_mute != !!mute))
  {
    _mute = !!mute;
    CSysfsPath("/sys/class/amhdmitx/amhdmitx0/vid_mute", _mute);
    CLog::Log(LOGDEBUG, "AMLUtils::{} - {} video", __FUNCTION__, mute ? "mute" : "unmute");
  }
}

void aml_set_audio_passthrough(bool passthrough)
{
  CSysfsPath("/sys/class/audiodsp/digital_raw", (passthrough ? 2 : 0));
}

void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode)
{
  int fd;
  if ((fd = open("/dev/amvideo", O_RDWR)) >= 0)
  {
    if (ioctl(fd, AMSTREAM_IOC_SET_3D_TYPE, mode) != 0)
      CLog::Log(LOGERROR, "AMLUtils::{} - unable to set 3D video mode 0x%x", __FUNCTION__, mode);
    close(fd);

    CSysfsPath("/sys/module/amvideo/parameters/framepacking_support", framepacking_support ? 1 : 0);
    CSysfsPath("/sys/module/amvdec_h264mvc/parameters/view_mode", view_mode);
  }
}

void aml_probe_hdmi_audio()
{
  // Audio {format, channel, freq, cce}
  // {1, 7, 7f, 7}
  // {7, 5, 1e, 0}
  // {2, 5, 7, 0}
  // {11, 7, 7e, 1}
  // {10, 7, 6, 0}
  // {12, 7, 7e, 0}

  int fd = open("/sys/class/amhdmitx/amhdmitx0/edid", O_RDONLY);
  if (fd >= 0)
  {
    char valstr[1024] = {0};

    read(fd, valstr, sizeof(valstr) - 1);
    valstr[strlen(valstr)] = '\0';
    close(fd);

    std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

    for (std::vector<std::string>::const_iterator i = probe_str.begin(); i != probe_str.end(); ++i)
    {
      if (i->find("Audio") == std::string::npos)
      {
        for (std::vector<std::string>::const_iterator j = i + 1; j != probe_str.end(); ++j)
        {
          if      (j->find("{1,")  != std::string::npos)
            printf(" PCM found {1,\n");
          else if (j->find("{2,")  != std::string::npos)
            printf(" AC3 found {2,\n");
          else if (j->find("{3,")  != std::string::npos)
            printf(" MPEG1 found {3,\n");
          else if (j->find("{4,")  != std::string::npos)
            printf(" MP3 found {4,\n");
          else if (j->find("{5,")  != std::string::npos)
            printf(" MPEG2 found {5,\n");
          else if (j->find("{6,")  != std::string::npos)
            printf(" AAC found {6,\n");
          else if (j->find("{7,")  != std::string::npos)
            printf(" DTS found {7,\n");
          else if (j->find("{8,")  != std::string::npos)
            printf(" ATRAC found {8,\n");
          else if (j->find("{9,")  != std::string::npos)
            printf(" One_Bit_Audio found {9,\n");
          else if (j->find("{10,") != std::string::npos)
            printf(" Dolby found {10,\n");
          else if (j->find("{11,") != std::string::npos)
            printf(" DTS_HD found {11,\n");
          else if (j->find("{12,") != std::string::npos)
            printf(" MAT found {12,\n");
          else if (j->find("{13,") != std::string::npos)
            printf(" ATRAC found {13,\n");
          else if (j->find("{14,") != std::string::npos)
            printf(" WMA found {14,\n");
          else
            break;
        }
        break;
      }
    }
  }
}

int aml_axis_value(AML_DISPLAY_AXIS_PARAM param)
{
  std::string axis;
  int value[8];

  CSysfsPath display_axis{"/sys/class/display/axis"};
  if (display_axis.Exists())
    axis = display_axis.Get<std::string>().value();

  sscanf(axis.c_str(), "%d %d %d %d %d %d %d %d", &value[0], &value[1], &value[2], &value[3], &value[4], &value[5], &value[6], &value[7]);

  return value[param];
}

bool aml_mode_to_resolution(const char *mode, RESOLUTION_INFO *res)
{
  if (!res)
    return false;

  res->iWidth = 0;
  res->iHeight= 0;

  if(!mode)
    return false;

  const bool nativeGui = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);
  std::string fromMode = mode;
  StringUtils::Trim(fromMode);
  // strips, for example, 720p* to 720p
  // the * indicate the 'native' mode of the display
  if (StringUtils::EndsWith(fromMode, "*"))
    fromMode.erase(fromMode.size() - 1);

  if (StringUtils::EqualsNoCase(fromMode, "panel"))
  {
    res->iWidth = aml_axis_value(AML_DISPLAY_AXIS_PARAM_WIDTH);
    res->iHeight= aml_axis_value(AML_DISPLAY_AXIS_PARAM_HEIGHT);
    res->iScreenWidth = aml_axis_value(AML_DISPLAY_AXIS_PARAM_WIDTH);
    res->iScreenHeight= aml_axis_value(AML_DISPLAY_AXIS_PARAM_HEIGHT);
    res->fRefreshRate = 60;
    res->dwFlags = D3DPRESENTFLAG_PROGRESSIVE;
  }
  else if (StringUtils::EqualsNoCase(fromMode, "4k2ksmpte") || StringUtils::EqualsNoCase(fromMode, "smpte24hz"))
  {
    res->iWidth = nativeGui ? 4096 : 1920;
    res->iHeight= nativeGui ? 2160 : 1080;
    res->iScreenWidth = 4096;
    res->iScreenHeight= 2160;
    res->fRefreshRate = 24;
    res->dwFlags = D3DPRESENTFLAG_PROGRESSIVE;
  }
  else
  {
    int width = 0, height = 0, rrate = 60;
    char smode[2] = { 0 };

    if (sscanf(fromMode.c_str(), "%dx%dp%dhz", &width, &height, &rrate) == 3)
    {
      *smode = 'p';
    }
    else if (sscanf(fromMode.c_str(), "%d%1[ip]%dhz", &height, smode, &rrate) >= 2)
    {
      switch (height)
      {
        case 480:
        case 576:
          width = 720;
          break;
        case 720:
          width = 1280;
          break;
        case 1080:
          width = 1920;
          break;
        case 2160:
          width = 3840;
          break;
      }
    }
    else if (sscanf(fromMode.c_str(), "%dcvbs", &height) == 1)
    {
      width = 720;
      *smode = 'i';
      rrate = (height == 576) ? 50 : 60;
    }
    else if (sscanf(fromMode.c_str(), "4k2k%d", &rrate) == 1)
    {
      width = 3840;
      height = 2160;
      *smode = 'p';
    }
    else
    {
      return false;
    }

    res->iWidth = nativeGui ? width : std::min(width, 1920);
    res->iHeight= nativeGui ? height : std::min(height, 1080);
    res->iScreenWidth = width;
    res->iScreenHeight = height;
    res->dwFlags = (*smode == 'p') ? D3DPRESENTFLAG_PROGRESSIVE : D3DPRESENTFLAG_INTERLACED;

    switch (rrate)
    {
      case 23:
      case 29:
      case 59:
        res->fRefreshRate = (float)((rrate + 1)/1.001f);
        break;
      default:
        res->fRefreshRate = (float)rrate;
        break;
    }
  }

  res->bFullScreen   = true;
  res->iSubtitles    = (int)(0.965 * res->iHeight);
  res->fPixelRatio   = 1.0f;
  res->strId         = fromMode;
  res->strMode       = StringUtils::Format("{:d}x{:d} @ {:.2f}{} - Full Screen", res->iScreenWidth, res->iScreenHeight, res->fRefreshRate,
    res->dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "");

  if (fromMode.find("FramePacking") != std::string::npos)
    res->dwFlags |= D3DPRESENTFLAG_MODE3DFP;

  if (fromMode.find("TopBottom") != std::string::npos)
    res->dwFlags |= D3DPRESENTFLAG_MODE3DTB;

  if (fromMode.find("SidebySide") != std::string::npos)
    res->dwFlags |= D3DPRESENTFLAG_MODE3DSBS;

  return res->iWidth > 0 && res->iHeight> 0;
}

bool aml_get_native_resolution(RESOLUTION_INFO *res)
{
  std::string mode;
  CSysfsPath display_mode{"/sys/class/display/mode"};
  if (display_mode.Exists())
    mode = display_mode.Get<std::string>().value();
  bool result = aml_mode_to_resolution(mode.c_str(), res);

  if (aml_has_frac_rate_policy())
  {
    int fractional_rate = 0;
    CSysfsPath frac_rate_policy{"/sys/class/amhdmitx/amhdmitx0/frac_rate_policy"};
    if (frac_rate_policy.Exists())
      fractional_rate = frac_rate_policy.Get<int>().value();
    if (fractional_rate == 1)
      res->fRefreshRate /= 1.001f;
  }

  return result;
}

bool aml_set_native_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
  const int stereo_mode, bool force_mode_switch)
{
  bool result = false;

  aml_handle_display_stereo_mode(stereo_mode);
  result = aml_set_display_resolution(res, framebuffer_name, force_mode_switch);
  if (stereo_mode != RENDER_STEREO_MODE_OFF)
    CSysfsPath("/sys/class/amhdmitx/amhdmitx0/phy", 1);


  aml_handle_scale(res);

  return result;
}

bool aml_probe_resolutions(std::vector<RESOLUTION_INFO> &resolutions)
{
  std::string valstr, addstr;

  CSysfsPath user_dcapfile{CSpecialProtocol::TranslatePath("special://home/userdata/disp_cap")};

  if (!user_dcapfile.Exists())
  {
    CSysfsPath dcapfile{"/sys/class/amhdmitx/amhdmitx0/disp_cap"};
    if (dcapfile.Exists())
      valstr = dcapfile.Get<std::string>().value();
    else
      return false;

    CSysfsPath vesa{"/flash/vesa.enable"};
    if (vesa.Exists())
    {
      CSysfsPath vesa_cap{"/sys/class/amhdmitx/amhdmitx0/vesa_cap"};
      if (vesa_cap.Exists())
      {
        addstr = vesa_cap.Get<std::string>().value();
        valstr += "\n" + addstr;
      }
    }

    CSysfsPath custom_mode{"/sys/class/amhdmitx/amhdmitx0/custom_mode"};
    if (custom_mode.Exists())
    {
      addstr = custom_mode.Get<std::string>().value();
      valstr += "\n" + addstr;
    }

    CSysfsPath user_daddfile{CSpecialProtocol::TranslatePath("special://home/userdata/disp_add")};
    if (user_daddfile.Exists())
    {
      addstr = user_daddfile.Get<std::string>().value();
      valstr += "\n" + addstr;
    }
  }
  else
    valstr = user_dcapfile.Get<std::string>().value();

  if (aml_display_support_3d())
  {
    CSysfsPath user_dcapfile_3d{CSpecialProtocol::TranslatePath("special://home/userdata/disp_cap_3d")};
    if (!user_dcapfile_3d.Exists())
    {
      CSysfsPath dcapfile3d{"/sys/class/amhdmitx/amhdmitx0/disp_cap_3d"};
      if (dcapfile3d.Exists())
      {
        addstr = dcapfile3d.Get<std::string>().value();
        valstr += "\n" + addstr;
      }
    }
    else
      valstr = user_dcapfile_3d.Get<std::string>().value();
  }


  std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

  resolutions.clear();
  RESOLUTION_INFO res;
  for (std::vector<std::string>::const_iterator i = probe_str.begin(); i != probe_str.end(); ++i)
  {
    if (((StringUtils::StartsWith(i->c_str(), "4k2k")) && (aml_support_h264_4k2k() > AML_NO_H264_4K2K)) || !(StringUtils::StartsWith(i->c_str(), "4k2k")))
    {
      if (aml_mode_to_resolution(i->c_str(), &res))
        resolutions.push_back(res);

      if (aml_has_frac_rate_policy())
      {
        // Add fractional frame rates: 23.976, 29.97 and 59.94 Hz
        switch ((int)res.fRefreshRate)
        {
          case 24:
          case 30:
          case 60:
            res.fRefreshRate /= 1.001f;
            res.strMode       = StringUtils::Format("{:d}x{:d} @ {:.2f}{} - Full Screen", res.iScreenWidth, res.iScreenHeight, res.fRefreshRate,
              res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "");
            resolutions.push_back(res);
            break;
        }
      }
    }
  }
  return resolutions.size() > 0;
}

bool aml_set_display_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
  bool force_mode_switch)
{
  std::string mode = res.strId.c_str();
  std::string cur_mode;
  std::string custom_mode;
  std::vector<std::string> _mode = StringUtils::Split(mode, ' ');
  std::string mode_options;

  if (_mode.size() > 1)
  {
    mode = _mode[0];
    unsigned int i = 1;
    while(i < (_mode.size() - 1))
    {
      if (i > 1)
        mode_options.append(" ");
      mode_options.append(_mode[i]);
      i++;
    }
    CLog::Log(LOGDEBUG, "{}: try to set mode: {} ({})", __FUNCTION__, mode.c_str(), mode_options.c_str());
  }
  else
    CLog::Log(LOGDEBUG, "{}: try to set mode: {}", __FUNCTION__, mode.c_str());

  CSysfsPath display_mode{"/sys/class/display/mode"};
  if (display_mode.Exists())
    cur_mode = display_mode.Get<std::string>().value();

  CSysfsPath amhdmitx0_custom_mode{"/sys/class/amhdmitx/amhdmitx0/custom_mode"};
  if (amhdmitx0_custom_mode.Exists())
    custom_mode = amhdmitx0_custom_mode.Get<std::string>().value();

  if (custom_mode == mode)
  {
    mode = "custombuilt";
  }

  if (aml_has_frac_rate_policy())
  {
    int cur_fractional_rate;
    int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
    CSysfsPath amhdmitx0_frac_rate_policy{"/sys/class/amhdmitx/amhdmitx0/frac_rate_policy"};
    if (amhdmitx0_frac_rate_policy.Exists())
      cur_fractional_rate = amhdmitx0_frac_rate_policy.Get<int>().value();

    if ((cur_fractional_rate != fractional_rate) || force_mode_switch)
    {
      cur_mode = "null";
      if (display_mode.Exists())
        display_mode.Set(cur_mode);
      if (amhdmitx0_frac_rate_policy.Exists())
        amhdmitx0_frac_rate_policy.Set(fractional_rate);
    }
  }

  if (cur_mode != mode)
  {
    if (display_mode.Exists())
      display_mode.Set(mode);
  }

  aml_set_framebuffer_resolution(res, framebuffer_name);

  return true;
}

void aml_handle_scale(const RESOLUTION_INFO &res)
{
  if (res.iScreenWidth > res.iWidth && res.iScreenHeight > res.iHeight)
    aml_enable_freeScale(res);
  else
    aml_disable_freeScale();
}

void aml_handle_display_stereo_mode(const int stereo_mode)
{
  static int kernel_stereo_mode = -1;

  if (kernel_stereo_mode == -1)
  {
    CSysfsPath _kernel_stereo_mode{"/sys/class/amhdmitx/amhdmitx0/stereo_mode"};
    if (_kernel_stereo_mode.Exists())
      kernel_stereo_mode = _kernel_stereo_mode.Get<int>().value();
  }

  if (kernel_stereo_mode != stereo_mode)
  {
    std::string command = "3doff";
    switch (stereo_mode)
    {
      case RENDER_STEREO_MODE_SPLIT_VERTICAL:
        command = "3dlr";
        break;
      case RENDER_STEREO_MODE_SPLIT_HORIZONTAL:
        command = "3dtb";
        break;
      case RENDER_STEREO_MODE_HARDWAREBASED:
        command = "3dfp";
        break;
      default:
        // nothing - command is already initialised to "3doff"
        break;
    }

    CLog::Log(LOGDEBUG, "AMLUtils::{} setting new mode: {}", __FUNCTION__, command);
    CSysfsPath("/sys/class/amhdmitx/amhdmitx0/config", command);
    kernel_stereo_mode = stereo_mode;
  }
}

void aml_enable_freeScale(const RESOLUTION_INFO &res)
{
  char fsaxis_str[256] = {0};
  sprintf(fsaxis_str, "0 0 %d %d", res.iWidth-1, res.iHeight-1);
  char waxis_str[256] = {0};
  sprintf(waxis_str, "0 0 %d %d", res.iScreenWidth-1, res.iScreenHeight-1);

  CSysfsPath("/sys/class/graphics/fb0/free_scale", 0);
  CSysfsPath("/sys/class/graphics/fb0/free_scale_axis", fsaxis_str);
  CSysfsPath("/sys/class/graphics/fb0/window_axis", waxis_str);
  CSysfsPath("/sys/class/graphics/fb0/free_scale", 0x10001);
}

void aml_disable_freeScale()
{
  // turn off frame buffer freescale
  CSysfsPath("/sys/class/graphics/fb0/free_scale", 0);
  CSysfsPath("/sys/class/graphics/fb1/free_scale", 0);
}

void aml_set_framebuffer_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name)
{
  aml_set_framebuffer_resolution(res.iWidth, res.iHeight, framebuffer_name);
}

void aml_set_framebuffer_resolution(unsigned int width, unsigned int height, std::string framebuffer_name)
{
  int fd0;
  std::string framebuffer = "/dev/" + framebuffer_name;

  if ((fd0 = open(framebuffer.c_str(), O_RDWR)) >= 0)
  {
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd0, FBIOGET_VSCREENINFO, &vinfo) == 0)
    {
      if (width != vinfo.xres || height != vinfo.yres)
      {
        vinfo.xres = width;
        vinfo.yres = height;
        vinfo.xres_virtual = width;
        vinfo.yres_virtual = height * 2;
        vinfo.bits_per_pixel = 32;
        vinfo.activate = FB_ACTIVATE_ALL;
        ioctl(fd0, FBIOPUT_VSCREENINFO, &vinfo);
      }
    }
    close(fd0);
  }
}

bool aml_read_reg(const std::string &reg, uint32_t &reg_val)
{
  CSysfsPath paddr{"/sys/kernel/debug/aml_reg/paddr"};
  if (paddr.Exists())
  {
    paddr.Set(reg);
    std::string val = paddr.Get<std::string>().value();

    CRegExp regexp;
    regexp.RegComp("\\[0x(?<reg>.+)\\][\\s]+=[\\s]+(?<val>.+)");
    if (regexp.RegFind(val) == 0)
    {
      std::string match;
      if (regexp.GetNamedSubPattern("reg", match))
      {
        if (match == reg)
        {
          if (regexp.GetNamedSubPattern("val", match))
          {
            try
            {
              reg_val = std::stoul(match, 0, 16);
              return true;
            }
            catch (...) {}
          }
        }
      }
    }
  }
  return false;
}

bool aml_has_capability_ignore_alpha()
{
  // 4.9 seg faults on access to /sys/kernel/debug/aml_reg/paddr and since we are CE it's always AML
  return true;
}

bool aml_set_reg_ignore_alpha()
{
  if (aml_has_capability_ignore_alpha())
  {
    CSysfsPath fb0_debug{"/sys/class/graphics/fb0/debug"};
    if (fb0_debug.Exists())
    {
      fb0_debug.Set("write 0x1a2d 0x7fc0");
      return true;
    }
  }
  return false;
}

bool aml_unset_reg_ignore_alpha()
{
  if (aml_has_capability_ignore_alpha())
  {
    CSysfsPath fb0_debug{"/sys/class/graphics/fb0/debug"};
    if (fb0_debug.Exists())
    {
      fb0_debug.Set("write 0x1a2d 0x3fc0");
      return true;
    }
  }
  return false;
}
