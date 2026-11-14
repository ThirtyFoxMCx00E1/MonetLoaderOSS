// MonetLoader for MP 3.8.2: a small video player for Lua scripts (tvplayer.lua).
// Video: Android NDK AMediaExtractor + AMediaCodec (hardware decoder, ByteBuffer output, YUV -> RGBA on the
// decoder thread, scaled down to at most 512 px wide) uploaded into an RwRaster that the render functions draw.
// Audio: OpenSL ES plays the same file (any format Android's media player knows: mp4 / aac / mp3 / ...).
// Lua:
//   id = videoOpen(path)                  0 = can't open
//   videoPlay(id) videoPause(id) videoClose(id)
//   videoSeek(id, sec) videoGetPosition(id) videoGetDuration(id) videoIsPlaying(id) videoIsEnded(id)
//   videoSetVolume(id, 0..1) videoSetLoop(id, bool)
//   tex = videoGetTexture(id)             the newest frame (nil until the first one); call every frame
//   w, h = videoGetSize(id)
//   renderDrawTextureTriangles(tex, {x,y,u,v, x,y,u,v, x,y,u,v, ...}, color)   screen-space textured triangles
#include "luavideo.h"
#include "game/RenderWare.h"
#include "imgui/imgui.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define VLOG(...) __android_log_print(ANDROID_LOG_INFO, "MonetVideo", __VA_ARGS__)

namespace {
using clk = std::chrono::steady_clock;

SLObjectItf g_engineObj = nullptr;
SLEngineItf g_engine = nullptr;
SLObjectItf g_mixObj = nullptr;
bool InitSL()
{
  if (g_engine)
    return true;
  if (slCreateEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS)
    return false;
  if ((*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
    return false;
  if ((*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &g_engine) != SL_RESULT_SUCCESS) {
    g_engine = nullptr;
    return false;
  }
  if ((*g_engine)->CreateOutputMix(g_engine, &g_mixObj, 0, nullptr, nullptr) != SL_RESULT_SUCCESS
      || (*g_mixObj)->Realize(g_mixObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
    g_engine = nullptr;
    return false;
  }
  return true;
}

struct Video {
  std::string path;
  int fd = -1;
  AMediaExtractor* ex = nullptr;
  AMediaCodec* codec = nullptr;
  int64_t durationUs = 0;
  int vw = 0, vh = 0, stride = 0, slice = 0, colorFmt = 21, cl = 0, ct = 0, cr = -1, cb = -1;
  int tw = 0, th = 0; // the texture
  std::thread th_;
  std::atomic<bool> run { true }, playing { false }, ended { false }, loop { false };
  std::atomic<int64_t> seekReq { -1 };
  std::atomic<int64_t> posUs { 0 };
  // clock: posUs = base + (now - start) while playing
  std::mutex clkMx;
  int64_t clkBase = 0;
  clk::time_point clkStart;
  // frame hand-over
  std::mutex frMx;
  std::vector<uint8_t> front, back;
  bool fresh = false;
  rw::RwRaster* raster = nullptr;
  // audio
  SLObjectItf aObj = nullptr;
  SLPlayItf aPlay = nullptr;
  SLSeekItf aSeek = nullptr;
  SLVolumeItf aVol = nullptr;
  float volume = 1.0f;

  int64_t Now()
  {
    std::lock_guard<std::mutex> l(clkMx);
    if (!playing)
      return clkBase;
    return clkBase + std::chrono::duration_cast<std::chrono::microseconds>(clk::now() - clkStart).count();
  }
  void SetClock(int64_t us)
  {
    std::lock_guard<std::mutex> l(clkMx);
    clkBase = us;
    clkStart = clk::now();
  }
};

std::map<int, std::unique_ptr<Video>> g_videos;
int g_nextId = 1;

void ReadFormat(Video& v, AMediaFormat* f)
{
  int32_t x = 0;
  if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &x)) v.vw = x;
  if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &x)) v.vh = x;
  v.stride = v.vw;
  v.slice = v.vh;
  if (AMediaFormat_getInt32(f, "stride", &x) && x >= v.vw) v.stride = x;
  if (AMediaFormat_getInt32(f, "slice-height", &x) && x >= v.vh) v.slice = x;
  if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &x)) v.colorFmt = x;
  int32_t l, t, r, b;
  if (AMediaFormat_getInt32(f, "crop-left", &l) && AMediaFormat_getInt32(f, "crop-top", &t)
      && AMediaFormat_getInt32(f, "crop-right", &r) && AMediaFormat_getInt32(f, "crop-bottom", &b)) {
    v.cl = l; v.ct = t; v.cr = r; v.cb = b;
  } else {
    v.cl = 0; v.ct = 0; v.cr = v.vw - 1; v.cb = v.vh - 1;
  }
  VLOG("%s: %dx%d stride %d slice %d color %d crop %d,%d-%d,%d", v.path.c_str(), v.vw, v.vh, v.stride, v.slice, v.colorFmt, v.cl, v.ct, v.cr, v.cb);
}

inline uint8_t Clamp(int c) { return (uint8_t)(c < 0 ? 0 : (c > 255 ? 255 : c)); }

void Convert(Video& v, const uint8_t* buf, size_t size)
{
  const int cw = v.cr - v.cl + 1, ch = v.cb - v.ct + 1;
  if (cw <= 0 || ch <= 0 || v.tw <= 0 || v.th <= 0)
    return;
  const size_t ySize = (size_t)v.stride * v.slice;
  const bool planar = v.colorFmt == 19 || v.colorFmt == 20; // I420; everything else is treated as NV12
  if (size < ySize + ySize / 2 - (size_t)v.stride) // the last chroma row may be short
    return;
  v.back.resize((size_t)v.tw * v.th * 4);
  uint8_t* o = v.back.data();
  for (int y = 0; y < v.th; ++y) {
    const int sy = v.ct + y * ch / v.th;
    const uint8_t* yr = buf + (size_t)sy * v.stride;
    const size_t cy = (size_t)(sy >> 1);
    for (int x = 0; x < v.tw; ++x) {
      const int sx = v.cl + x * cw / v.tw;
      int Y = yr[sx], U, V;
      if (planar) {
        const size_t cs = (size_t)v.stride / 2;
        const size_t off = cy * cs + (sx >> 1);
        U = buf[ySize + off];
        V = buf[ySize + ySize / 4 + off];
      } else {
        const size_t off = ySize + cy * v.stride + (sx & ~1);
        if (off + 1 >= size) { U = V = 128; }
        else { U = buf[off]; V = buf[off + 1]; }
      }
      const int c = (Y - 16) * 298, d = U - 128, e = V - 128;
      o[0] = Clamp((c + 409 * e + 128) >> 8);
      o[1] = Clamp((c - 100 * d - 208 * e + 128) >> 8);
      o[2] = Clamp((c + 516 * d + 128) >> 8);
      o[3] = 255;
      o += 4;
    }
  }
  std::lock_guard<std::mutex> l(v.frMx);
  v.front.swap(v.back);
  v.fresh = true;
}

void AudioSeek(Video& v, int64_t us)
{
  if (v.aSeek)
    (*v.aSeek)->SetPosition(v.aSeek, (SLmillisecond)(us / 1000), SL_SEEKMODE_ACCURATE);
}
void AudioState(Video& v, bool play)
{
  if (v.aPlay)
    (*v.aPlay)->SetPlayState(v.aPlay, play ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_PAUSED);
}
void AudioVolume(Video& v)
{
  if (!v.aVol)
    return;
  const float g = v.volume <= 0.001f ? 0.0f : v.volume;
  SLmillibel mb = g <= 0.0f ? SL_MILLIBEL_MIN : (SLmillibel)std::lround(2000.0 * std::log10(g));
  if (mb < SL_MILLIBEL_MIN) mb = SL_MILLIBEL_MIN;
  (*v.aVol)->SetVolumeLevel(v.aVol, mb);
}

void Worker(Video* pv)
{
  Video& v = *pv;
  bool inEos = false;
  int64_t dropUntil = -1;
  while (v.run) {
    const int64_t sk = v.seekReq.exchange(-1);
    if (sk >= 0) {
      AMediaExtractor_seekTo(v.ex, sk, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
      AMediaCodec_flush(v.codec);
      inEos = false;
      v.ended = false;
      dropUntil = sk;
      v.SetClock(sk);
      v.posUs = sk;
      AudioSeek(v, sk);
    }
    if (!v.playing && dropUntil < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (!inEos) {
      const ssize_t ii = AMediaCodec_dequeueInputBuffer(v.codec, 2000);
      if (ii >= 0) {
        size_t cap = 0;
        uint8_t* ib = AMediaCodec_getInputBuffer(v.codec, (size_t)ii, &cap);
        const ssize_t n = ib ? AMediaExtractor_readSampleData(v.ex, ib, cap) : -1;
        if (n < 0) {
          AMediaCodec_queueInputBuffer(v.codec, (size_t)ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          inEos = true;
        } else {
          AMediaCodec_queueInputBuffer(v.codec, (size_t)ii, 0, (size_t)n, AMediaExtractor_getSampleTime(v.ex), 0);
          AMediaExtractor_advance(v.ex);
        }
      }
    }
    AMediaCodecBufferInfo info;
    const ssize_t oi = AMediaCodec_dequeueOutputBuffer(v.codec, &info, 5000);
    if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* f = AMediaCodec_getOutputFormat(v.codec);
      ReadFormat(v, f);
      AMediaFormat_delete(f);
      continue;
    }
    if (oi < 0)
      continue;
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      AMediaCodec_releaseOutputBuffer(v.codec, (size_t)oi, false);
      if (v.loop) {
        v.seekReq = 0;
      } else {
        v.ended = true;
        v.playing = false;
        AudioState(v, false);
      }
      continue;
    }
    const int64_t pts = info.presentationTimeUs;
    bool show = true;
    if (dropUntil >= 0) {
      if (pts + 40000 < dropUntil) show = false; // decode up to the seek point
      else dropUntil = -1;
    } else {
      // wait for the frame's time (the clock follows real time while playing)
      while (v.run && v.seekReq.load() < 0) {
        const int64_t now = v.Now();
        if (pts <= now + 5000) break;
        if (!v.playing) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        const int64_t w = pts - now;
        std::this_thread::sleep_for(std::chrono::microseconds(w > 20000 ? 20000 : w));
      }
      if (v.Now() - pts > 150000) show = false; // late: skip the conversion, keep up
    }
    if (show && v.seekReq.load() < 0) {
      size_t sz = 0;
      uint8_t* ob = AMediaCodec_getOutputBuffer(v.codec, (size_t)oi, &sz);
      if (ob && info.size > 0)
        Convert(v, ob + info.offset, (size_t)info.size);
      v.posUs = pts;
    }
    AMediaCodec_releaseOutputBuffer(v.codec, (size_t)oi, false);
  }
}

bool OpenAudio(Video& v)
{
  if (!InitSL())
    return false;
  std::string uri = "file://" + v.path;
  SLDataLocator_URI loc = { SL_DATALOCATOR_URI, (SLchar*)uri.c_str() };
  SLDataFormat_MIME fmt = { SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_UNSPECIFIED };
  SLDataSource src = { &loc, &fmt };
  SLDataLocator_OutputMix outMix = { SL_DATALOCATOR_OUTPUTMIX, g_mixObj };
  SLDataSink sink = { &outMix, nullptr };
  const SLInterfaceID ids[2] = { SL_IID_SEEK, SL_IID_VOLUME };
  const SLboolean req[2] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
  if ((*g_engine)->CreateAudioPlayer(g_engine, &v.aObj, &src, &sink, 2, ids, req) != SL_RESULT_SUCCESS) {
    v.aObj = nullptr;
    return false;
  }
  if ((*v.aObj)->Realize(v.aObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
    (*v.aObj)->Destroy(v.aObj);
    v.aObj = nullptr;
    return false;
  }
  (*v.aObj)->GetInterface(v.aObj, SL_IID_PLAY, &v.aPlay);
  (*v.aObj)->GetInterface(v.aObj, SL_IID_SEEK, &v.aSeek);
  (*v.aObj)->GetInterface(v.aObj, SL_IID_VOLUME, &v.aVol);
  AudioState(v, false);
  AudioVolume(v);
  return true;
}

void Destroy(Video& v)
{
  v.run = false;
  if (v.th_.joinable())
    v.th_.join();
  if (v.codec) { AMediaCodec_stop(v.codec); AMediaCodec_delete(v.codec); v.codec = nullptr; }
  if (v.ex) { AMediaExtractor_delete(v.ex); v.ex = nullptr; }
  if (v.fd >= 0) { close(v.fd); v.fd = -1; }
  if (v.aObj) { (*v.aObj)->Destroy(v.aObj); v.aObj = nullptr; v.aPlay = nullptr; v.aSeek = nullptr; v.aVol = nullptr; }
  if (v.raster) { rw::RwRasterDestroy(v.raster); v.raster = nullptr; }
}

int Open(const std::string& path)
{
  auto v = std::make_unique<Video>();
  v->path = path;
  v->fd = open(path.c_str(), O_RDONLY);
  if (v->fd < 0) { VLOG("can't open %s", path.c_str()); return 0; }
  struct stat st;
  fstat(v->fd, &st);
  v->ex = AMediaExtractor_new();
  if (AMediaExtractor_setDataSourceFd(v->ex, v->fd, 0, st.st_size) != AMEDIA_OK) { VLOG("%s: not a video", path.c_str()); Destroy(*v); return 0; }
  const size_t n = AMediaExtractor_getTrackCount(v->ex);
  for (size_t i = 0; i < n && !v->codec; ++i) {
    AMediaFormat* f = AMediaExtractor_getTrackFormat(v->ex, i);
    const char* mime = nullptr;
    if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !strncmp(mime, "video/", 6)) {
      int64_t d = 0;
      if (AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &d)) v->durationUs = d;
      AMediaExtractor_selectTrack(v->ex, i);
      ReadFormat(*v, f);
      AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888); // COLOR_FormatYUV420Flexible
      v->codec = AMediaCodec_createDecoderByType(mime);
      if (v->codec && AMediaCodec_configure(v->codec, f, nullptr, nullptr, 0) != AMEDIA_OK) {
        // some decoders refuse the flexible format: their default
        AMediaCodec_delete(v->codec);
        v->codec = AMediaCodec_createDecoderByType(mime);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21);
        if (v->codec && AMediaCodec_configure(v->codec, f, nullptr, nullptr, 0) != AMEDIA_OK) { AMediaCodec_delete(v->codec); v->codec = nullptr; }
      }
    }
    AMediaFormat_delete(f);
  }
  if (!v->codec || AMediaCodec_start(v->codec) != AMEDIA_OK || v->vw <= 0 || v->vh <= 0) { VLOG("%s: no video decoder", path.c_str()); Destroy(*v); return 0; }
  v->tw = v->vw > 512 ? 512 : v->vw;
  v->th = (int)((int64_t)v->vh * v->tw / v->vw);
  v->tw &= ~1; v->th &= ~1;
  if (v->th < 2) v->th = 2;
  if (!OpenAudio(*v)) VLOG("%s: no sound (OpenSL ES)", path.c_str());
  Video* raw = v.get();
  v->th_ = std::thread(Worker, raw);
  const int id = g_nextId++;
  g_videos[id] = std::move(v);
  VLOG("opened %s as %d (%dx%d -> %dx%d, %.1f s)", path.c_str(), id, raw->vw, raw->vh, raw->tw, raw->th, raw->durationUs / 1e6);
  return id;
}

Video* Get(int id)
{
  auto it = g_videos.find(id);
  return it == g_videos.end() ? nullptr : it->second.get();
}

// render thread (= the script thread in GTA SA): the newest frame into the raster
void* Texture(Video& v)
{
  std::lock_guard<std::mutex> l(v.frMx);
  if (!v.fresh)
    return v.raster;
  if (!v.raster)
    v.raster = rw::RwRasterCreate(v.tw, v.th, 32, static_cast<std::int32_t>(rw::rwRASTERFORMAT8888) | rw::rwRASTERTYPETEXTURE);
  if (!v.raster)
    return nullptr;
  rw::RwRasterLock(v.raster, 0, rw::rwRASTERLOCKWRITE | rw::rwRASTERLOCKNOFETCH);
  bool own = false;
  if (!v.raster->cpPixels) {
    v.raster->cpPixels = static_cast<std::uint8_t*>(std::malloc((size_t)4 * v.raster->width * v.raster->height));
    v.raster->stride = 4 * v.raster->width;
    own = true;
  }
  if (v.raster->cpPixels) {
    const int rows = v.raster->height < v.th ? v.raster->height : v.th;
    const size_t row = (size_t)4 * (v.raster->width < v.tw ? v.raster->width : v.tw);
    for (int y = 0; y < rows; ++y)
      memcpy(v.raster->cpPixels + (size_t)y * v.raster->stride, v.front.data() + (size_t)y * v.tw * 4, row);
  }
  rw::RwRasterUnlock(v.raster);
  (void)own;
  v.fresh = false;
  return v.raster;
}
} // namespace

void luavideo::reg(sol::state& state)
{
  state["videoOpen"] = [](std::string path) { return Open(path); };
  state["videoClose"] = [](int id) {
    auto it = g_videos.find(id);
    if (it == g_videos.end()) return;
    Destroy(*it->second);
    g_videos.erase(it);
  };
  state["videoPlay"] = [](int id) {
    Video* v = Get(id);
    if (!v || v->playing) return;
    if (v->ended) { v->ended = false; v->seekReq = 0; }
    v->SetClock(v->posUs.load());
    v->playing = true;
    AudioState(*v, true);
  };
  state["videoPause"] = [](int id) {
    Video* v = Get(id);
    if (!v || !v->playing) return;
    v->SetClock(v->Now());
    v->playing = false;
    AudioState(*v, false);
  };
  state["videoSeek"] = [](int id, double sec) {
    Video* v = Get(id);
    if (!v) return;
    int64_t us = (int64_t)(sec * 1e6);
    if (us < 0) us = 0;
    if (v->durationUs > 0 && us > v->durationUs) us = v->durationUs - 100000;
    if (us < 0) us = 0;
    v->seekReq = us;
  };
  state["videoGetPosition"] = [](int id) { Video* v = Get(id); return v ? v->posUs.load() / 1e6 : 0.0; };
  state["videoGetDuration"] = [](int id) { Video* v = Get(id); return v ? v->durationUs / 1e6 : 0.0; };
  state["videoIsPlaying"] = [](int id) { Video* v = Get(id); return v && v->playing.load(); };
  state["videoIsEnded"] = [](int id) { Video* v = Get(id); return v && v->ended.load(); };
  state["videoSetVolume"] = [](int id, float vol) {
    Video* v = Get(id);
    if (!v) return;
    v->volume = vol < 0 ? 0 : (vol > 1 ? 1 : vol);
    AudioVolume(*v);
  };
  state["videoSetLoop"] = [](int id, bool on) { if (Video* v = Get(id)) v->loop = on; };
  state["videoGetTexture"] = [](int id) -> void* { Video* v = Get(id); return v ? Texture(*v) : nullptr; };
  state["videoGetSize"] = [](int id) {
    Video* v = Get(id);
    return std::make_tuple(v ? v->vw : 0, v ? v->vh : 0);
  };
  state["renderDrawTextureTriangles"] = [](void* tex, sol::table verts, std::uint32_t argb) {
    if (!tex) return;
    const int n = (int)verts.size() / 4;
    if (n < 3) return;
    const int tris = n / 3 * 3;
    const ImU32 col = IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushTextureID(tex);
    dl->PrimReserve(tris, tris);
    for (int i = 0; i < tris; ++i) {
      const float x = verts[i * 4 + 1], y = verts[i * 4 + 2], u = verts[i * 4 + 3], v = verts[i * 4 + 4];
      dl->PrimVtx(ImVec2(x, y), ImVec2(u, v), col);
    }
    dl->PopTextureID();
  };
}
