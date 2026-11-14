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
      if (v.Now() - pts > 150000) show = false; // late: skip the conversion, keep up
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
  v->th = pot((int)((int64_t)v->vh * v->tw / v->vw), 32, 512);
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
void* Texture(Video& v)
{
  std::lock_guard<std::mutex> l(v.frMx);
  if (!v.fresh || v.front.size() < (size_t)v.tw * v.th * 4)
    return v.raster;
  rw::RwRaster* r = rw::RwRasterCreate(v.tw, v.th, 32, static_cast<std::int32_t>(rw::rwRASTERFORMAT8888) | rw::rwRASTERTYPETEXTURE);
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
} // namespace

void luavideo::init3d(void* gtasa)
{
  if (!gtasa)
    return;
  *(void**)&g_im3dTransform = dlsym(gtasa, "_Z15RwIm3DTransformP18RxObjSpace3DVertexjP11RwMatrixTagj");
  *(void**)&g_im3dRenderPrim = dlsym(gtasa, "_Z21RwIm3DRenderPrimitive15RwPrimitiveType");
  *(void**)&g_im3dRenderIdx = dlsym(gtasa, "_Z28RwIm3DRenderIndexedPrimitive15RwPrimitiveTypePti");
  *(void**)&g_im3dEnd = dlsym(gtasa, "_Z9RwIm3DEndv");
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
}

// RenderEffects: the world (and the peds) are drawn, the depth buffer is the scene's
void luavideo::render3d()
{
  g_lastRender = g_frame;
  if (g_draws.empty() || !available3d())
    return;
  using namespace rw;
  int zt = 1, zw = 1, va = 0, cull = 2, fog = 0, filt = 2, addr = 1, src = 5, dst = 6;
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
  RwRenderStateGet(rwRENDERSTATETEXTURERASTER, &ras);
  static std::vector<std::uint16_t> idx;
  for (auto& d : g_draws) {
    if (d.verts.size() < 3)
      continue;
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)(intptr_t)(d.ztest ? 1 : 0));
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)(intptr_t)1);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void*)(intptr_t)0);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void*)(intptr_t)rwBLENDONE);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void*)(intptr_t)rwBLENDZERO);
    RwRenderStateSet(rwRENDERSTATECULLMODE, (void*)(intptr_t)rwCULLMODECULLNONE);
    RwRenderStateSet(rwRENDERSTATEFOGENABLE, (void*)(intptr_t)0);
    RwRenderStateSet(rwRENDERSTATETEXTUREFILTER, (void*)(intptr_t)rwFILTERLINEAR);
    RwRenderStateSet(rwRENDERSTATETEXTUREADDRESS, (void*)(intptr_t)rwTEXTUREADDRESSCLAMP);
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, d.raster);
    const std::uint32_t n = (std::uint32_t)(d.verts.size() / 3 * 3);
    // rwIM3D_VERTEXUV | rwIM3D_VERTEXXYZ | rwIM3D_VERTEXRGBA
    if (g_im3dTransform(d.verts.data(), n, nullptr, 0x01 | 0x08 | 0x10)) {
      if (g_im3dRenderPrim) {
        g_im3dRenderPrim(rwPRIMTYPETRILIST);
      } else {
        idx.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) idx[i] = (std::uint16_t)i;
        g_im3dRenderIdx(rwPRIMTYPETRILIST, idx.data(), (int)n);
      }
      g_im3dEnd();
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
  RwRenderStateSet(rwRENDERSTATETEXTURERASTER, ras);
  g_draws.clear();
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
  state["isRender3DAvailable"] = []() { return luavideo::available3d(); };
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
