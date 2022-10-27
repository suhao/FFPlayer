// SDLPlayer.cpp : Defines the entry point for the application.
//

#include "FFPlayer.h"

#include "framework.h"
#include "pch.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <list>
#include <math.h>
#include <memory>
#include <stdbool.h>
#include <stdio.h>
#include <tchar.h>
#include <vector>

#include <winstring.h>

#include "wil/filesystem.h"
#include "wil/stl.h"
#include "wil/wrl.h"
#include <wil/common.h>

#include "SDL.h"
#include "SDL_ttf.h"
#include "SDL_types.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
}

namespace {

std::string SysWideToMultiByte(const std::wstring &wide, uint32_t code_page) {
  int wide_length = static_cast<int>(wide.length());
  if (wide_length == 0)
    return std::string();

  // Compute the length of the buffer we'll need.
  int charcount = WideCharToMultiByte(code_page, 0, wide.data(), wide_length,
                                      NULL, 0, NULL, NULL);
  if (charcount == 0)
    return std::string();

  std::string mb;
  mb.resize(static_cast<size_t>(charcount));
  WideCharToMultiByte(code_page, 0, wide.data(), wide_length, &mb[0], charcount,
                      NULL, NULL);

  return mb;
}

} // namespace

namespace Foundation {

class Notepad {
public:
  Notepad() {
    std::filesystem::path path(wil::GetModuleFileNameW<std::wstring>(nullptr));
    path = path.parent_path().append("msyh.ttf");
    if (std::filesystem::exists(path)) {
      auto multi_byte_path = SysWideToMultiByte(path.c_str(), CP_ACP);
      auto result = TTF_Init();
      font = TTF_OpenFont(multi_byte_path.c_str(), 12);
    }
  }
  ~Notepad() {
    TTF_CloseFont(font);
    font = nullptr;
    TTF_Quit();
  }

  void write(SDL_Renderer *render, const std::string &text, int x, int y,
             int width, int height = 20) {
    if (!render || text.empty()) {
      return;
    }

    SDL_Color color = {255, 0, 0, 255};
    auto surface = TTF_RenderText_Blended(font, text.c_str(), color);
    auto texture = SDL_CreateTextureFromSurface(render, surface);
    SDL_Rect rect = {x, y, width, height};
    SDL_RenderCopy(render, texture, nullptr, &rect);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
  }

private:
  TTF_Font *font = nullptr;
};

void SDL_RenderDrawCircle(SDL_Renderer *render, int x, int y, int radius) {
  float angle = 0;
  constexpr auto step = 5.0f;
  for (int i = 0; i < 360 / step; ++i) {
    float prevRadian = angle * M_PI / 180.0f;
    float nextRadian = (angle + step) * M_PI / 180.0f;
    SDL_RenderDrawLine(
        render, x + radius * cosf(prevRadian), y + radius * sinf(prevRadian),
        x + radius * cosf(nextRadian), y + radius * sinf(nextRadian));
    angle += step;
  }
}

static const Uint64 gFrequency =
    SDL_GetPerformanceFrequency() / 1000; // ticks of per ms.

// 帧率FPS：1s内图像渲染的帧数，或者每秒图形处理器可以刷新几次
// 渲染帧率：渲染前创建对象，渲染后计算并输出即可
// 系统可渲染帧率/事件帧率/非渲染帧率：一次渲染结束到下次渲染开始的帧率
class AutoFramesPerSecond {
public:
  AutoFramesPerSecond() {}
  ~AutoFramesPerSecond() {}

  auto fps(float *elapsedPerformanceTime) {
    auto endCounter = SDL_GetPerformanceCounter();
    auto elapsedCounter = endCounter - startCounter;
    auto elapsedTime = elapsedCounter * 1.0f / gFrequency; // ms

    if (elapsedPerformanceTime) {
      *elapsedPerformanceTime = elapsedTime;
    }

    auto fps = 1 * 1000 / elapsedTime;
    return fps;
  }

  void reset() { startCounter = SDL_GetPerformanceCounter(); }

private:
  Uint64 startCounter = SDL_GetPerformanceCounter();
};

namespace Particle {

struct World {
  struct Partical {
    static constexpr float density = 0.15; // 粒子密度
    int radius = 5;                        // 半径
    int health = 0;                        // 生命值
    int decreaseHealth = 1;                // 生命值减少步长
    SDL_Color color;                       // 颜色
    SDL_FPoint position;                   // 位置
    SDL_FPoint direct;                     // 方向

    void UpdateHealth(int health, int decrease) {
      if (health != 0) {
        this->health = health + rand() % 1000;
      }

      if (decrease == 0)
        decrease = 1;
      this->decreaseHealth =
          (std::max)(decrease, rand() % (this->health / decrease * 10));
    }
  };

  SDL_FPoint gravity;                                     // 重力
  std::vector<std::shared_ptr<Partical>> healthParticals; // 健康粒子

  static std::shared_ptr<World> CreateWorld(SDL_FPoint gravity) {
    // srand(static_cast<unsigned>(time(NULL)));
    auto world = std::make_shared<World>();
    world->gravity = gravity;
    return world;
  }

  void UpdateWorld(SDL_Renderer *render) {
    auto &particals = healthParticals;
    for (auto it = particals.begin(); it != particals.end();) {
      if (!(*it))
        continue;
      auto obj = *it;
      obj->position.x += obj->direct.x + gravity.x / 2.0;
      obj->position.y += obj->direct.y + gravity.y / 2.0;
      SDL_SetRenderDrawColor(render, obj->color.r, obj->color.g, obj->color.b,
                             obj->color.a);
      SDL_RenderDrawCircle(render, obj->position.x, obj->position.y,
                           obj->radius);
      obj->health -= obj->decreaseHealth;

      if (obj->health <= 0) {
        deadParticals.push_back(obj);
        it = particals.erase(it);
      } else {
        ++it;
      }
    }
  }

  void UpdateDegree(float halfDegree, int health, SDL_Point position) {
    shootNumber = static_cast<std::size_t>(
        ceil(halfDegree * 2 * World::Partical::density));

    if (shootNumber > particals.size()) {
      srand(static_cast<unsigned>(time(NULL)));
      std::size_t number = shootNumber - particals.size();
      for (std::size_t i = 0; i < number; ++i) {
        CreatePartical(health, position);
      }
      return;
    }

    if (shootNumber == healthParticals.size()) {
      return;
    }

    if (shootNumber > healthParticals.size()) {
      auto number = shootNumber - healthParticals.size();
      for (std::size_t i = 0; i < number; ++i) {
        auto it = deadParticals.front();
        deadParticals.pop_front();
        if (it) {
          it->UpdateHealth(health, 1);
        }
        healthParticals.push_back(it);
      }
      return;
    }

    std::size_t number = shootNumber - particals.size();
    for (std::size_t i = 0; i < number; ++i) {
      auto index = (std::max)(std::size_t(0),
                              std::size_t(rand() % healthParticals.size() - 1));
      if (healthParticals[index]) {
        healthParticals[index]->UpdateHealth(0, 3);
      }
    }
  }

private:
  std::vector<std::shared_ptr<Partical>> particals;   // 粒子池
  std::list<std::shared_ptr<Partical>> deadParticals; // 死亡粒子
  std::size_t shootNumber = 0;                        // 发射粒子数量
  std::shared_ptr<Partical> CreatePartical(int health, SDL_Point position) {
    auto partical = std::make_shared<Partical>();
    partical->radius = (std::max)(5, rand() % 10);
    partical->color.r = rand() % 255;
    partical->color.g = rand() % 255;
    partical->color.b = rand() % 255;
    partical->color.a = (std::max)(rand() % 255, 180);
    partical->position.x = position.x;
    partical->position.y = position.y;
    partical->UpdateHealth(health, 1);

    particals.push_back(partical);
    healthParticals.push_back(partical);
    return partical;
  }
};

struct Launcher {
  SDL_Point position;     // 发射器的位置
  SDL_FPoint shootDirect; // 参考发射方向
  int health = 100;       // 粒子参考生命值
  float halfDegree;       // 发射口中发射中心的最大角度
  std::weak_ptr<World> world;

  static std::shared_ptr<Launcher> CreateLauncher(SDL_Point position,
                                                  SDL_FPoint direct, int health,
                                                  std::weak_ptr<World> world) {
    auto w = world.lock();
    if (!w)
      return nullptr;

    auto launcher = std::make_shared<Launcher>();
    launcher->position = position;
    launcher->shootDirect = direct;
    launcher->health = health;
    launcher->world = world;

    return launcher;
  }

  void Shoot(float halfDegree) {
    auto w = world.lock();
    if (!w)
      return;
    if (this->halfDegree != halfDegree) {
      w->UpdateDegree(halfDegree, health, position);
    }

    auto &particals = w->healthParticals;

    srand(static_cast<unsigned>(time(NULL)));
    for (auto it = particals.begin(); it != particals.end(); ++it) {
      if (!(*it))
        continue;
      auto randum = rand() % (static_cast<int>(2 * halfDegree * 1000 + 1)) -
                    halfDegree * 1000;
      float degree = randum / 1000.0f;
      float radian = degree * M_PI / 180.0f;
      SDL_FPoint direct = {0, 0};
      direct.x = cosf(radian) * shootDirect.x - sinf(radian) * shootDirect.y;
      direct.y = sinf(radian) * shootDirect.x + cosf(radian) * shootDirect.y;
      (*it)->direct = direct;
    }
  }
};

} // namespace Particle

} // namespace Foundation

namespace stream {

int get_channel_layout_nb_channels(int64_t layout) {
  int nb_channels = 0;
  AVChannelLayout ch_layout;
  av_channel_layout_from_mask(&ch_layout, layout);
  nb_channels = ch_layout.nb_channels;
  av_channel_layout_uninit(&ch_layout);
  return nb_channels;
}

class AudioStream;
std::unique_ptr<AudioStream> gLocalAudioStream;
std::unique_ptr<AudioStream> gFFmpegAudioStream;

class VideoStream;
std::unique_ptr<VideoStream> gFFmpegVideoStream;

// 采样率: 48000，1s中的采样次数
// 采样大小：位深度，一个采样使用多少bit存放，16bit
// 声道数：2
// 时间：1s
constexpr int gAudioMaxFrameSize = 48000 * 16 * 2 * 1 / 8;

constexpr std::size_t gInvalidVernier =
    (std::numeric_limits<std::size_t>::max)();

// 采样时间间隔：每采取一帧音频数据所需的时间间隔
// 采样间隔：如果为20ms，则一秒采集50次
// 每帧音频大小：gAudioPreFrameSize=gAudioMaxFrameSize/50
// 每个通道样本数: gAudioPreChannelFrameSize=gAudioPreFrameSize/2

class AudioStream {
public:
  AudioStream(AVCodecContext *audioCodecContext)
      : _audioCodecContext(audioCodecContext) {
    SDL_AudioSpec spec;
    {
      memset(&spec, 0, sizeof(spec));
      spec.freq = 44100;          // 采样率
      spec.format = AUDIO_S16SYS; // 数据格式
      spec.channels = get_channel_layout_nb_channels(
          AV_CH_LAYOUT_STEREO); // 声道数：1单声道，2立体声
      spec.silence = 0;         // 静音的值
      spec.samples = 1024;      // 采样个数，2的N次方
      spec.userdata = nullptr;  // 自定义数据
      spec.callback = &AudioStream::ReadMixAudioData;
      spec.userdata = nullptr;

      auto maxAudioFrameSize = AudioBufferSize;
      AudioBufferSize = av_samples_get_buffer_size(
          nullptr, spec.channels, spec.samples, AV_SAMPLE_FMT_S16, 1);
    }

    auto result = SDL_OpenAudio(&spec, NULL);

    if (_audioCodecContext) {
      audioSwresampleContext = swr_alloc();

      AVChannelLayout input_channel_layout;
      av_channel_layout_from_mask(&input_channel_layout, AV_CH_LAYOUT_STEREO);
      swr_alloc_set_opts2(&audioSwresampleContext, &input_channel_layout,
                          AV_SAMPLE_FMT_S16, spec.freq,
                          &_audioCodecContext->ch_layout,
                          _audioCodecContext->sample_fmt,
                          _audioCodecContext->sample_rate, 0, nullptr);
      av_channel_layout_uninit(&input_channel_layout);

      swr_init(audioSwresampleContext);

      // Allocate video frame.
      frame = av_frame_alloc();

      bufferSize = gAudioMaxFrameSize * 3 / 2;
    } else {
      std::filesystem::path path(
          wil::GetModuleFileNameW<std::wstring>(nullptr));
      path = path.parent_path().append("demo.pcm");
      if (std::filesystem::exists(path)) {
        auto multi_byte_path = SysWideToMultiByte(path.c_str(), CP_ACP);
        auto r = fopen_s(&handle, multi_byte_path.c_str(), "rb");
        if (handle) {
          fseek(handle, 0, SEEK_END);
          auto size = ftell(handle);
          rewind(handle);

          bufferSize = AudioBufferSize;
        }
      }
    }

    buffer = std::make_unique<char[]>(bufferSize + 1);
    std::memset(buffer.get(), 0, bufferSize + 1);

    // SDL_AudioInit("directsound");
    SDL_PauseAudio(0);
  }
  ~AudioStream() {

    if (handle) {
      fclose(handle);
      handle = nullptr;
    }

    if (frame)
      av_frame_free(&frame);
    frame = nullptr;

    for (auto it = packets.begin(); it != packets.end(); ++it) {
      av_packet_unref(it->get());
    }
    packets.clear();

    if (packet) {
      av_packet_unref(packet.get());
      packet = nullptr;
    }

    if (audioSwresampleContext) {
      swr_free(&audioSwresampleContext);
    }
    audioSwresampleContext = nullptr;

    buffer = nullptr;
  }

  // stream指向需要填充的音频缓冲区
  // length音频缓冲区大小，字节单位
  static void ReadMixAudioData(void *userdata, Uint8 *stream, int length) {
    SDL_memset(stream, 0, length);
    if (length == 0 || (!gLocalAudioStream && !gFFmpegAudioStream))
      return;

    if (gLocalAudioStream) {

      auto data = gLocalAudioStream->read(length);
      if (data == nullptr || length == 0) {
        return;
      }

      SDL_MixAudio(stream, reinterpret_cast<Uint8 *>(data), length,
                   SDL_MIX_MAXVOLUME);
    }

    if (gFFmpegAudioStream) {
      auto data = gFFmpegAudioStream->read(length);
      if (data == nullptr || length == 0) {
        return;
      }

      SDL_MixAudio(stream, reinterpret_cast<Uint8 *>(data), length,
                   SDL_MIX_MAXVOLUME);
    }
  }

  char *read(int &length) {
    if (!buffer)
      return nullptr;

    if (length >= AudioBufferSize) {
      length = AudioBufferSize;
    }

    if (_audioCodecContext) {
      if (vernier != gInvalidVernier) {
        auto pos = buffer.get() + (AudioBufferSize - vernier);
        if (length >= vernier) {
          length = vernier;
          vernier = gInvalidVernier;
          return pos;
        }
        vernier -= length;
        return pos;
      }

      if (!packet) {
        if (packets.empty())
          return nullptr;
        packet.reset(packets.front().release());
        packets.pop_front();
        if (!packet)
          return nullptr;
        avcodec_send_packet(_audioCodecContext, packet.get());
      }

      if (avcodec_receive_frame(_audioCodecContext, frame) != 0) {
        av_packet_unref(packet.get());
        packet = nullptr;
        return read(length);
      }

      auto out = reinterpret_cast<uint8_t *>(buffer.get());
      auto in = const_cast<const uint8_t **>(frame->data);

      auto result = swr_convert(audioSwresampleContext, &out,
                                gAudioMaxFrameSize, in, frame->nb_samples);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        av_packet_unref(packet.get());
        packet = nullptr;
        return read(length);
      } else if (result < 0) {
        return nullptr;
      }

      vernier = AudioBufferSize;
      return read(length);

    } else if (handle) {
      if (feof(handle)) {
        rewind(handle);
      }

      auto audioSize =
          fread_s(buffer.get(), AudioBufferSize, 1, length, handle);
      if (audioSize != length) {
        if (feof(handle)) {
          length = audioSize;
          return buffer.get();
        }
        return nullptr;
      }
    } else {
      return nullptr;
    }

    return buffer.get();
  }

  bool push(AVPacket *packet) {
    packets.push_back(std::unique_ptr<AVPacket>(packet));
    return true;
  }

private:
  // ffprobe.exe demo.mp3=>Audio: mp3, 44100 Hz, stereo, fltp, 320 kb/s
  // ffmpeg.exe -y -i demo.mp3 -acodec pcm_s16le -f s16le -ac 2 -ar 44100
  // demo.pcm
  // =>demo.pcm
  // ffplay -ar 44100 -channels 2 -f s16le -i demo.pcm
  // =>audio
  std::size_t AudioBufferSize = 1024 * 2 * SDL_AUDIO_BITSIZE(AUDIO_S16SYS) / 8;
  std::unique_ptr<char[]> buffer;
  std::size_t bufferSize = 0;

  AVCodecContext *_audioCodecContext = nullptr;
  SwrContext *audioSwresampleContext = nullptr;
  std::list<std::unique_ptr<AVPacket>> packets;
  std::unique_ptr<AVPacket> packet;
  AVFrame *frame = nullptr;
  std::size_t vernier = gInvalidVernier;

  std::FILE *handle = nullptr;
};

class VideoStream {
public:
  VideoStream() {
    std::filesystem::path path(wil::GetModuleFileNameW<std::wstring>(nullptr));
    path = path.parent_path().append("demo.mp4");

    [&]() {
      if (!std::filesystem::exists(path))
        return;
      auto multi_byte_path = SysWideToMultiByte(path.c_str(), CP_ACP);

      // Open input file, result should be zero.
      auto result = avformat_open_input(&formatContext, multi_byte_path.c_str(),
                                        nullptr, nullptr);
      if (result != 0)
        return;

      // Find video stream
      videoStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1,
                                        -1, nullptr, 0);
      if (videoStream == -1)
        return;

      // Find video decoder and initialize a context.
      auto videoCodecParameters = formatContext->streams[videoStream]->codecpar;
      auto videoCodec = avcodec_find_decoder(videoCodecParameters->codec_id);
      if (videoCodec == nullptr)
        return;
      videoCodecContext = avcodec_alloc_context3(videoCodec);
      result = avcodec_parameters_to_context(videoCodecContext,
                                             videoCodecParameters);
      if (result != 0)
        return;

      // open decoder.
      result = avcodec_open2(videoCodecContext, videoCodec, nullptr);
      if (result < 0)
        return;

      // Find audio stream
      audioStream = av_find_best_stream(formatContext, AVMEDIA_TYPE_AUDIO, -1,
                                        -1, nullptr, 0);
      if (audioStream == -1)
        return;

      // Find audio decoder and initialize a context
      auto audioCodecParameters = formatContext->streams[audioStream]->codecpar;
      auto audioCodec = avcodec_find_decoder(audioCodecParameters->codec_id);
      if (audioCodec == nullptr)
        return;
      audioCodecContext = avcodec_alloc_context3(audioCodec);
      result = avcodec_parameters_to_context(audioCodecContext,
                                             audioCodecParameters);

      if (result != 0)
        return;

      // open decoder.
      result = avcodec_open2(audioCodecContext, audioCodec, nullptr);
      if (result < 0)
        return;

      gFFmpegAudioStream = std::make_unique<AudioStream>(audioCodecContext);

      // Allocate video frame.
      frame = av_frame_alloc();

      _width = videoCodecContext->width;
      _height = videoCodecContext->height;
    }();
  }

  ~VideoStream() {
    gFFmpegAudioStream.reset();

    if (frame)
      av_frame_free(&frame);
    frame = nullptr;

    if (packet)
      av_packet_unref(packet.get());
    packet.reset();

    if (videoCodecContext)
      avcodec_close(videoCodecContext);
    videoCodecContext = nullptr;

    if (audioCodecContext) {
      avcodec_close(audioCodecContext);
    }
    audioCodecContext = nullptr;

    if (formatContext)
      avformat_close_input(&formatContext);
    formatContext = nullptr;
  }

  int width() const { return this->_width; }
  int height() const { return this->_height; }

  bool HasFrame() {
    if (!packet) {
      packet = std::make_unique<AVPacket>();
      if (!packet)
        return false;
      if (av_read_frame(formatContext, packet.get()) < 0) {
        av_packet_unref(packet.get());
        packet = nullptr;
        return false;
      }
      if (videoStream != packet->stream_index) {
        if (!gFFmpegAudioStream || packet->stream_index != audioStream ||
            !gFFmpegAudioStream->push(packet.release())) {
          av_packet_unref(packet.get());
          packet = nullptr;
        }
        return HasFrame();
      }
      avcodec_send_packet(videoCodecContext, packet.get());
    }

    if (avcodec_receive_frame(videoCodecContext, frame) != 0) {
      av_packet_unref(packet.get());
      packet = nullptr;
      return HasFrame();
    }
    return true;
  }

  bool Read(SDL_Texture *texture) {
    if (!texture)
      return false;

    if (!HasFrame()) {
      return false;
    }

    SDL_UpdateYUVTexture(texture, nullptr, frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1], frame->data[2],
                         frame->linesize[2]);
    return true;
  }

private:
  AVFormatContext *formatContext = nullptr;
  AVCodecContext *videoCodecContext = nullptr;
  AVCodecContext *audioCodecContext = nullptr;
  AVFrame *frame = nullptr;
  int _width = 0;
  int _height = 0;
  int videoStream = -1;
  int audioStream = -1;
  std::unique_ptr<AVPacket> packet;
};

} // namespace stream

namespace Foundation {

class Window {
public:
  Window() {
    window = SDL_CreateWindow("FFPlayer", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 850, 600,
                              SDL_WINDOW_RESIZABLE);
    render = SDL_CreateRenderer(
        window, -1,
        /*SDL_RENDERER_SOFTWARE | */ SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);

    texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, 850, 600);

    SDL_SetWindowMinimumSize(window, 750, 400);

    //SDL_FPoint gravity = {0, 0};
    //world = Foundation::Particle::World::CreateWorld(gravity);
    //SDL_Point position = {850 / 2, 600};
    //SDL_FPoint direct = {5, -5};
    //launcher = Foundation::Particle::Launcher::CreateLauncher(position, direct,
    //                                                          100, world);
  }
  ~Window() {
    SDL_DestroyTexture(videoTexture);
    videoTexture = nullptr;
    SDL_DestroyTexture(texture);
    texture = nullptr;
    SDL_DestroyRenderer(render);
    render = nullptr;
    SDL_DestroyWindow(window);
    window = nullptr;
  }

  void Paint(AutoFramesPerSecond &fpsCounter) {
    float eventTime = 0;
    auto eventFPS = fpsCounter.fps(&eventTime);

    float renderFPS = 0;
    float renderTime = 0;
    AutoFramesPerSecond renderCounter;

    static constexpr SDL_Rect rect = {0, 0, 850, 600};

    SDL_Rect windowRectangle = rect;
    SDL_GetWindowSize(window, &windowRectangle.w, &windowRectangle.h);

    if (windowRectangle.h == 0)
      return;

    // backgroud: 750*480
    {
      SDL_SetRenderTarget(render, texture);
      SDL_SetRenderDrawColor(render, 255, 255, 255, 255);
      SDL_RenderClear(render);

      SDL_SetRenderTarget(render, nullptr);
      SDL_RenderCopy(render, texture, nullptr, nullptr);
    }

    // video: videoRectangle
    SDL_Rect videoRectangle = windowRectangle;
    float ratio = windowRectangle.w / windowRectangle.h;
    if (ratio >= 0.75f) {
      videoRectangle.h = windowRectangle.h * 0.8f;
      videoRectangle.w = videoRectangle.h * 4 / 3;
    }
    if (ratio < 0.75f || ((windowRectangle.w - videoRectangle.w) < 150)) {
      videoRectangle.w = windowRectangle.w * 80 / 100;
      videoRectangle.h = videoRectangle.w * 3 / 4;
    }

    {
      using namespace stream;
      if (gFFmpegVideoStream) {
        if (!videoTexture) {
          videoTexture = SDL_CreateTexture(
              render, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
              gFFmpegVideoStream->width(), gFFmpegVideoStream->height());
        }

        SDL_SetRenderTarget(render, videoTexture);
        SDL_RenderClear(render);
        gFFmpegVideoStream->Read(videoTexture);
        SDL_SetRenderTarget(render, nullptr);
      }

      SDL_RenderCopy(render, videoTexture, nullptr, &videoRectangle);
    }

    // Wav: {0, videoRectangle.h, rect.w, rect.h - videoRectangle.h}
    {
      SDL_Rect wavRectangle = windowRectangle;
      wavRectangle.y = videoRectangle.h;
      wavRectangle.h -= wavRectangle.y;
      wavRectangle.y += 2;
      wavRectangle.h -= 4;
      SDL_SetRenderTarget(render, texture);

      SDL_SetRenderDrawColor(render, 0, 0, 0, 255);
      SDL_RenderClear(render);
      SDL_RenderDrawRect(render, &rect);
      {
        SDL_BlendMode blendMode = SDL_BLENDMODE_INVALID;
        SDL_GetRenderDrawBlendMode(render, &blendMode);
        SDL_SetRenderDrawBlendMode(render, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(render, 230, 230, 230, 200);
        auto blendRect = rect;
        blendRect.x += 1;
        blendRect.y += 5;
        blendRect.w -= 1;
        blendRect.h -= 10;
        SDL_RenderFillRect(render, &blendRect);
        SDL_SetRenderDrawBlendMode(render, blendMode);
      }

      {
        SDL_SetRenderDrawColor(render, 250, 250, 250, 200);
        SDL_Rect originCoordinate = {0, rect.h / 2, rect.w, 1};
        {
          // 纵轴: (0,0)-(0,h)，宽度压缩比不大可以使用SDL_RenderDrawLine
          SDL_Point start = {0, 0};
          SDL_Point end = {0, rect.h};
          SDL_RenderDrawLine(render, start.x, start.y, end.x, end.y);
        }
        {
          // 横轴: (0,h/2)-(w,h/2)，
          // 高度压缩比过大时使用SDL_RenderDrawLine就无法正常展示了
          // weight系数设为2px: 横轴会稍粗
          // weight系数设置1px：在特定比例下进行换算会出现无法展示问题，原因是计算originCoordinate.y时会除以2；我们对实际高度做+1px处理来解决，
          int weight = 1 * rect.h / wavRectangle.h;
          originCoordinate.y -= weight / 2;
          originCoordinate.h = weight + 1;

          SDL_RenderDrawRect(render, &originCoordinate);
          SDL_RenderFillRect(render, &originCoordinate);
        }

        if (launcher) {
          launcher->Shoot(180);
        }

        if (world) {
          world->UpdateWorld(render);
        }
      }

      SDL_SetRenderTarget(render, nullptr);
      SDL_RenderCopy(render, texture, nullptr, &wavRectangle);
    }

    // FPS
    {
      SDL_Rect notepadRectangle = windowRectangle;
      notepadRectangle.x = videoRectangle.w;
      notepadRectangle.w -= notepadRectangle.x;
      notepadRectangle.h = videoRectangle.h;
      SDL_SetRenderTarget(render, texture);
      SDL_SetRenderDrawColor(render, 250, 250, 250, 255);
      SDL_RenderClear(render);
      SDL_SetRenderTarget(render, nullptr);
      SDL_RenderCopy(render, texture, nullptr, &notepadRectangle);

      renderFPS = renderCounter.fps(&renderTime);
      notepad.write(render, "eventTime: ", notepadRectangle.x, 10, 50);
      notepad.write(render, std::to_string(eventTime) + "ms",
                    notepadRectangle.x + 50, 10, 100);
      notepad.write(render, "eventFPS: ", notepadRectangle.x, 30, 50);
      notepad.write(render, std::to_string(eventFPS) + "ms",
                    notepadRectangle.x + 50, 30, 100);
      notepad.write(render, "renderTime: ", notepadRectangle.x, 50, 50);
      notepad.write(render, std::to_string(renderTime) + "ms",
                    notepadRectangle.x + 50, 50, 100);
      notepad.write(render, "renderFPS: ", notepadRectangle.x, 70, 50);
      notepad.write(render, std::to_string(renderFPS) + "ms",
                    notepadRectangle.x + 50, 70, 100);

      fpsCounter.reset();
    }

    SDL_RenderPresent(render);
  } // namespace Foundation

private:
  Notepad notepad;
  SDL_Window *window = nullptr;
  SDL_Renderer *render = nullptr;
  SDL_Texture *texture = nullptr;
  SDL_Texture *videoTexture = nullptr;
  std::shared_ptr<Particle::Launcher> launcher;
  std::shared_ptr<Particle::World> world;
};

} // namespace Foundation

void RunSimpleFFPlayerDemo() {
  auto window = std::make_unique<Foundation::Window>();

  using namespace stream;
  gLocalAudioStream = std::make_unique<AudioStream>(nullptr);
  gFFmpegVideoStream = std::make_unique<VideoStream>();

  bool quit = false;
  SDL_Event event;
  Foundation::AutoFramesPerSecond Counter;
  while (!quit) {
    if (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT: {
        SDL_Log("quit");
        quit = true;
      } break;
      case SDL_KEYUP:
      case SDL_KEYDOWN: {
        SDL_Log("event: key down, %d", event.key.type);
      } break;

      default:
        SDL_Log("event: %d", event.type);
        break;
      }
    } else {
      window->Paint(Counter);
    }

    SDL_Delay(1);
  }

  gFFmpegVideoStream = nullptr;
  gLocalAudioStream = nullptr;
  window = nullptr;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);

  if (0 != SDL_Init(SDL_INIT_EVERYTHING)) {
    return 1;
  }
  auto config = avcodec_configuration();

  RunSimpleFFPlayerDemo();

  SDL_Quit();
  return 0;
}