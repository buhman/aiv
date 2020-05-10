#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <fcntl.h>

#include <xcb/xcb.h>
#include <xcb/xcb_errors.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "aiv.h"

void
enprintf(int err, const char *fmt, ...)
{
  if (!(err < 0)) return;

  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  exit(1);
}

void
eaprintf(int err, const char *fmt, ...)
{
  va_list ap;
  char buf[1024];

  if (!(err < 0)) return;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  av_strerror(err, buf, sizeof (buf));
  fprintf(stderr, ": %s\n", buf);

  exit(1);
}

int
get_av_pix_fmt_from_visualid(xcb_visualid_t visual_id,
                             enum AVPixelFormat *pix_fmt)
{
  size_t i;
  struct pixel_format_entry {
    xcb_visualid_t visual_id;
    enum AVSampleFormat pix_fmt;
  } pixel_format_table[] = {
    { 33, AV_PIX_FMT_RGB32 },
  };
  struct pixel_format_entry *entry;

  for (i = 0; i < (sizeof (pixel_format_table) / sizeof (struct pixel_format_entry)); i++) {
    entry = &pixel_format_table[i];
    if (visual_id == entry->visual_id) {
      *pix_fmt = entry->pix_fmt;
      return 0;
    }
  }

  return -1;
}


void
find_visual_by_id(xcb_screen_t *screen,
                  xcb_visualtype_t **visual_out)
{
  xcb_depth_iterator_t depth_iter;
  xcb_visualtype_iterator_t visual_iter;
  xcb_visualtype_t *visual;
  int depth;

  depth_iter = xcb_screen_allowed_depths_iterator(screen);
  while (depth_iter.rem) {
    visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
    depth = depth_iter.data->depth;

    while (visual_iter.rem) {
      if (screen->root_visual == visual_iter.data->visual_id) {
        visual = visual_iter.data;
        goto found_visual;
      }
      xcb_visualtype_next(&visual_iter);
    }
    xcb_depth_next(&depth_iter);
  }

 found_visual:
  if (visual == NULL) enprintf(-1, "did not find visual: %d\n", visual);

  printf("visual: depth=%i red_mask=%x green_mask=%x blue_mask=%x\n",
         depth, visual->red_mask, visual->green_mask, visual->blue_mask);

  *visual_out = visual;
}

void
print_xcb_error(xcb_errors_context_t *ctx,
                xcb_value_error_t *err)
{
  const char *extension;
  const char *name;

  name = xcb_errors_get_name_for_error(ctx, err->error_code, &extension);
  fprintf(stderr, "error: major=%s minor=%s extension=%s name=%s sequence=%i bad_value=%i\n",
          xcb_errors_get_name_for_major_code(ctx, err->major_opcode),
          xcb_errors_get_name_for_minor_code(ctx, err->major_opcode, err->minor_opcode),
          extension,
          name,
          err->sequence,
          err->bad_value);
}

int
open_image(const char *filename,
           enum AVPixelFormat output_pixel_format,
           aiv_image_t *aiv_image)
{
  AVCodec *codec;
  int ret;

  aiv_image->format_context = NULL;
  ret = avformat_open_input(&aiv_image->format_context, filename, NULL, NULL);
  eaprintf(ret, "avformat_open_input: %s", filename);

  assert(avformat_find_stream_info(aiv_image->format_context, NULL) == 0);

  ret = av_find_best_stream(aiv_image->format_context, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  eaprintf(ret, "av_find_best_stream");

  aiv_image->stream_index = ret;
  aiv_image->codec_context = avcodec_alloc_context3(codec);
  assert(aiv_image->codec_context != NULL);

  ret = avcodec_parameters_to_context(aiv_image->codec_context,
                                      aiv_image->format_context->streams[aiv_image->stream_index]->codecpar);
  eaprintf(ret, "avcodec_parameters_to_context");

  ret = avcodec_open2(aiv_image->codec_context, codec, NULL);
  eaprintf(ret, "avcodec_open2");

  fprintf(stderr, "stream[%i]: codec=%s pix_fmt=%s\n",
          aiv_image->stream_index,
          codec->name,
          av_get_pix_fmt_name(aiv_image->codec_context->pix_fmt));

  //

  aiv_image->packet = av_packet_alloc();
  aiv_image->frame = av_frame_alloc();

  //

  AVCodecContext *codec_context = aiv_image->codec_context;

  aiv_image->sws_ctx = sws_getContext(// source
                                      codec_context->width,
                                      codec_context->height,
                                      codec_context->pix_fmt,
                                      // destination
                                      codec_context->width,
                                      codec_context->height,
                                      output_pixel_format,
                                      // scale
                                      SWS_LANCZOS,
                                      NULL, NULL, NULL);
  assert(aiv_image->sws_ctx != NULL);

  //

  ret = av_image_alloc(aiv_image->line,
                       aiv_image->linesize,
                       codec_context->width,
                       codec_context->height,
                       output_pixel_format,
                       1);
  eaprintf(ret, "av_image_alloc");


  ret = av_image_get_buffer_size(output_pixel_format,
                                 codec_context->width,
                                 codec_context->height,
                                 1);
  eaprintf(ret, "av_image_get_buffer_size");

  aiv_image->buf_size = ret;
  aiv_image->buf = malloc(aiv_image->buf_size);

  return 0;
}

void
free_image(aiv_image_t *aiv_image)
{
  avformat_close_input(&aiv_image->format_context);
  avcodec_free_context(&aiv_image->codec_context);

  av_packet_free(&aiv_image->packet);
  av_frame_free(&aiv_image->frame);

  sws_freeContext(aiv_image->sws_ctx);

  av_freep(&aiv_image->line[0]);
  free(aiv_image->buf);
}

int
next_frame(aiv_image_t *aiv_image)
{
  AVCodecContext *codec_context;
  AVPacket *packet;
  AVFrame *frame;
  int ret;

  codec_context = aiv_image->codec_context;
  packet = aiv_image->packet;
  frame = aiv_image->frame;

  while (av_read_frame(aiv_image->format_context, packet) >= 0) {
    if (packet->stream_index != aiv_image->stream_index)
      continue;

    ret = avcodec_send_packet(codec_context, packet);
    eaprintf(ret, "avcodec_send_packet");

    while (ret >= 0) {
      ret = avcodec_receive_frame(codec_context, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      else
        eaprintf(ret, "avcodec_recieve_frame");

      assert(frame->width == codec_context->width);
      assert(frame->height == codec_context->height);

      /* convert to destination format */
      sws_scale(aiv_image->sws_ctx,
                // source
                (const uint8_t * const*)frame->data,
                frame->linesize,
                //
                0,
                frame->height,
                // destination
                aiv_image->line,
                aiv_image->linesize);

      ret = av_image_copy_to_buffer(// destination
                                    aiv_image->buf,
                                    aiv_image->buf_size,
                                    // source
                                    (const uint8_t **)aiv_image->line,
                                    aiv_image->linesize,
                                    //
                                    AV_PIX_FMT_RGB32,
                                    frame->width,
                                    frame->height,
                                    1);

      eaprintf(ret, "av_image_copy_to_buffer");

      return 0;
    }

    av_packet_unref(packet);
  }

  return 0;
}

int
main(int argc, char *argv[])
{

  xcb_connection_t *connection;
  xcb_window_t window;
  xcb_screen_t *screen;
  xcb_visualtype_t *visual;
  xcb_errors_context_t *error_context;

  uint32_t mask = 0;
  uint32_t values[2];

  int ret;

  connection = xcb_connect(NULL, NULL);

  screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

  window = xcb_generate_id(connection);

  mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  values[0] = screen->white_pixel;
  values[1] = XCB_EVENT_MASK_EXPOSURE
    | XCB_EVENT_MASK_KEY_PRESS
    | XCB_EVENT_MASK_BUTTON_PRESS
    | XCB_EVENT_MASK_BUTTON_RELEASE
    | XCB_EVENT_MASK_POINTER_MOTION;
  xcb_create_window(connection,
                    XCB_COPY_FROM_PARENT,
                    window,
                    screen->root,
                    0, 0,
                    150, 150,
                    10,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual,
                    mask, values);

  xcb_map_window(connection, window);
  find_visual_by_id(screen, &visual);

  //

  aiv_image_t aiv_image;
  xcb_pixmap_t pixmap;
  xcb_gcontext_t gc;
  int image_index;
  xcb_get_geometry_reply_t *geometry;

  image_index = 1;

  void
  load_image()
  {
    ret = open_image(argv[image_index], AV_PIX_FMT_RGB32, &aiv_image);
    enprintf(ret, "open_image[%d]: %s", image_index, argv[image_index]);

    pixmap = xcb_generate_id(connection);
    xcb_create_pixmap(connection, screen->root_depth, pixmap, screen->root,
                      aiv_image.codec_context->width,
                      aiv_image.codec_context->height);

    gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, pixmap, 0, NULL);
  }

  //

  int x;
  int y;
  int origin_x = 0;
  int origin_y = 0;
  int drag_state = 0;

  x = 0;
  y = 0;

  void
  load_frame()
  {
    ret = next_frame(&aiv_image);
    enprintf(ret, "next_frame");

    xcb_put_image(connection,
                  XCB_IMAGE_FORMAT_Z_PIXMAP,
                  pixmap,
                  gc,
                  //
                  aiv_image.codec_context->width,
                  aiv_image.codec_context->height,
                  0,
                  0,
                  //
                  0,
                  screen->root_depth,
                  aiv_image.buf_size,
                  aiv_image.buf);
  }

  void
  render(uint8_t clear)
  {
    fprintf(stderr, "render: geometry: %d %d\n", geometry->width, geometry->height);

    xcb_clear_area(connection,
                   clear,
                   window,
                   0,
                   0,
                   geometry->width,
                   geometry->height);

    xcb_copy_area(connection, pixmap, window, gc,
                  0, 0,
                  x, y,
                  aiv_image.codec_context->width,
                  aiv_image.codec_context->height);
  }

  //

  load_image();
  load_frame();

  //

  ret = xcb_errors_context_new(connection, &error_context);
  enprintf(ret, "xcb_errors_context_new");
  xcb_generic_event_t *event;
  while ((event = xcb_wait_for_event(connection))) {
    switch (event->response_type & ~0x80) {
    case 0:
      {
        xcb_value_error_t *err = (xcb_value_error_t *)event;
        print_xcb_error(error_context, err);
      }
      break;
    case XCB_EXPOSE:
      {
        xcb_expose_event_t *expose = (xcb_expose_event_t *)event;
        fprintf(stderr, "%d %d %d %d\n", expose->x, expose->y, expose->width, expose->height);

        geometry = xcb_get_geometry_reply(connection,
                                          xcb_get_geometry(connection, window),
                                          NULL);

        render(0);
      }
      break;
    case XCB_BUTTON_PRESS:
      {
        xcb_button_press_event_t *button = (xcb_button_press_event_t *)event;
        fprintf(stderr, "p");

        origin_x = button->event_x;
        origin_y = button->event_y;
        drag_state = 1;
      }
      break;
    case XCB_BUTTON_RELEASE:
      {
        xcb_button_release_event_t *button = (xcb_button_release_event_t *)event;
        fprintf(stderr, "r");
        //xcb_button_release_event_t *button = (xcb_button_release_event_t *)event;
        origin_x = button->event_x;
        origin_y = button->event_y;
        drag_state = 0;
      }
      break;
    case XCB_MOTION_NOTIFY:
      {
        //fprintf(stderr, "%d %d\n", x, y);
        xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t *)event;
        if (drag_state == 1
            && x != motion->event_x - origin_x
            && y != motion->event_y - origin_y) {
          x = motion->event_x - origin_x;
          y = motion->event_y - origin_y;
          render(1);
        }
      }
      break;
    case XCB_KEY_PRESS:
      {
        xcb_key_press_event_t *key = (xcb_key_press_event_t *)event;
        switch (key->detail) {
        case 65: // space
          image_index++;
          if (image_index == argc)
            image_index = 1;
          load_image();
          load_frame();
          render(1);
          fprintf(stderr, "next image");

          break;
        default:
          fprintf(stderr, "key %d\n", key->detail);
          break;
        }

      }
      break;
    default:
      fprintf(stderr, "unhandled event: %d\n", event->response_type);
      break;
    }
    free(event);
    xcb_flush(connection);
  }

  xcb_disconnect(connection);

  return 0;
}
