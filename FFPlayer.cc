// SDLPlayer.cpp : Defines the entry point for the application.
//

#include "FFPlayer.h"

#include "framework.h"
#include "pch.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdio.h>
#include <tchar.h>

#include <winstring.h>

#include "wil/filesystem.h"
#include "wil/stl.h"
#include "wil/wrl.h"
#include <wil/common.h>

#include "SDL.h"
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

namespace window {

class Window {
public:
  Window() {
    rect.w = rect.h = 50;
    window = SDL_CreateWindow("FFPlayer", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 640, 480,
                              SDL_WINDOW_RESIZABLE);
    render = SDL_CreateRenderer(
        window, -1,
        /*SDL_RENDERER_SOFTWARE | */ SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);

    texture = SDL_CreateTexture(render, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, 640, 480);
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

  void Paint() {
    SDL_SetRenderTarget(render, texture);

    SDL_SetRenderDrawColor(render, 255, 255, 255, 255);
    SDL_RenderClear(render);

    SDL_Rect rectangle;
    rectangle.x = rectangle.y = 10;
    rectangle.w = rectangle.h = 50;
    SDL_SetRenderDrawColor(render, 255, 0, 0, 255);
    SDL_RenderDrawRect(render, &rectangle);
    SDL_SetRenderDrawColor(render, 0, 0, 0, 255);
    SDL_RenderFillRect(render, &rectangle);

    rect.x = rand() % 600;
    rect.y = rand() % 400;
    SDL_RenderDrawRect(render, &rect);

    SDL_SetRenderTarget(render, nullptr);

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

    SDL_Rect textureRectangle{0, 0, 200, 150};
    SDL_RenderCopy(render, texture, nullptr, &textureRectangle);

    textureRectangle.x = 200;
    textureRectangle.y = 150;
    textureRectangle.w = 440;
    textureRectangle.h = 330;
    SDL_RenderCopy(render, videoTexture, nullptr, &textureRectangle);

    SDL_RenderPresent(render);
  }

private:
  SDL_Window *window = nullptr;
  SDL_Renderer *render = nullptr;
  SDL_Texture *texture = nullptr;
  SDL_Texture *videoTexture = nullptr;
  SDL_Rect rect;
};

} // namespace window

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  UNREFERENCED_PARAMETER(lpCmdLine);

  if (0 != SDL_Init(SDL_INIT_EVERYTHING)) {
    return 1;
  }
  auto config = avcodec_configuration();

  auto window = std::make_unique<window::Window>();

  using namespace stream;
  gLocalAudioStream = std::make_unique<AudioStream>(nullptr);
  gFFmpegVideoStream = std::make_unique<VideoStream>();

  bool quit = false;
  SDL_Event event;
  while (!quit) {
    if (SDL_PollEvent(&event)) {
      switch (event.type) {
      case SDL_QUIT: {
        SDL_Log("quit");
        quit = true;
      } break;
      default:
        SDL_Log("event: %d", event.type);
        break;
      }
    }
    window->Paint();

    SDL_Delay(1);
  }

  gFFmpegVideoStream = nullptr;
  gLocalAudioStream = nullptr;
  window = nullptr;

  SDL_Quit();
  return 0;
}