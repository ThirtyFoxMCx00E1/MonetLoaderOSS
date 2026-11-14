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
//
// 3.8.5-mp (TV Player black screen / "ghost" fix):
//   - The video is drawn as a real 3D mesh in the world (RwIm3D, inside the game's RenderEffects pass), so the depth
//     buffer works: the player, furniture and walls in front of the TV hide it. renderDrawTextureTriangles drew
//     a flat 2D picture on top of everything, so the TV showed through CJ's body like a ghost.
//       renderDrawTexture3D(tex, {x,y,z,u,v, ...}, color[, depthTest = true])   world-space textured triangles
//       isRender3DAvailable()                                                     false = RenderEffects hook missing
//   - Every new frame goes into a NEW raster (the way MonetLoader's images are made, which always shows). Locking
//     and unlocking the same raster again did not upload the new pixels on the mobile RenderWare driver, so the TV
//     stayed on the video's first frame - which is black in most videos. Old rasters are freed a few frames later.
//   - The texture is a power of two (512x256 etc.): GLES2 shows a non-power-of-two texture as black when the game
//     has REPEAT addressing or mip filtering set.
//   - Decoding goes through AImageReader (YUV_420_888 with the real plane / pixel strides, any vendor format) when
//     the phone has it (Android 7+), the ByteBuffer path stays as the fallback.
//   - The log (logcat tag MonetVideo) says which path is used, the first frame's brightness and the upload count.
#include "luavideo.h"
#include "game/RenderWare.h"
#include "game/CEntity.h"
#include <algorithm>
#include <cctype>
#include "imgui/imgui.h"
#include <SLES/OpenSLES.h>
#include <dlfcn.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <android/native_window.h>
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

// AImageReader / AImage (libmediandk, Android 7+), looked up at run time
struct IrApi {
  bool ok = false;
  media_status_t (*newReader)(int32_t, int32_t, int32_t, int32_t, AImageReader**) = nullptr;
  media_status_t (*getWindow)(AImageReader*, ANativeWindow**) = nullptr;
  media_status_t (*acquireLatest)(AImageReader*, AImage**) = nullptr;
  void (*delReader)(AImageReader*) = nullptr;
  void (*delImage)(AImage*) = nullptr;
  media_status_t (*planeData)(const AImage*, int, uint8_t**, int*) = nullptr;
  media_status_t (*rowStride)(const AImage*, int, int32_t*) = nullptr;
  media_status_t (*pixelStride)(const AImage*, int, int32_t*) = nullptr;
  media_status_t (*cropRect)(const AImage*, AImageCropRect*) = nullptr;
  media_status_t (*numPlanes)(const AImage*, int32_t*) = nullptr;
};
IrApi& Ir()
{
  static IrApi a;
  static bool tried = false;
  if (tried)
    return a;
  tried = true;
  void* h = dlopen("libmediandk.so", RTLD_NOW);
  if (!h)
    return a;
  *(void**)&a.newReader = dlsym(h, "AImageReader_new");
  *(void**)&a.getWindow = dlsym(h, "AImageReader_getWindow");
  *(void**)&a.acquireLatest = dlsym(h, "AImageReader_acquireLatestImage");
  *(void**)&a.delReader = dlsym(h, "AImageReader_delete");
  *(void**)&a.delImage = dlsym(h, "AImage_delete");
  *(void**)&a.planeData = dlsym(h, "AImage_getPlaneData");
  *(void**)&a.rowStride = dlsym(h, "AImage_getPlaneRowStride");
  *(void**)&a.pixelStride = dlsym(h, "AImage_getPlanePixelStride");
  *(void**)&a.cropRect = dlsym(h, "AImage_getCropRect");
  *(void**)&a.numPlanes = dlsym(h, "AImage_getNumberOfPlanes");
  a.ok = a.newReader && a.getWindow && a.acquireLatest && a.delReader && a.delImage && a.planeData && a.rowStride
      && a.pixelStride && a.cropRect && a.numPlanes;
  return a;
}

// rasters that may still be in the game's render queue: freed a few game frames later
std::vector<std::pair<rw::RwRaster*, int>> g_graveyard;
int g_frame = 0;
void Bury(rw::RwRaster* r)
{
  if (r)
    g_graveyard.emplace_back(r, g_frame);
}

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
  AMediaFormat* fmt = nullptr; // the track's format (to configure the codec again)
  std::string mime;
  AImageReader* reader = nullptr; // surface mode (AImageReader) when not null
  int noImage = 0, frames = 0, uploads = 0;
  bool lumaLogged = false;
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

// one YUV 4:2:0 frame (any plane / pixel stride) scaled into the texture, RGBA
struct Planes {
  const uint8_t *y, *u, *v;
  const uint8_t *yEnd, *uEnd, *vEnd;
  int yRow, uvRow, uvPix;
  int cl, ct, cw, ch;
};
void ConvertPlanes(Video& v, const Planes& p)
{
  if (p.cw <= 0 || p.ch <= 0 || v.tw <= 0 || v.th <= 0)
    return;
  v.back.resize((size_t)v.tw * v.th * 4);
  uint8_t* o = v.back.data();
  uint64_t lumaSum = 0;
  for (int y = 0; y < v.th; ++y) {
    const int sy = p.ct + (int)((int64_t)y * p.ch / v.th);
    const uint8_t* yr = p.y + (size_t)sy * p.yRow;
    const size_t cy = (size_t)(sy >> 1) * p.uvRow;
    for (int x = 0; x < v.tw; ++x) {
      const int sx = p.cl + (int)((int64_t)x * p.cw / v.tw);
      const uint8_t* yp = yr + sx;
      int Y = yp < p.yEnd ? *yp : 16;
      const size_t co = cy + (size_t)(sx >> 1) * p.uvPix;
      const uint8_t* up = p.u + co;
      const uint8_t* vp = p.v + co;
      int U = up < p.uEnd ? *up : 128;
      int V = vp < p.vEnd ? *vp : 128;
      lumaSum += Y;
      const int c = (Y - 16) * 298, d = U - 128, e = V - 128;
      o[0] = Clamp((c + 409 * e + 128) >> 8);
      o[1] = Clamp((c - 100 * d - 208 * e + 128) >> 8);
      o[2] = Clamp((c + 516 * d + 128) >> 8);
      o[3] = 255;
      o += 4;
    }
  }
  ++v.frames;
  if (!v.lumaLogged && v.frames >= 1 && (v.frames == 1 || v.frames == 30)) {
    VLOG("%s: frame %d average brightness %d/255 (%s)", v.path.c_str(), v.frames, (int)(lumaSum / ((uint64_t)v.tw * v.th)),
        v.reader ? "AImageReader" : "ByteBuffer");
    if (v.frames == 30) v.lumaLogged = true;
  }
  std::lock_guard<std::mutex> l(v.frMx);
  v.front.swap(v.back);
  v.fresh = true;
}

// ByteBuffer output (the decoder's own layout: I420 or NV12)
void Convert(Video& v, const uint8_t* buf, size_t size)
{
  const int cw = v.cr - v.cl + 1, ch = v.cb - v.ct + 1;
  const size_t ySize = (size_t)v.stride * v.slice;
  if (size < ySize + ySize / 2 - (size_t)v.stride) // the last chroma row may be short
    return;
  const bool planar = v.colorFmt == 19 || v.colorFmt == 20; // I420; everything else is treated as NV12
  Planes p {};
  p.y = buf; p.yEnd = buf + size; p.yRow = v.stride;
  if (planar) {
    p.u = buf + ySize; p.v = buf + ySize + ySize / 4; p.uvRow = v.stride / 2; p.uvPix = 1;
  } else {
    p.u = buf + ySize; p.v = buf + ySize + 1; p.uvRow = v.stride; p.uvPix = 2;
  }
  p.uEnd = p.vEnd = buf + size;
  p.cl = v.cl; p.ct = v.ct; p.cw = cw; p.ch = ch;
  ConvertPlanes(v, p);
}

// AImageReader output: YUV_420_888 with explicit strides
bool ConvertImage(Video& v, AImage* img)
{
  IrApi& a = Ir();
  int32_t np = 0;
  if (a.numPlanes(img, &np) != AMEDIA_OK || np < 3)
    return false;
  uint8_t* d[3] = {};
  int len[3] = {};
  int32_t rs[3] = {}, ps[3] = {};
  for (int i = 0; i < 3; ++i) {
    if (a.planeData(img, i, &d[i], &len[i]) != AMEDIA_OK || !d[i])
      return false;
    a.rowStride(img, i, &rs[i]);
    a.pixelStride(img, i, &ps[i]);
  }
  AImageCropRect cr {};
  if (a.cropRect(img, &cr) != AMEDIA_OK || cr.right <= cr.left || cr.bottom <= cr.top) {
    cr.left = 0; cr.top = 0; cr.right = v.vw; cr.bottom = v.vh;
  }
  Planes p {};
  p.y = d[0]; p.yEnd = d[0] + len[0]; p.yRow = rs[0];
  p.u = d[1]; p.uEnd = d[1] + len[1];
  p.v = d[2]; p.vEnd = d[2] + len[2];
  p.uvRow = rs[1]; p.uvPix = ps[1] > 0 ? ps[1] : 1;
  p.cl = cr.left; p.ct = cr.top; p.cw = cr.right - cr.left; p.ch = cr.bottom - cr.top;
  ConvertPlanes(v, p);
  return true;
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

void FreeReader(Video& v)
{
  if (v.reader) { Ir().delReader(v.reader); v.reader = nullptr; }
}

// (re)makes the decoder: surface = decode into an AImageReader, else ByteBuffer output
bool Configure(Video& v, bool surface)
{
  if (v.codec) { AMediaCodec_stop(v.codec); AMediaCodec_delete(v.codec); v.codec = nullptr; }
  FreeReader(v);
  if (surface && Ir().ok) {
    IrApi& a = Ir();
    ANativeWindow* win = nullptr;
    if (a.newReader(v.vw, v.vh, AIMAGE_FORMAT_YUV_420_888, 4, &v.reader) == AMEDIA_OK && v.reader
        && a.getWindow(v.reader, &win) == AMEDIA_OK && win) {
      v.codec = AMediaCodec_createDecoderByType(v.mime.c_str());
      if (v.codec && AMediaCodec_configure(v.codec, v.fmt, win, nullptr, 0) == AMEDIA_OK && AMediaCodec_start(v.codec) == AMEDIA_OK) {
        VLOG("%s: decoding into AImageReader (YUV_420_888)", v.path.c_str());
        return true;
      }
      if (v.codec) { AMediaCodec_delete(v.codec); v.codec = nullptr; }
    }
    FreeReader(v);
    VLOG("%s: AImageReader not usable, ByteBuffer output", v.path.c_str());
  }
  const int32_t fmts[2] = { 0x7F420888, 21 }; // COLOR_FormatYUV420Flexible, then NV12 (some decoders refuse flexible)
  for (int32_t cf : fmts) {
    AMediaFormat_setInt32(v.fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, cf);
    v.codec = AMediaCodec_createDecoderByType(v.mime.c_str());
    if (v.codec && AMediaCodec_configure(v.codec, v.fmt, nullptr, nullptr, 0) == AMEDIA_OK && AMediaCodec_start(v.codec) == AMEDIA_OK) {
      VLOG("%s: decoding into ByteBuffers (color %d)", v.path.c_str(), cf);
      return true;
    }
    if (v.codec) { AMediaCodec_delete(v.codec); v.codec = nullptr; }
  }
  return false;
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
      if (v.Now() - pts > 150000 && v.frames > 0) show = false; // late: skip the conversion, keep up (never the first)
    }
    if (v.reader) {
      const bool draw = show && v.seekReq.load() < 0;
      AMediaCodec_releaseOutputBuffer(v.codec, (size_t)oi, draw); // true = to the AImageReader
      if (draw) {
        AImage* img = nullptr;
        for (int t = 0; t < 40 && v.run; ++t) { // the frame reaches the reader a moment later
          if (Ir().acquireLatest(v.reader, &img) == AMEDIA_OK && img) break;
          img = nullptr;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (img) {
          if (ConvertImage(v, img)) v.noImage = 0; else ++v.noImage;
          Ir().delImage(img);
        } else {
          ++v.noImage;
        }
        v.posUs = pts;
        if (v.noImage >= 30 && v.frames == 0) {
          // this decoder gives the reader nothing we can read: ByteBuffers from here on
          VLOG("%s: no image from AImageReader after 30 frames, switching to ByteBuffer output", v.path.c_str());
          const int64_t at = v.posUs.load();
          if (!Configure(v, false)) { VLOG("%s: decoder lost", v.path.c_str()); v.ended = true; v.playing = false; AudioState(v, false); return; }
          v.seekReq = at;
          inEos = false;
        }
      }
      continue;
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
  FreeReader(v);
  if (v.fmt) { AMediaFormat_delete(v.fmt); v.fmt = nullptr; }
  if (v.ex) { AMediaExtractor_delete(v.ex); v.ex = nullptr; }
  if (v.fd >= 0) { close(v.fd); v.fd = -1; }
  if (v.aObj) { (*v.aObj)->Destroy(v.aObj); v.aObj = nullptr; v.aPlay = nullptr; v.aSeek = nullptr; v.aVol = nullptr; }
  Bury(v.raster); // may still be queued for drawing this frame
  v.raster = nullptr;
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
  for (size_t i = 0; i < n && !v->fmt; ++i) {
    AMediaFormat* f = AMediaExtractor_getTrackFormat(v->ex, i);
    const char* mime = nullptr;
    if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !strncmp(mime, "video/", 6)) {
      int64_t d = 0;
      if (AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &d)) v->durationUs = d;
      AMediaExtractor_selectTrack(v->ex, i);
      ReadFormat(*v, f);
      v->mime = mime;
      v->fmt = f; // kept (Configure can make the decoder again)
      continue;
    }
    AMediaFormat_delete(f);
  }
  if (!v->fmt || v->vw <= 0 || v->vh <= 0 || !Configure(*v, true)) { VLOG("%s: no video decoder", path.c_str()); Destroy(*v); return 0; }
  // power-of-two texture (GLES2 shows NPOT textures black with REPEAT / mipmaps); the mesh's UVs cover all of it
  auto pot = [](int x, int lo, int hi) { int p = lo; while (p < hi && p * 2 <= x + x / 3) p *= 2; return p; };
  v->tw = pot(v->vw, 64, 512);
  v->th = v->tw; // square (like the font atlas and MP's textures - some phones show non-square ones black); the
                 // mesh's UVs cover the whole texture, so the picture keeps its shape on the TV
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

// script thread (= the render thread in GTA SA): the newest frame into a NEW raster. Locking the same raster again
// did not upload on the mobile driver (the TV stayed on the first, black, frame); a fresh raster made the way
// MonetLoader makes its images is always uploaded. The old one is freed a few frames later.
// RwImage helpers (the way MP makes its working GIF / logo textures: RwImage -> RwImageFindRasterFormat ->
// RwRasterCreate -> RwRasterSetFromImage). Looked up in init3d.
rw::RwImage* (*g_imgCreate)(int, int, int) = nullptr;
rw::RwImage* (*g_imgAlloc)(rw::RwImage*) = nullptr;
int (*g_imgDestroy)(rw::RwImage*) = nullptr;
rw::RwImage* (*g_imgFindFmt)(rw::RwImage*, int, int*, int*, int*, int*) = nullptr;
rw::RwRaster* (*g_rasFromImage)(rw::RwRaster*, rw::RwImage*) = nullptr;
rw::RwImage* g_img = nullptr; // one reusable image (square, the texture size)
int g_imgSide = 0;
int g_texMode = 0; // logged once: 1 = RwImage path, 2 = raster lock fallback

rw::RwRaster* RasterViaImage(Video& v)
{
  if (!g_imgCreate || !g_imgAlloc || !g_imgFindFmt || !g_rasFromImage)
    return nullptr;
  if (!g_img || g_imgSide != v.tw) {
    if (g_img && g_imgDestroy) g_imgDestroy(g_img);
    g_img = g_imgCreate(v.tw, v.th, 32);
    if (!g_img) return nullptr;
    g_imgAlloc(g_img);
    if (!g_img->cpPixels || g_img->stride < v.tw * 4) { if (g_imgDestroy) g_imgDestroy(g_img); g_img = nullptr; return nullptr; }
    g_imgSide = v.tw;
  }
  for (int y = 0; y < v.th; ++y)
    memcpy(g_img->cpPixels + (size_t)y * g_img->stride, v.front.data() + (size_t)y * v.tw * 4, (size_t)v.tw * 4);
  int rw_ = 0, rh = 0, rd = 0, rf = 0;
  g_imgFindFmt(g_img, rw::rwRASTERTYPETEXTURE, &rw_, &rh, &rd, &rf);
  if (rw_ != v.tw || rh != v.th) {
    static bool said = false;
    if (!said) { said = true; VLOG("RwImageFindRasterFormat %dx%d != %dx%d", rw_, rh, v.tw, v.th); }
    return nullptr;
  }
  // 3.8.9-mp: with mip levels (RenderWare makes them), else the game's mipmapped 3D sampling reads the frame as black
  rw::RwRaster* r = rw::RwRasterCreate(rw_, rh, rd, rf | static_cast<int>(rw::rwRASTERFORMATMIPMAP) | static_cast<int>(rw::rwRASTERFORMATAUTOMIPMAP));
  if (!r) r = rw::RwRasterCreate(rw_, rh, rd, rf);
  if (r) r = g_rasFromImage(r, g_img);
  return r;
}

// script thread (= the render thread in GTA SA): the newest frame into a NEW raster, the same way MP makes the GIF
// textures that show on every phone. The old raster is freed a few frames later.
void* Texture(Video& v)
{
  std::lock_guard<std::mutex> l(v.frMx);
  if (!v.fresh || v.front.size() < (size_t)v.tw * v.th * 4)
    return v.raster;
  rw::RwRaster* r = RasterViaImage(v);
  if (r) {
    if (g_texMode != 1) { g_texMode = 1; VLOG("textures: RwImage + RwRasterSetFromImage (%dx%d)", v.tw, v.th); }
  } else {
    // fallback: lock / unlock a new raster (MonetLoader's own image loader)
    r = rw::RwRasterCreate(v.tw, v.th, 32, static_cast<std::int32_t>(rw::rwRASTERFORMAT8888) | rw::rwRASTERTYPETEXTURE |
                           static_cast<std::int32_t>(rw::rwRASTERFORMATMIPMAP) | static_cast<std::int32_t>(rw::rwRASTERFORMATAUTOMIPMAP));
    if (!r) r = rw::RwRasterCreate(v.tw, v.th, 32, static_cast<std::int32_t>(rw::rwRASTERFORMAT8888) | rw::rwRASTERTYPETEXTURE);
    if (!r)
      return v.raster;
    std::uint8_t* locked = rw::RwRasterLock(r, 0, rw::rwRASTERLOCKWRITE | rw::rwRASTERLOCKNOFETCH);
    if (!r->cpPixels && locked)
      r->cpPixels = locked;
    if (!r->cpPixels) {
      r->cpPixels = static_cast<std::uint8_t*>(std::malloc((size_t)4 * r->width * r->height));
      r->stride = 4 * r->width;
    }
    if (r->cpPixels) {
      const int stride = r->stride > 0 ? r->stride : 4 * r->width;
      const int rows = r->height < v.th ? r->height : v.th;
      const size_t row = (size_t)4 * (r->width < v.tw ? r->width : v.tw);
      for (int y = 0; y < rows; ++y)
        memcpy(r->cpPixels + (size_t)y * stride, v.front.data() + (size_t)y * v.tw * 4, row);
    }
    rw::RwRasterUnlock(r);
    if (g_texMode != 2) { g_texMode = 2; VLOG("textures: raster lock fallback (%dx%d)", v.tw, v.th); }
  }
  Bury(v.raster);
  v.raster = r;
  v.fresh = false;
  if (++v.uploads == 1 || v.uploads == 100)
    VLOG("%s: %d frames uploaded (%dx%d raster)", v.path.c_str(), v.uploads, v.tw, v.th);
  return v.raster;
}

// ---- 3D drawing (RwIm3D in the game's RenderEffects pass) ----
struct Im3DVertex { // RxObjSpace3DVertex on Android
  float x, y, z;
  float nx, ny, nz;
  std::uint32_t color; // r, g, b, a bytes
  float u, v;
};
struct Draw3D {
  rw::RwRaster* raster;
  bool ztest;
  std::vector<Im3DVertex> verts;
};
std::vector<Draw3D> g_draws;
void* (*g_im3dTransform)(Im3DVertex*, std::uint32_t, void*, std::uint32_t) = nullptr;
int (*g_im3dRenderPrim)(int) = nullptr;
int (*g_im3dRenderIdx)(int, std::uint16_t*, int) = nullptr;
int (*g_im3dEnd)() = nullptr;
bool g_hooked3d = false;
int g_lastRender = -1000; // g_frame of the last RenderEffects call
rw::RwTexture* (*g_texCreate)(rw::RwRaster*) = nullptr;
void* (*g_clumpForAll)(void*, void* (*)(void*, void*), void*) = nullptr;
void* (*g_geoLock)(void*, int) = nullptr;   // 3.8.8-mp: RpGeometryLock / Unlock make the game build the TV mesh again
void* (*g_geoUnlock)(void*) = nullptr;
void (*g_freeResEntry)(void*) = nullptr;      // 3.8.9-mp: RwResourcesFreeResEntry - drop the cached GPU copy of the TV mesh
int* g_dnOffset = nullptr;                    // 3.8.9-mp: CCustomBuildingDNPipeline::ms_extraVertColourPluginOffset
// 3.9.0-mp: the TV picture as a small model of its own (RpAtomic), drawn by the game's model pipeline
void* (*g_geoCreate)(int, int, std::uint32_t) = nullptr;
int (*g_geoDestroy)(void*) = nullptr;
void* (*g_matCreate)() = nullptr;
int (*g_matDestroy)(void*) = nullptr;
void* (*g_triSetMat)(void*, void*, void*) = nullptr;
void* (*g_atoCreate)() = nullptr;
int (*g_atoDestroy)(void*) = nullptr;
void* (*g_atoSetFrame)(void*, void*) = nullptr;
void* (*g_atoSetGeo)(void*, void*, std::uint32_t) = nullptr;
void* (*g_frmCreate)() = nullptr;
int (*g_frmDestroy)(void*) = nullptr;
void* (*g_atoRender)(void*) = nullptr;
int g_render3dMethod = 1;                     // 1 = model (RpAtomic), 2 = RwIm3D (3.8.5 - 3.8.9)
bool RenderAsModel(const Draw3D& d);          // (below)
void ModelGc();
} // namespace

void luavideo::init3d(void* gtasa)
{
  if (!gtasa)
    return;
  *(void**)&g_im3dTransform = dlsym(gtasa, "_Z15RwIm3DTransformP18RxObjSpace3DVertexjP11RwMatrixTagj");
  *(void**)&g_im3dRenderPrim = dlsym(gtasa, "_Z21RwIm3DRenderPrimitive15RwPrimitiveType");
  *(void**)&g_im3dRenderIdx = dlsym(gtasa, "_Z28RwIm3DRenderIndexedPrimitive15RwPrimitiveTypePti");
  *(void**)&g_im3dEnd = dlsym(gtasa, "_Z9RwIm3DEndv");
  *(void**)&g_imgCreate = dlsym(gtasa, "_Z13RwImageCreateiii");
  *(void**)&g_imgAlloc = dlsym(gtasa, "_Z21RwImageAllocatePixelsP7RwImage");
  *(void**)&g_imgDestroy = dlsym(gtasa, "_Z14RwImageDestroyP7RwImage");
  *(void**)&g_imgFindFmt = dlsym(gtasa, "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_");
  *(void**)&g_rasFromImage = dlsym(gtasa, "_Z20RwRasterSetFromImageP8RwRasterP7RwImage");
  *(void**)&g_texCreate = dlsym(gtasa, "_Z15RwTextureCreateP8RwRaster");
  *(void**)&g_clumpForAll = dlsym(gtasa, "_Z20RpClumpForAllAtomicsP7RpClumpPFP8RpAtomicS2_PvES3_");
  *(void**)&g_geoLock = dlsym(gtasa, "_Z14RpGeometryLockP10RpGeometryi");
  *(void**)&g_geoUnlock = dlsym(gtasa, "_Z16RpGeometryUnlockP10RpGeometry");
  *(void**)&g_freeResEntry = dlsym(gtasa, "_Z23RwResourcesFreeResEntryP10RwResEntry");
  g_dnOffset = static_cast<int*>(dlsym(gtasa, "_ZN25CCustomBuildingDNPipeline30ms_extraVertColourPluginOffsetE"));
  *(void**)&g_geoCreate = dlsym(gtasa, "_Z16RpGeometryCreateiij");
  *(void**)&g_geoDestroy = dlsym(gtasa, "_Z17RpGeometryDestroyP10RpGeometry");
  *(void**)&g_matCreate = dlsym(gtasa, "_Z16RpMaterialCreatev");
  *(void**)&g_matDestroy = dlsym(gtasa, "_Z17RpMaterialDestroyP10RpMaterial");
  *(void**)&g_triSetMat = dlsym(gtasa, "_Z29RpGeometryTriangleSetMaterialP10RpGeometryP10RpTriangleP10RpMaterial");
  *(void**)&g_atoCreate = dlsym(gtasa, "_Z14RpAtomicCreatev");
  *(void**)&g_atoDestroy = dlsym(gtasa, "_Z15RpAtomicDestroyP8RpAtomic");
  *(void**)&g_atoSetFrame = dlsym(gtasa, "_Z16RpAtomicSetFrameP8RpAtomicP7RwFrame");
  *(void**)&g_atoSetGeo = dlsym(gtasa, "_Z19RpAtomicSetGeometryP8RpAtomicP10RpGeometryj");
  *(void**)&g_frmCreate = dlsym(gtasa, "_Z13RwFrameCreatev");
  *(void**)&g_frmDestroy = dlsym(gtasa, "_Z14RwFrameDestroyP7RwFrame");
  *(void**)&g_atoRender = dlsym(gtasa, "_Z27AtomicDefaultRenderCallBackP8RpAtomic");
  VLOG("TV model: RpGeometryCreate %p, RpAtomicCreate %p, AtomicDefaultRenderCallBack %p, RwFrameCreate %p",
       (void*)g_geoCreate, (void*)g_atoCreate, (void*)g_atoRender, (void*)g_frmCreate);
  VLOG("TV screen: RpGeometryLock %p, RwResourcesFreeResEntry %p, DN colour offset %d", (void*)g_geoLock, (void*)g_freeResEntry,
       g_dnOffset ? *g_dnOffset : -1);
  VLOG("Im3D: transform %p, render %p / %p, end %p", (void*)g_im3dTransform, (void*)g_im3dRenderPrim, (void*)g_im3dRenderIdx, (void*)g_im3dEnd);
}

void luavideo::set_hooked3d(bool on) { g_hooked3d = on; }

bool luavideo::available3d()
{
  // the hook must really be called (another mod may have taken the function): seen in the last 60 frames
  return g_hooked3d && g_im3dTransform && g_im3dEnd && (g_im3dRenderPrim || g_im3dRenderIdx) && g_frame - g_lastRender < 60;
}

// the game's frame (CGame::Process, before the scripts): last frame's draws are gone, old rasters are freed
void luavideo::frame_tick()
{
  ++g_frame;
  g_draws.clear();
  size_t keep = 0;
  for (size_t i = 0; i < g_graveyard.size(); ++i) {
    if (g_frame - g_graveyard[i].second > 4)
      rw::RwRasterDestroy(g_graveyard[i].first);
    else
      g_graveyard[keep++] = g_graveyard[i];
  }
  g_graveyard.resize(keep);
  ModelGc();
  restore_screens_if_idle();
}

// RenderEffects: the world (and the peds) are drawn, the depth buffer is the scene's
// 3.8.7-mp: the vertices go into a PERSISTENT ring buffer (2 frames). The phone's RenderWare queues draws for its render
// thread, which can read the vertex data after RwIm3DRenderPrimitive returns: 3.8.5 / 3.8.6 handed it a std::vector that was
// freed right after (the TV mesh stayed black). The game itself always uses static buffers (TempBufferVertices), and
// MonetLoader's ImGui renderer too - that is why the 2D picture showed and the 3D one did not.
namespace {
constexpr std::uint32_t kRingVerts = 6144;
Im3DVertex g_ring[2][kRingVerts];
std::uint16_t g_ringIdx[kRingVerts];
bool g_ringIdxInit = false;
int g_stat3dDraws = 0, g_stat3dOk = 0;
}
void luavideo::render3d()
{
  g_lastRender = g_frame;
  if (g_draws.empty() || !available3d())
    return;
  using namespace rw;
  if (!g_ringIdxInit) {
    for (std::uint32_t i = 0; i < kRingVerts; ++i) g_ringIdx[i] = (std::uint16_t)i;
    g_ringIdxInit = true;
  }
  Im3DVertex* ring = g_ring[g_frame & 1];
  std::uint32_t used = 0;
  int zt = 1, zw = 1, va = 0, cull = 2, fog = 0, filt = 2, addr = 1, src = 5, dst = 6, atf = 5, persp = 1;
  void* ras = nullptr;
  RwRenderStateGet(rwRENDERSTATEZTESTENABLE, &zt);
  RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, &zw);
  RwRenderStateGet(rwRENDERSTATEVERTEXALPHAENABLE, &va);
  RwRenderStateGet(rwRENDERSTATECULLMODE, &cull);
  RwRenderStateGet(rwRENDERSTATEFOGENABLE, &fog);
  RwRenderStateGet(rwRENDERSTATETEXTUREFILTER, &filt);
  RwRenderStateGet(rwRENDERSTATETEXTUREADDRESS, &addr);
  RwRenderStateGet(rwRENDERSTATESRCBLEND, &src);
  RwRenderStateGet(rwRENDERSTATEDESTBLEND, &dst);
  RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, &atf);
  RwRenderStateGet(rwRENDERSTATETEXTUREPERSPECTIVE, &persp);
  RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &ras);
  for (auto& d : g_draws) {
    const std::uint32_t n = (std::uint32_t)(d.verts.size() / 3 * 3);
    if (n < 3 || used + n > kRingVerts)
      continue;
    Im3DVertex* v = ring + used;
    memcpy(v, d.verts.data(), sizeof(Im3DVertex) * n);
    used += n;
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)(intptr_t)(d.ztest ? 1 : 0));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)(intptr_t)1);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)(intptr_t)0);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)(intptr_t)rwBLENDONE);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)(intptr_t)rwBLENDZERO);
    RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)(intptr_t)rwCULLMODECULLNONE);
    RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)(intptr_t)0);
    RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)(intptr_t)8); // rwALPHATESTFUNCTIONALWAYS
    RwRenderStateSet(rwRENDERSTATETEXTUREPERSPECTIVE, (void*)(intptr_t)1);
    RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)(intptr_t)rwFILTERLINEAR);
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)(intptr_t)rwTEXTUREADDRESSCLAMP);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, d.raster);
    ++g_stat3dDraws;
    // 3.9.0-mp: the game's model pipeline (the way it draws the TV itself). RwIm3D drew the mesh but sampled the video
    // frame as pure black on phones, while the same frame showed fine in 2D (RwIm2D).
    if (g_render3dMethod != 2 && RenderAsModel(d)) { ++g_stat3dOk; continue; }
    // rwIM3D_VERTEXUV | rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA
    if (g_im3dTransform(v, n, nullptr, 0x01 | 0x08 | 0x10)) {
      if (g_im3dRenderIdx)
        g_im3dRenderIdx(rwPRIMTYPETRILIST, g_ringIdx, (int)n);
      else
        g_im3dRenderPrim(rwPRIMTYPETRILIST);
      g_im3dEnd();
      ++g_stat3dOk;
    }
  }
  RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)(intptr_t)zt);
  RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)(intptr_t)zw);
  RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)(intptr_t)va);
  RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)(intptr_t)cull);
  RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)(intptr_t)fog);
  RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)(intptr_t)filt);
  RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)(intptr_t)addr);
  RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)(intptr_t)src);
  RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)(intptr_t)dst);
  RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)(intptr_t)atf);
  RwRenderStateSet(rwRENDERSTATETEXTUREPERSPECTIVE, (void*)(intptr_t)persp);
  RwRenderStateSet(rwRENDERSTATETEXTURERASTER, ras);
  g_draws.clear();
}

// ---- 3.8.7-mp: the video on the TV MODEL's own screen material ----
// The game draws the TV with its own pipeline (lighting, depth, everything in front hides it), and the screen material's
// texture is pointed at the video frame. The screen material is the one whose triangles lie on the screen rectangle
// the script gives (tvplayer/screens.lua, model space). RenderWare 3.7 layouts as in Psdk-aml (Android RpGeometry has
// an extra vertexBuffer field).
namespace {
struct XRwObject { std::uint8_t type, subType, flags, privateFlags; void* parent; };
struct XRwLLLink { void* next; void* prev; };
struct XRwObjectHasFrame { XRwObject object; XRwLLLink lFrame; void* sync; };
struct XRpGeometry;
struct XRpAtomic { XRwObjectHasFrame object; void* repEntry; XRpGeometry* geometry; };
struct XRpMaterial { rw::RwTexture* texture; std::uint8_t color[4]; void* pipeline; float surf[3]; std::int16_t refCount; std::int16_t pad; };
struct XRpMaterialList { XRpMaterial** materials; std::int32_t numMaterials; std::int32_t space; };
struct XRpTriangle { std::uint16_t v[3]; std::uint16_t mat; };
struct XRpMorphTarget { void* parent; float sphere[4]; float* verts; float* normals; };
struct XRpGeometry {
  XRwObject object; std::uint32_t flags; std::uint16_t locked; std::int16_t refCount;
  std::int32_t numTriangles, numVertices, numMorphTargets, numTexCoordSets;
  XRpMaterialList matList; XRpTriangle* triangles; void* preLit; float* texCoords[8];
  std::uintptr_t vertexBuffer; // Android build only (Psdk-aml RpGeometry)
  void* mesh; void* repEntry; XRpMorphTarget* morphTarget;
};
struct Swap {
  std::uintptr_t ent;
  XRpMaterial* mat;
  rw::RwTexture* orig;
  rw::RwTexture* ours;
  std::uint8_t color[4];
  int lastFrame;
  std::string name;
  // 3.8.8-mp: the screen's own UVs / vertex colours, put back by Restore
  XRpGeometry* geo = nullptr;
  std::vector<int> vidx;
  std::vector<float> ouv;
  std::vector<std::uint32_t> ocol;
  std::vector<std::uint32_t> onight, oday;   // 3.8.9-mp: the day / night building colours (map TVs use the DN pipeline)
  std::vector<XRpAtomic*> atomics;           // their cached GPU copies are dropped too
};
std::vector<Swap> g_swaps;

bool Plausible(const void* p) { return p && (std::uintptr_t)p > 0x10000 && ((std::uintptr_t)p & 3) == 0; }

// ---- 3.9.0-mp: the TV picture as its own model ----
// An RpGeometry with the screen's triangles (world space, from tvplayer/screens.lua), prelit white, one material whose
// texture's raster is the newest video frame. Drawn with AtomicDefaultRenderCallBack in RenderEffects: the depth
// buffer is the scene's (CJ in front hides it) and the frame is sampled like any other model texture.
struct TvModel { std::uint64_t key; XRpAtomic* atomic; void* frame; rw::RwTexture* tex; int lastFrame; };
std::vector<TvModel> g_models;
std::vector<std::pair<TvModel, int>> g_modelGrave;   // freed a few frames later (the render thread may still use them)
const char* g_modelErr = "not tried";
int g_modelBuilds = 0;

std::uint64_t DrawKey(const Draw3D& d)
{
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::uint32_t x) { h ^= x; h *= 1099511628211ull; };
  mix((std::uint32_t)d.verts.size());
  for (const Im3DVertex& v : d.verts) {
    mix((std::uint32_t)(std::int32_t)(v.x * 200)); mix((std::uint32_t)(std::int32_t)(v.y * 200)); mix((std::uint32_t)(std::int32_t)(v.z * 200));
    mix((std::uint32_t)(std::int32_t)(v.u * 1000)); mix((std::uint32_t)(std::int32_t)(v.v * 1000)); mix(v.color);
  }
  return h;
}

void FreeModel(TvModel& m)
{
  if (m.tex) m.tex->raster = nullptr;   // the video rasters are MonetLoader's (graveyard), not the texture's
  if (m.atomic && g_atoDestroy) g_atoDestroy(m.atomic);   // its geometry -> material -> texture go with it
  if (m.frame && g_frmDestroy) g_frmDestroy(m.frame);
  m.atomic = nullptr; m.frame = nullptr; m.tex = nullptr;
}

void ModelGc()
{
  size_t keep = 0;
  for (size_t i = 0; i < g_modelGrave.size(); ++i) {
    if (g_frame - g_modelGrave[i].second > 4) FreeModel(g_modelGrave[i].first);
    else g_modelGrave[keep++] = g_modelGrave[i];
  }
  g_modelGrave.resize(keep);
  keep = 0;
  for (size_t i = 0; i < g_models.size(); ++i) {
    if (g_frame - g_models[i].lastFrame > 120) g_modelGrave.emplace_back(g_models[i], g_frame);
    else g_models[keep++] = g_models[i];
  }
  g_models.resize(keep);
}

bool BuildModel(const Draw3D& d, TvModel& out)
{
  if (!g_geoCreate || !g_geoUnlock || !g_matCreate || !g_triSetMat || !g_atoCreate || !g_atoSetFrame || !g_atoSetGeo ||
      !g_frmCreate || !g_atoRender || !g_texCreate) { g_modelErr = "RenderWare functions missing"; return false; }
  const int n = (int)(d.verts.size() / 3 * 3);
  if (n < 3 || n > 60000) { g_modelErr = "bad vertex count"; return false; }
  XRpGeometry* g = static_cast<XRpGeometry*>(g_geoCreate(n, n / 3, 0x02 | 0x04 | 0x08));   // POSITIONS | TEXTURED | PRELIT
  if (!g) { g_modelErr = "RpGeometryCreate failed"; return false; }
  if (g->numVertices != n || g->numTriangles != n / 3 || !Plausible(g->morphTarget) || !Plausible(g->morphTarget->verts) ||
      !Plausible(g->texCoords[0]) || !Plausible(g->preLit) || !Plausible(g->triangles)) {
    g_modelErr = "RpGeometry layout not as expected";
    if (g_geoDestroy) g_geoDestroy(g);
    return false;
  }
  float* pv = g->morphTarget->verts;
  float* uv = g->texCoords[0];
  std::uint32_t* pl = static_cast<std::uint32_t*>(g->preLit);
  float cx = 0, cy = 0, cz = 0;
  for (int i = 0; i < n; ++i) {
    const Im3DVertex& v = d.verts[i];
    pv[i * 3] = v.x; pv[i * 3 + 1] = v.y; pv[i * 3 + 2] = v.z;
    uv[i * 2] = v.u; uv[i * 2 + 1] = v.v;
    pl[i] = v.color | 0xFF000000u;   // r, g, b, a bytes
    cx += v.x; cy += v.y; cz += v.z;
  }
  cx /= n; cy /= n; cz /= n;
  float r2 = 0;
  for (int i = 0; i < n; ++i) {
    const float dx = pv[i * 3] - cx, dy = pv[i * 3 + 1] - cy, dz = pv[i * 3 + 2] - cz;
    r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
  }
  g->morphTarget->sphere[0] = cx; g->morphTarget->sphere[1] = cy; g->morphTarget->sphere[2] = cz;
  g->morphTarget->sphere[3] = std::sqrt(r2) + 0.01f;
  XRpMaterial* mat = static_cast<XRpMaterial*>(g_matCreate());
  rw::RwTexture* tex = g_texCreate(d.raster);
  if (!mat || !tex) { g_modelErr = "RpMaterialCreate / RwTextureCreate failed"; if (g_geoDestroy) g_geoDestroy(g); return false; }
  tex->filterAddressing = 2u | (3u << 8) | (3u << 12);   // linear, clamp U / V
  mat->texture = tex;                                      // the material owns the texture (refCount 1)
  mat->color[0] = mat->color[1] = mat->color[2] = mat->color[3] = 255;
  for (int k = 0; k < n / 3; ++k) {
    g->triangles[k].v[0] = (std::uint16_t)(k * 3); g->triangles[k].v[1] = (std::uint16_t)(k * 3 + 1); g->triangles[k].v[2] = (std::uint16_t)(k * 3 + 2);
    g_triSetMat(g, &g->triangles[k], mat);
  }
  if (g_matDestroy) g_matDestroy(mat);   // the geometry's material list keeps it
  g_geoUnlock(g);                        // builds the mesh
  void* atomic = g_atoCreate();
  void* frame = g_frmCreate();
  if (!atomic || !frame) { g_modelErr = "RpAtomicCreate / RwFrameCreate failed"; if (g_geoDestroy) g_geoDestroy(g); return false; }
  g_atoSetFrame(atomic, frame);
  g_atoSetGeo(atomic, g, 0);
  if (g_geoDestroy) g_geoDestroy(g);     // the atomic keeps it
  out.key = DrawKey(d); out.atomic = static_cast<XRpAtomic*>(atomic); out.frame = frame; out.tex = tex; out.lastFrame = g_frame;
  if (++g_modelBuilds <= 3) VLOG("TV model: built (%d triangles)", n / 3);
  g_modelErr = "ok";
  return true;
}

bool RenderAsModel(const Draw3D& d)
{
  const std::uint64_t key = DrawKey(d);
  TvModel* m = nullptr;
  for (auto& x : g_models) if (x.key == key) { m = &x; break; }
  if (!m) {
    if (g_models.size() >= 8) { g_modelGrave.emplace_back(g_models.front(), g_frame); g_models.erase(g_models.begin()); }
    TvModel nm {};
    if (!BuildModel(d, nm)) {
      static int said = 0;
      if (said++ < 3) VLOG("TV model: %s - RwIm3D is used", g_modelErr);
      return false;
    }
    g_models.push_back(nm);
    m = &g_models.back();
  }
  m->lastFrame = g_frame;
  m->tex->raster = d.raster;
  g_atoRender(m->atomic);
  return true;
}

void* CollectAtomic(void* a, void* data) { static_cast<std::vector<XRpAtomic*>*>(data)->push_back((XRpAtomic*)a); return a; }

std::vector<XRpAtomic*> Atomics(std::uintptr_t ent)
{
  std::vector<XRpAtomic*> out;
  CEntity* e = reinterpret_cast<CEntity*>(ent);
  void* obj = reinterpret_cast<void*>(e->m_pRwObject);
  if (!Plausible(obj)) return out;
  const std::uint8_t type = static_cast<XRwObject*>(obj)->type;
  if (type == 1) out.push_back((XRpAtomic*)obj);                       // rpATOMIC
  else if (type == 2 && g_clumpForAll) g_clumpForAll(obj, CollectAtomic, &out); // rpCLUMP
  return out;
}

struct Pick { XRpMaterial* mat = nullptr; XRpGeometry* geo = nullptr; int matIdx = -1; float score = 0; std::string info; };

// c = screen centre, n = screen normal, w / h = screen size (model space)
// 3.8.9-mp: pts = the screen's own vertex positions from tvplayer/screens.lua (made from the game's TV models): the
// material whose triangles sit on those exact points is the screen. The plane test below is the fallback, and it now
// also takes a material that is mostly (not only) on the screen.
Pick FindScreen(std::uintptr_t ent, const float c[3], const float n[3], float w, float h, const std::vector<float>& pts = {})
{
  Pick best, loose, exact;
  int exactTris = 0;
  std::string list;
  for (XRpAtomic* a : Atomics(ent)) {
    XRpGeometry* g = Plausible(a) ? a->geometry : nullptr;
    if (!Plausible(g) || g->object.type != 8) { list += "[geometry?]"; continue; } // rpGEOMETRY
    const int nm = g->matList.numMaterials;
    if (nm < 1 || nm > 64 || !Plausible(g->matList.materials)) continue;
    const bool hasTris = Plausible(g->triangles) && g->numTriangles > 0 && g->numTriangles < 65536 && g->numMorphTargets > 0 &&
                         Plausible(g->morphTarget) && Plausible(g->morphTarget[0].verts) && g->numVertices > 0;
    const float* uv = (g->numTexCoordSets > 0 && Plausible(g->texCoords[0])) ? g->texCoords[0] : nullptr;
    for (int m = 0; m < nm; ++m) {
      XRpMaterial* mat = g->matList.materials[m];
      if (!Plausible(mat)) continue;
      const char* tn = Plausible(mat->texture) ? mat->texture->name : "";
      char nameBuf[33] = {};
      std::strncpy(nameBuf, tn, 32);
      int tris = 0, inside = 0, onPts = 0;
      float area = 0, umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
      if (hasTris) {
        const float* V = g->morphTarget[0].verts;
        for (int t = 0; t < g->numTriangles; ++t) {
          const XRpTriangle& tr = g->triangles[t];
          if ((int)tr.mat != m) continue;
          ++tris;
          bool in = true;
          float p[3][3];
          for (int k = 0; k < 3; ++k) {
            const int vi = tr.v[k];
            if (vi >= g->numVertices) { in = false; break; }
            for (int j = 0; j < 3; ++j) p[k][j] = V[vi * 3 + j];
            const float dx = p[k][0] - c[0], dy = p[k][1] - c[1], dz = p[k][2] - c[2];
            const float d = dx * n[0] + dy * n[1] + dz * n[2];
            const float r2 = dx * dx + dy * dy + dz * dz - d * d;
            const float rmax = 0.5f * std::sqrt(w * w + h * h) * 1.25f + 0.03f;
            if (std::fabs(d) > 0.08f || r2 > rmax * rmax) in = false;
            if (uv) {
              const float u = uv[vi * 2], vv = uv[vi * 2 + 1];
              umin = std::min(umin, u); umax = std::max(umax, u); vmin = std::min(vmin, vv); vmax = std::max(vmax, vv);
            }
          }
          if (!pts.empty()) {
            int hit = 0;
            for (int k = 0; k < 3; ++k)
              for (size_t q = 0; q + 2 < pts.size(); q += 3) {
                const float ex = p[k][0] - pts[q], ey = p[k][1] - pts[q + 1], ez = p[k][2] - pts[q + 2];
                if (ex * ex + ey * ey + ez * ez < 0.012f * 0.012f) { ++hit; break; }
              }
            if (hit == 3) ++onPts;
          }
          if (!in) continue;
          ++inside;
          const float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
          const float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
          const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
          area += 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
      }
      char line[160];
      std::snprintf(line, sizeof line, "%s#%d %s(%d tris, %d on screen, %d on screens.lua, %.0f%% area, uv %.2f-%.2f/%.2f-%.2f)", list.empty() ? "" : ", ",
                    m, nameBuf[0] ? nameBuf : "-", tris, inside, onPts, w * h > 0 ? 100.f * area / (w * h) : 0.f,
                    umin < 1e8f ? umin : 0.f, umax > -1e8f ? umax : 0.f, vmin < 1e8f ? vmin : 0.f, vmax > -1e8f ? vmax : 0.f);
      list += line;
      if (!hasTris || tris == 0) continue;
      // every triangle of the material on the screen rectangle, covering most of it
      const float cover = w * h > 0 ? area / (w * h) : 0;
      if (inside == tris && cover > 0.35f && cover > best.score) {
        best.mat = mat;
        best.geo = g;
        best.matIdx = m;
        best.score = cover;
        best.info = nameBuf;
      }
      // most of its triangles are the screens.lua triangles (the screen material can also cover the TV's sides)
      if (onPts >= 2 && onPts * 2 >= tris && onPts > exactTris) {
        exact.mat = mat; exact.geo = g; exact.matIdx = m; exact.score = 1; exact.info = std::string(nameBuf[0] ? nameBuf : "-") + ", matched screens.lua";
        exactTris = onPts;
      }
      if (inside * 2 >= tris && cover > 0.2f && cover > loose.score) {
        loose.mat = mat; loose.geo = g; loose.matIdx = m; loose.score = cover; loose.info = std::string(nameBuf[0] ? nameBuf : "-") + ", mostly on the screen";
      }
    }
  }
  if (exact.mat) best = exact;
  else if (!best.mat && loose.mat) best = loose;
  if (!best.mat) {
    // no triangle data in memory: a material named like a screen
    for (XRpAtomic* a : Atomics(ent)) {
      XRpGeometry* g = Plausible(a) ? a->geometry : nullptr;
      if (!Plausible(g) || g->object.type != 8 || !Plausible(g->matList.materials)) continue;
      for (int m = 0; m < g->matList.numMaterials && m < 64; ++m) {
        XRpMaterial* mat = g->matList.materials[m];
        if (!Plausible(mat) || !Plausible(mat->texture)) continue;
        char nb[33] = {};
        std::strncpy(nb, mat->texture->name, 32);
        for (char& ch : nb) ch = (char)std::tolower((unsigned char)ch);
        if (std::strstr(nb, "screen") || std::strstr(nb, "scrn")) { best.mat = mat; best.info = nb; best.score = 0.01f; }
      }
    }
  }
  best.info = best.mat ? ("screen material '" + best.info + "'") : ("no screen material in: " + list);
  return best;
}

// 3.8.9-mp: map objects (the TV in CJ's house) are drawn by the day / night building pipeline. It does not use the
// geometry's prelit colours but two own arrays (night, day) from a geometry plugin, so a black night colour on the glass
// kept the screen black whatever the texture was.
struct XDNColours { std::uint32_t* night; std::uint32_t* day; float param; };
XDNColours* DNColours(XRpGeometry* g)
{
  if (!g_dnOffset || *g_dnOffset <= 0 || *g_dnOffset > 4096 || !Plausible(g)) return nullptr;
  auto* d = reinterpret_cast<XDNColours*>(reinterpret_cast<std::uint8_t*>(g) + *g_dnOffset);
  return (Plausible(d->night) || Plausible(d->day)) ? d : nullptr;
}

void Reinstance(XRpGeometry* g, const std::vector<XRpAtomic*>& atomics)
{
  // the phone keeps a ready-made copy of the mesh for the GPU: lock + unlock = build it again from the changed arrays
  if (g_geoLock && g_geoUnlock && Plausible(g)) { g_geoLock(g, 0x18 /* PRELIGHT | TEXCOORDS */); g_geoUnlock(g); }
  // 3.8.9-mp: the mobile pipelines do not always look at the lock flags, and their shader was chosen for the old
  // material (no texture on some TV glass): drop the cached copy, the next draw builds it from scratch
  if (!g_freeResEntry || !Plausible(g)) return;
  if (Plausible(g->repEntry)) g_freeResEntry(g->repEntry);
  for (XRpAtomic* a : atomics)
    if (Plausible(a) && a->geometry == g && Plausible(a->repEntry)) g_freeResEntry(a->repEntry);
}

// 3.8.8-mp: many TV models map their screen to a few texels of a shared texture (or have black vertex colours
// on the glass), so a video swapped into the material showed as one dark colour = a black screen.
// ax = { ox,oy,oz, ux,uy,uz, vx,vy,vz }: model-space point of uv (0,0) and the edges to uv (1,0) / (0,1).
// Every vertex of the screen material gets the uv of its place on the screen and a white vertex colour.
std::string Remap(Swap& s, XRpGeometry* g, int m, const float ax[9])
{
  if (!Plausible(g) || !Plausible(g->triangles) || g->numMorphTargets < 1 || !Plausible(g->morphTarget) || !Plausible(g->morphTarget[0].verts))
    return "no mesh data";
  float* uv = (g->numTexCoordSets > 0 && Plausible(g->texCoords[0])) ? g->texCoords[0] : nullptr;
  if (!uv) return "no uv";
  std::uint32_t* col = ((g->flags & 0x8) && Plausible(g->preLit)) ? static_cast<std::uint32_t*>(g->preLit) : nullptr;
  const float uu = ax[3] * ax[3] + ax[4] * ax[4] + ax[5] * ax[5], vv = ax[6] * ax[6] + ax[7] * ax[7] + ax[8] * ax[8];
  const float uv_ = ax[3] * ax[6] + ax[4] * ax[7] + ax[5] * ax[8], det = uu * vv - uv_ * uv_;
  if (uu < 1e-6f || vv < 1e-6f || std::fabs(det) < 1e-10f) return "bad axes";
  std::vector<char> seen(g->numVertices, 0);
  for (int t = 0; t < g->numTriangles; ++t)
    if ((int)g->triangles[t].mat == m)
      for (int k = 0; k < 3; ++k) if (g->triangles[t].v[k] < g->numVertices) seen[g->triangles[t].v[k]] = 1;
  const float* V = g->morphTarget[0].verts;
  for (int i = 0; i < g->numVertices; ++i) {
    if (!seen[i]) continue;
    s.vidx.push_back(i);
    s.ouv.push_back(uv[i * 2]); s.ouv.push_back(uv[i * 2 + 1]);
    s.ocol.push_back(col ? col[i] : 0);
    const float dx = V[i * 3] - ax[0], dy = V[i * 3 + 1] - ax[1], dz = V[i * 3 + 2] - ax[2];
    const float du = dx * ax[3] + dy * ax[4] + dz * ax[5], dv = dx * ax[6] + dy * ax[7] + dz * ax[8];
    const float u = (du * vv - dv * uv_) / det, v = (dv * uu - du * uv_) / det;
    uv[i * 2] = std::min(1.f, std::max(0.f, u));
    uv[i * 2 + 1] = std::min(1.f, std::max(0.f, v));
    if (col) col[i] = 0xFFFFFFFFu;
  }
  XDNColours* dn = DNColours(g);
  if (dn)
    for (int i : s.vidx) {
      s.onight.push_back(Plausible(dn->night) ? dn->night[i] : 0); s.oday.push_back(Plausible(dn->day) ? dn->day[i] : 0);
      if (Plausible(dn->night)) dn->night[i] = 0xFFFFFFFFu;
      if (Plausible(dn->day)) dn->day[i] = 0xFFFFFFFFu;
    }
  s.geo = g;
  Reinstance(g, s.atomics);
  char buf[128];
  std::snprintf(buf, sizeof buf, "%d vertices remapped%s%s%s", (int)s.vidx.size(), col ? ", white" : "", dn ? ", day+night white" : "",
                g_freeResEntry ? ", rebuilt" : (g_geoLock ? "" : ", no RpGeometryLock"));
  return buf;
}

// 3.8.9-mp: the geometry is only touched while the entity still uses it (a TV streamed out frees its model)
bool GeoAlive(const Swap& s)
{
  if (!Plausible(s.geo) || !Plausible((void*)s.ent)) return false;
  for (XRpAtomic* a : Atomics(s.ent)) if (Plausible(a) && a->geometry == s.geo) return true;
  return false;
}

void Restore(Swap& s)
{
  const bool alive = GeoAlive(s);
  if (alive && Plausible(s.mat) && s.mat->texture == s.ours) {   // texture first, so the rebuilt mesh has the TV's own
    s.mat->texture = s.orig;
    std::memcpy(s.mat->color, s.color, 4);
  }
  if (alive && !s.vidx.empty() && Plausible(s.geo->texCoords[0])) {
    float* uv = s.geo->texCoords[0];
    std::uint32_t* col = ((s.geo->flags & 0x8) && Plausible(s.geo->preLit)) ? static_cast<std::uint32_t*>(s.geo->preLit) : nullptr;
    for (size_t k = 0; k < s.vidx.size(); ++k) {
      const int i = s.vidx[k];
      if (i >= s.geo->numVertices) continue;
      uv[i * 2] = s.ouv[k * 2]; uv[i * 2 + 1] = s.ouv[k * 2 + 1];
      if (col) col[i] = s.ocol[k];
    }
    if (XDNColours* dn = DNColours(s.geo))
      for (size_t k = 0; k < s.vidx.size() && k < s.onight.size(); ++k) {
        if (Plausible(dn->night)) dn->night[s.vidx[k]] = s.onight[k];
        if (Plausible(dn->day)) dn->day[s.vidx[k]] = s.oday[k];
      }
    Reinstance(s.geo, s.atomics);
  }
  s.vidx.clear(); s.ouv.clear(); s.ocol.clear(); s.onight.clear(); s.oday.clear(); s.geo = nullptr;
  // our texture is kept (its refCount is high so nothing destroys it, and it never owns the video raster)
  if (s.ours) s.ours->raster = nullptr;
}
} // namespace

void luavideo::restore_screens_if_idle()
{
  for (size_t i = 0; i < g_swaps.size();) {
    if (g_frame - g_swaps[i].lastFrame > 90) { Restore(g_swaps[i]); g_swaps.erase(g_swaps.begin() + i); }
    else ++i;
  }
}

namespace {
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
  // 3.8.6-mp: decoded frames / uploaded textures (the TV Player tells the player when the decoder gives nothing)
  state["videoGetStats"] = [](int id) {
    Video* v = Get(id);
    return std::make_tuple(v ? v->frames : 0, v ? v->uploads : 0);
  };
  state["isRender3DAvailable"] = []() { return luavideo::available3d(); };
  // 3.8.7-mp: draws queued / drawn by the 3D mesh path (the TV Player prints them)
  state["getRender3DStats"] = []() { return std::make_tuple(g_stat3dDraws, g_stat3dOk, luavideo::available3d(), std::string(g_render3dMethod == 2 ? "im3d" : g_modelErr)); };
  // 3.9.0-mp: 1 = the TV picture as its own model (default), 2 = RwIm3D (3.8.5 - 3.8.9)
  state["setRender3DMethod"] = [](int m) { g_render3dMethod = m == 2 ? 2 : 1; };
  // 3.8.7-mp: the video on the TV model's own screen material.
  //   setEntityScreenTexture(entityPtr, raster, {cx,cy,cz}, {nx,ny,nz}, width, height) -> ok, info
  //   call it every frame with the newest frame; restoreEntityScreenTexture([entityPtr]) puts the TV's texture back
  //   (also done by itself 90 frames after the last call)
  //   3.8.8-mp: optional 7th argument axes = { ox,oy,oz, ux,uy,uz, vx,vy,vz } (uv (0,0) and the edges to (1,0) / (0,1)):
  //   the screen's own uv / vertex colours are replaced so the whole picture shows (put back on restore)
  //   3.8.9-mp: optional 8th argument pts = { x,y,z, x,y,z, ... } the screen's vertex positions (model space)
  state["setEntityScreenTexture"] = [](double entD, void* raster, sol::table c, sol::table nrm, float w, float h, sol::optional<sol::table> axes,
                                       sol::optional<sol::table> ptsT) {
    const std::uintptr_t ent = (std::uintptr_t)entD;
    if (!Plausible((void*)ent) || !raster || !g_texCreate) return std::make_tuple(false, std::string(g_texCreate ? "bad entity / texture" : "RwTextureCreate not found"));
    for (auto& s : g_swaps) {
      if (s.ent != ent) continue;
      if (Plausible(s.mat) && s.mat->texture == s.ours && Plausible(reinterpret_cast<void*>(reinterpret_cast<CEntity*>(ent)->m_pRwObject))) {
        s.ours->raster = static_cast<rw::RwRaster*>(raster);
        s.lastFrame = g_frame;
        return std::make_tuple(true, s.name);
      }
      // the model was streamed out / in again: look again
      Restore(s);
      s.mat = nullptr;
    }
    g_swaps.erase(std::remove_if(g_swaps.begin(), g_swaps.end(), [&](const Swap& s) { return s.ent == ent && !s.mat; }), g_swaps.end());
    const float cc[3] = { c.get_or(1, 0.f), c.get_or(2, 0.f), c.get_or(3, 0.f) };
    float nn[3] = { nrm.get_or(1, 0.f), nrm.get_or(2, -1.f), nrm.get_or(3, 0.f) };
    const float nl = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    if (nl > 0) for (float& x : nn) x /= nl;
    std::vector<float> pts;
    if (ptsT) { const int np = (int)ptsT->size(); for (int i = 1; i <= np && i <= 3 * 512; ++i) pts.push_back(ptsT->get_or(i, 0.f)); }
    Pick p = FindScreen(ent, cc, nn, w, h, pts);
    VLOG("screen texture: %s", p.info.c_str());
    if (!p.mat) return std::make_tuple(false, p.info);
    for (auto& s : g_swaps) // another TV of the same model already shows it
      if (s.mat == p.mat) { s.ours->raster = static_cast<rw::RwRaster*>(raster); s.lastFrame = g_frame; return std::make_tuple(true, s.name); }
    rw::RwTexture* ours = g_texCreate(static_cast<rw::RwRaster*>(raster));
    if (!ours) return std::make_tuple(false, std::string("RwTextureCreate failed"));
    ours->refCount += 100000;               // nothing may destroy it (it would destroy the video raster with it)
    ours->filterAddressing = 0x3302;        // linear, clamp U / V
    std::strncpy(ours->name, "mp_tvplayer", 31);
    Swap s { ent, p.mat, p.mat->texture, ours, {}, g_frame, p.info, nullptr, {}, {}, {}, {}, {}, Atomics(ent) };
    std::memcpy(s.color, p.mat->color, 4);
    // 3.8.9-mp: texture + colour before the remap, so the rebuilt GPU mesh is made with the video's material
    p.mat->color[0] = p.mat->color[1] = p.mat->color[2] = p.mat->color[3] = 255; // a dark screen colour would darken the video
    p.mat->texture = ours;
    if (axes && p.geo && p.matIdx >= 0) {
      float ax[9];
      for (int i = 0; i < 9; ++i) ax[i] = axes->get_or(i + 1, 0.f);
      const std::string r = Remap(s, p.geo, p.matIdx, ax);
      if (r == "no uv" || r == "no mesh data" || r == "bad axes") {
        // 3.8.9-mp: no texture coordinates on the glass = no picture can ever show there: the script uses the 3D mesh
        p.mat->texture = s.orig;
        std::memcpy(p.mat->color, s.color, 4);
        return std::make_tuple(false, p.info + " (" + r + ")");
      }
      s.name += " (" + r + ")";
    }
    VLOG("screen texture: %s", s.name.c_str());
    g_swaps.push_back(s);
    return std::make_tuple(true, s.name);
  };
  state["restoreEntityScreenTexture"] = [](sol::optional<double> entD) {
    for (size_t i = 0; i < g_swaps.size();) {
      if (!entD || g_swaps[i].ent == (std::uintptr_t)*entD) { Restore(g_swaps[i]); g_swaps.erase(g_swaps.begin() + i); }
      else ++i;
    }
  };
  state["renderDrawTexture3D"] = [](void* tex, sol::table verts, std::uint32_t argb, sol::optional<bool> depthTest) {
    if (!tex) return;
    const int n = (int)verts.size() / 5 / 3 * 3;
    if (n < 3 || g_draws.size() > 64) return;
    Draw3D d;
    d.raster = static_cast<rw::RwRaster*>(tex);
    d.ztest = depthTest.value_or(true);
    d.verts.resize(n);
    const std::uint32_t col = ((argb >> 16) & 0xFF) | (((argb >> 8) & 0xFF) << 8) | ((argb & 0xFF) << 16) | (((argb >> 24) & 0xFF) << 24);
    for (int i = 0; i < n; ++i) {
      Im3DVertex& o = d.verts[i];
      o.x = verts[i * 5 + 1]; o.y = verts[i * 5 + 2]; o.z = verts[i * 5 + 3];
      o.nx = 0; o.ny = 0; o.nz = 1;
      o.color = col;
      o.u = verts[i * 5 + 4]; o.v = verts[i * 5 + 5];
    }
    g_draws.push_back(std::move(d));
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
