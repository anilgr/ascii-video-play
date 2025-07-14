/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * API example for decoding and filtering (with dynamic scale for ASCII output)
 */

#define _XOPEN_SOURCE 600 /* for usleep */
#include <unistd.h>      // For usleep (though not used in single-frame mode)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>      // For snprintf, av_strdup
#include <math.h>        // For round() and other math functions

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>    // For AV_TIME_BASE_Q
#include <libavutil/avstring.h> // For av_strdup
#include <libavutil/log.h>     // For av_log, AV_LOG_ERROR
#include <libavutil/error.h>   // For av_err2str
#include <libavutil/rational.h> // For av_q2d

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;

#define MAX_ASCII_WIDTH 80 // Max characters per line for ASCII output
// Characters are typically taller than they are wide.
// A typical terminal font has a character aspect ratio (width/height) of around 0.5.
// To make the video appear with its original proportions in ASCII,
// we need to effectively "stretch" the width or "compress" the height based on this factor.
#define CHARACTER_ASPECT_RATIO 0.5

static int open_input_file(const char *filename);
static int init_filters(int input_width, int input_height); // Updated prototype
static void display_frame(const AVFrame *frame, AVRational time_base);


static int open_input_file(const char *filename)
{
    int ret;
    const AVCodec *dec = NULL; // Initialize dec to NULL

    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file %s\n", filename);
        return ret;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    // Explicit cast for av_find_best_stream to satisfy strict compilers.
    // &dec is passed as `const AVCodec **` which `av_find_best_stream` expects.
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, (const AVCodec **)&dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    video_stream_index = ret;
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);

    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }

    return 0;
}

static int init_filters(int input_width, int input_height)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE }; // Output grayscale

    // Retrieve the stream's time_base for the buffer source
    AVRational stream_time_base = fmt_ctx->streams[video_stream_index]->time_base;

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    // Using original frame width/height, pixel format, and time base from stream
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             input_width, input_height, dec_ctx->pix_fmt,
             stream_time_base.num, stream_time_base.den, // Use stream_time_base
             dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filtergraph. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
        goto end;
    }

    /* Set the endpoints for the filtergraph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    // --- DYNAMIC SCALE CALCULATION ---
    double video_width = input_width;
    double video_height = input_height;

    // Account for display aspect ratio if available
    if (dec_ctx->sample_aspect_ratio.num > 0 && dec_ctx->sample_aspect_ratio.den > 0) {
        AVRational sar = dec_ctx->sample_aspect_ratio;
        video_width = video_width * av_q2d(sar);
    }

    double video_display_aspect_ratio = video_width / video_height;
    double target_width;
    double target_height;

    // Adjust for terminal character aspect ratio (characters are typically taller than wide)
    // This helps the video appear with its correct visual proportions in ASCII characters.
    double adjusted_aspect_ratio = video_display_aspect_ratio / CHARACTER_ASPECT_RATIO;

    // Prioritize fitting within MAX_ASCII_WIDTH
    target_width = MAX_ASCII_WIDTH;
    target_height = round(target_width / adjusted_aspect_ratio);

    // Ensure dimensions are positive and even numbers (many filters prefer even dimensions)
    if (target_height < 1) target_height = 1;
    target_width = (double)((int)round(target_width / 2.0) * 2); // Make it even
    target_height = (double)((int)round(target_height / 2.0) * 2); // Make it even

    // Ensure we don't end up with 0 dimensions in case of extremely small calculated values
    if (target_width == 0) target_width = 2;
    if (target_height == 0) target_height = 2;

    char filters_descr[128]; // Buffer for the generated filter string

    // Generate the filter string: "scale=W:H,format=gray"
    snprintf(filters_descr, sizeof(filters_descr), "scale=%d:%d,format=gray",
             (int)target_width, (int)target_height);

    av_log(NULL, AV_LOG_INFO, "Input video resolution: %dx%d (Pixel Aspect Ratio: %d:%d, Display Aspect Ratio: %f)\n",
           input_width, input_height,
           dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den,
           video_display_aspect_ratio);
    av_log(NULL, AV_LOG_INFO, "Terminal character aspect ratio compensation: %f\n", CHARACTER_ASPECT_RATIO);
    av_log(NULL, AV_LOG_INFO, "Applying filter: \"%s\"\n", filters_descr);
    av_log(NULL, AV_LOG_INFO, "Output ASCII dimensions (characters): %dx%d\n",
           (int)target_width, (int)target_height);


    ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                   &inputs, &outputs, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot parse graph description: %s\n", av_err2str(ret));
        goto end;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot configure filter graph: %s\n", av_err2str(ret));
        goto end;
    }

end:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static void display_frame(const AVFrame *frame, AVRational time_base)
{
    int x, y;
    uint8_t *p0, *p;

    /* Trivial ASCII grayscale display. */
    p0 = frame->data[0];
    printf("\033[H"); // Move cursor to top-left (1;1)
    for (y = 0; y < frame->height; y++) {
        p = p0;
        for (x = 0; x < frame->width; x++)
            putchar(" .-+#"[*(p++) / 52]); // Use 5 shades of gray (0-51, 52-103, etc.)
        putchar('\n');
        p0 += frame->linesize[0];
    }
    fflush(stdout); // Ensure the output is immediately displayed
}

int main(int argc, char **argv)
{
    int ret;
    AVPacket *packet;
    AVFrame *frame;
    AVFrame *filt_frame;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        exit(1);
    }

    // Optional: Set FFmpeg log level. AV_LOG_INFO will show the filter config.
    // av_log_set_level(AV_LOG_QUIET); // Uncomment to silence all FFmpeg logs

    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !filt_frame || !packet) {
        fprintf(stderr, "Could not allocate frame or packet\n");
        exit(1);
    }

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;

    // Call init_filters with the detected input dimensions
    if ((ret = init_filters(dec_ctx->width, dec_ctx->height)) < 0)
        goto end;

    // Process and display only the first video frame
    int frame_displayed = 0;
    while (!frame_displayed) {
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0) {
            if (ret != AVERROR_EOF) {
                av_log(NULL, AV_LOG_ERROR, "Error reading frame from input: %s\n", av_err2str(ret));
            }
            break; // Exit loop if no more packets or read error
        }

        if (packet->stream_index == video_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder: %s\n", av_err2str(ret));
                // If it's not a temporary error (EAGAIN/EOF), break to avoid infinite loop
                if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                    break;
                }
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    // Need more packets or no more frames from decoder
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder: %s\n", av_err2str(ret));
                    goto end; // Critical error, exit program
                }

                frame->pts = frame->best_effort_timestamp;

                // Push the decoded frame into the filtergraph
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph: %s\n", av_err2str(ret));
                    break;
                }

                // Pull filtered frames from the filtergraph
                while (1) {
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        // Need more frames from filtergraph or no more
                        break;
                    }
                    if (ret < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Error while pulling from filtergraph: %s\n", av_err2str(ret));
                        goto end; // Critical error, exit program
                    }
                    display_frame(filt_frame, buffersink_ctx->inputs[0]->time_base);
                    av_frame_unref(filt_frame);
                    //frame_displayed = 1; // Mark that a frame has been displayed
                    break; // Exit inner loop after displaying one filtered frame
                }
                av_frame_unref(frame);
                if (frame_displayed) {
                    break; // Exit outer loop if a frame was displayed
                }
            }
        }
        av_packet_unref(packet);
        if (frame_displayed) {
            break; // Exit main loop if a frame was displayed
        }
    }

end:
    // Free all allocated FFmpeg structures
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    // Report final status
    if (ret < 0 && ret != AVERROR_EOF && !frame_displayed) {
        fprintf(stderr, "Program finished with an error: %s\n", av_err2str(ret));
        exit(1);
    } else if (!frame_displayed && ret == AVERROR_EOF) {
        fprintf(stderr, "End of file reached, but no video frame could be displayed.\n");
        exit(1);
    }

    exit(0);
}
