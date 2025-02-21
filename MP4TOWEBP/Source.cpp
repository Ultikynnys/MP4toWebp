#include <iostream>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" {
    // FFmpeg headers
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <webp/encode.h>
#include <webp/anim_encode.h>
#include <webp/mux.h>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input.mp4> <output.webp>\n";
        return -1;
    }

    const char* input_filename = argv[1];
    const char* output_filename = argv[2];

    // Initialize FFmpeg
    av_register_all();
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_filename, nullptr, nullptr) < 0) {
        std::cerr << "Error opening input file.\n";
        return -1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Error finding stream info.\n";
        return -1;
    }

    // Find the first video stream
    int video_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }
    if (video_stream_index == -1) {
        std::cerr << "No video stream found.\n";
        return -1;
    }

    // Open the codec
    AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
    AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "Unsupported codec.\n";
        return -1;
    }
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Could not allocate codec context.\n";
        return -1;
    }
    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        std::cerr << "Could not copy codec parameters.\n";
        return -1;
    }
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec.\n";
        return -1;
    }

    // Prepare to read frames
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();

    // Set up a conversion context to convert the frame to RGBA
    SwsContext* sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx) {
        std::cerr << "Could not initialize the conversion context.\n";
        return -1;
    }

    // Allocate an AVFrame for the RGBA data
    AVFrame* frame_rgba = av_frame_alloc();
    if (!frame_rgba) {
        std::cerr << "Could not allocate RGBA frame.\n";
        return -1;
    }
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, codec_ctx->width, codec_ctx->height, 1);
    uint8_t* rgba_buffer = (uint8_t*)av_malloc(num_bytes * sizeof(uint8_t));
    if (!rgba_buffer) {
        std::cerr << "Could not allocate buffer.\n";
        return -1;
    }
    av_image_fill_arrays(frame_rgba->data, frame_rgba->linesize, rgba_buffer, AV_PIX_FMT_RGBA, codec_ctx->width, codec_ctx->height, 1);

    // Initialize WebP animation encoder options and encoder
    WebPAnimEncoderOptions enc_options;
    if (!WebPAnimEncoderOptionsInit(&enc_options)) {
        std::cerr << "Could not initialize WebP animation encoder options.\n";
        return -1;
    }
    WebPAnimEncoder* encoder = WebPAnimEncoderNew(codec_ctx->width, codec_ctx->height, &enc_options);
    if (!encoder) {
        std::cerr << "Could not create WebP animation encoder.\n";
        return -1;
    }

    // Initialize WebP configuration (quality, etc.)
    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        std::cerr << "Could not initialize WebP config.\n";
        return -1;
    }
    // Optionally, adjust the configuration (e.g., config.quality = 75;)

    // Calculate the time base in milliseconds for frame timestamps
    double time_base = av_q2d(fmt_ctx->streams[video_stream_index]->time_base);

    // Read frames from the video
    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            if (avcodec_send_packet(codec_ctx, packet) == 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    // Convert the frame to RGBA
                    sws_scale(
                        sws_ctx,
                        frame->data,
                        frame->linesize,
                        0,
                        codec_ctx->height,
                        frame_rgba->data,
                        frame_rgba->linesize
                    );

                    // Calculate timestamp in milliseconds
                    int64_t pts = (frame->pts == AV_NOPTS_VALUE) ? 0 : frame->pts;
                    int timestamp = static_cast<int>(pts * time_base * 1000);

                    // Add the RGBA frame to the WebP animation encoder
                    if (!WebPAnimEncoderAdd(encoder, frame_rgba->data[0], frame_rgba->linesize[0], timestamp, &config)) {
                        std::cerr << "Failed to add a frame to the WebP encoder.\n";
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Assemble the animated WebP
    WebPData webp_data;
    WebPDataInit(&webp_data);
    if (!WebPAnimEncoderAssemble(encoder, &webp_data)) {
        std::cerr << "Failed to assemble WebP animation.\n";
        return -1;
    }

    // Write the WebP data to the output file
    FILE* out_file = std::fopen(output_filename, "wb");
    if (!out_file) {
        std::cerr << "Could not open output file for writing.\n";
        return -1;
    }
    std::fwrite(webp_data.bytes, webp_data.size, 1, out_file);
    std::fclose(out_file);
    std::cout << "Conversion complete. Output saved to " << output_filename << "\n";

    // Clean up resources
    WebPAnimEncoderDelete(encoder);
    WebPDataClear(&webp_data);
    av_free(rgba_buffer);
    av_frame_free(&frame);
    av_frame_free(&frame_rgba);
    av_packet_free(&packet);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    sws_freeContext(sws_ctx);

    return 0;
}
