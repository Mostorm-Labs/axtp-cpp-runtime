#include "media/render/win32/media_render_host.hpp"
#include "media/model/format.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>

#include <audioclient.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace axtp::mediahost {
namespace {

#include "media/render/win32/common/media_render_common.inc"
#include "media/render/win32/ui/overlay_controls.inc"
#include "media/render/win32/video/h264_bitstream.inc"
#include "media/render/win32/audio/adts_parser.inc"
#include "media/render/win32/video/d3d_h264_renderer.inc"
#include "media/render/win32/audio/wasapi_aac_renderer.inc"

} // namespace
} // namespace axtp::mediahost

namespace axtp::mediahost {

#include "media/render/win32/audio/axtp_audio_renderer.inc"
#include "media/render/win32/video/axtp_video_renderer.inc"
#include "media/render/win32/common/media_render_host_impl.inc"

} // namespace axtp::mediahost
