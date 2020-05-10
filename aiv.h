#pragma once

typedef struct aiv_image {
  AVFormatContext *format_context;
  int stream_index;
  AVCodecContext *codec_context;
  AVPacket *packet;
  AVFrame *frame;
  struct SwsContext *sws_ctx;
  uint8_t *line[4];
  int linesize[4];
  void *buf;
  int buf_size;
} aiv_image_t;
