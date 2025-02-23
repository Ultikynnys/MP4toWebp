#include <iostream>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <cctype>

namespace fs = std::filesystem;

extern "C" {
    // FFmpeg headers
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <webp/encode.h>
#include <webp/mux.h>
#include <webp/demux.h>
#include <webp/decode.h>

// Uncomment or define this macro to re-enable clearing the console.
// When set to 0, the console remains visible.
#define ENABLE_CLEAR_CONSOLE 0

// Utility: clear the console screen
void clearConsole() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

// Print progress bar using time-based progress.
void print_progress_bar(double progress, int currentOutputFrame, int expectedOutputFrames,
    const std::string& inputFile, const std::string& outputFile,
    int quality, uintmax_t fileSize) {
    const int barWidth = 50;
    int pos = static_cast<int>(barWidth * progress);

#if ENABLE_CLEAR_CONSOLE
    clearConsole();
#endif

    std::cout << "[";
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos)
            std::cout << "=";
        else if (i == pos)
            std::cout << ">";
        else
            std::cout << " ";
    }
    std::cout << "] " << static_cast<int>(progress * 100.0) << "%  ";
    std::cout << "Output Frame: " << currentOutputFrame << " / " << expectedOutputFrames << "\n";
    std::cout << "Filesize: " << (static_cast<double>(fileSize) / (1024.0 * 1024.0)) << " MB\n";
    std::cout << "Input: " << inputFile << "\nOutput: " << outputFile;
    std::cout << "\nQuality: " << quality << "\n";
    std::cout.flush();
}

// Print help message.
void print_help(const char* progName) {
    std::cout << "Usage: " << progName << " <input.mp4|input.mkv|input.webp> [output.webp] [options]\n\n";
    std::cout << "Description:\n";
    std::cout << "  Converts an input video file (MP4, MKV or animated WebP) into an animated WebP file.\n";
    std::cout << "  If only an input file is provided, the output file is generated with a '.webp' extension.\n\n";
    std::cout << "Basic Options (optional, defaults in parentheses):\n";
    std::cout << "  -q, --quality <value>       Set quality (0-100, default 80).\n";
    std::cout << "  -l, --lossless              Enable lossless encoding (default is lossy).\n";
    std::cout << "  -f, --framerate <value>     Set output framerate in FPS (default 30).\n";
    std::cout << "  --thread_level <value>      Set extra thread level for WebP encoder (default 0).\n";
    std::cout << "  --method <value>            Set quality/speed trade-off for WebP (0=fast, 6=slower-better, default 0).\n";
    std::cout << "  --target_size <value>       Set target file size in MB (converted internally to bytes, default 0).\n";
    std::cout << "  --scale <value>             Set scale factor for input video (default 1.0).\n\n";
    std::cout << "Example Usage:\n";
    std::cout << "  " << progName << " sample.mkv output.webp -q 75 -l -f 24 --method 4 --target_size 2 --scale 0.5\n";
}

// Structures for frame data.
struct FrameData {
    int timestamp;              // Output timestamp in ms.
    std::vector<uint8_t> rgba;  // RGBA pixel data.
    int id;                     // Unique id for the original frame.
};

struct ConvertedFrame {
    int origTimestamp;
    std::vector<uint8_t> rgba;
    int id;                     // Unique id assigned to the original frame.
};

// Global atomic counter for original frame IDs.
std::atomic<int> originalFrameCounter{ 0 };

// ----- Thread Pool Implementation (used in the FFmpeg branch) -----
class ThreadPool {
public:
    ThreadPool(size_t numThreads);
    ~ThreadPool();

    void enqueue(std::function<void()> task);
    void wait();

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    std::mutex wait_mutex;
    std::condition_variable wait_condition;
    std::atomic<int> tasks_in_progress;
};

ThreadPool::ThreadPool(size_t numThreads)
    : stop(false), tasks_in_progress(0) {
    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this]() {
            thread_local SwsContext* local_sws_ctx = nullptr;
            thread_local std::vector<uint8_t> local_buffer;
            thread_local AVFrame* local_rgba = nullptr;
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this]() { return this->stop || !this->tasks.empty(); });
                    if (this->stop && this->tasks.empty())
                        return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                    tasks_in_progress++;
                }
                task();
                tasks_in_progress--;
                wait_condition.notify_all();
            }
            });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        worker.join();
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        tasks.push(std::move(task));
    }
    condition.notify_one();
}

void ThreadPool::wait() {
    std::unique_lock<std::mutex> lock(wait_mutex);
    wait_condition.wait(lock, [this]() {
        std::unique_lock<std::mutex> lock2(queue_mutex);
        return tasks.empty() && (tasks_in_progress.load() == 0);
        });
}

// ----- End Thread Pool Implementation -----

// Helper function to reset a WebPPicture.
bool ResetPicture(WebPPicture* picture) {
    WebPPictureFree(picture);
    return WebPPictureInit(picture);
}

int main(int argc, char* argv[]) {
    // No explicit DLL loading needed.

    if (argc < 2) {
        std::cerr << "Error: Insufficient arguments.\n";
        print_help(argv[0]);
        return -1;
    }
    std::string inputFilename = argv[1];
    std::string outputFilename;
    if (argc > 2 && argv[2][0] != '-') {
        outputFilename = argv[2];
    }
    else {
        size_t dotPos = inputFilename.find_last_of(".");
        outputFilename = (dotPos != std::string::npos) ? inputFilename.substr(0, dotPos) + ".webp"
            : inputFilename + ".webp";
    }

    // Default options.
    int quality = 80;
    bool lossless = false;
    int outputFramerate = 15;
    int extra_thread_level = -1;
    int method = 0;           // quality/speed trade-off.
    int target_size = 4*1024*1024;      // in bytes.
    double scale = 1.0;       // scale factor (default 1.0).

    int optionIndex = (argc > 2 && argv[2][0] != '-') ? 3 : 2;
    for (int i = optionIndex; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quality") == 0) {
            if (i + 1 < argc) { quality = std::atoi(argv[++i]); }
            else { std::cerr << "Error: Missing value for quality.\n"; return -1; }
        }
        else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--lossless") == 0) {
            lossless = true;
        }
        else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--framerate") == 0) {
            if (i + 1 < argc) {
                outputFramerate = std::atoi(argv[++i]);
                if (outputFramerate <= 0) { std::cerr << "Error: Framerate must be positive.\n"; return -1; }
            }
            else { std::cerr << "Error: Missing value for framerate.\n"; return -1; }
        }
        else if (strcmp(argv[i], "--thread_level") == 0) {
            if (i + 1 < argc) { extra_thread_level = std::atoi(argv[++i]); }
            else { std::cerr << "Error: Missing value for --thread_level.\n"; return -1; }
        }
        else if (strcmp(argv[i], "--method") == 0) {
            if (i + 1 < argc) { method = std::atoi(argv[++i]); }
            else { std::cerr << "Error: Missing value for --method.\n"; return -1; }
        }
        else if (strcmp(argv[i], "--target_size") == 0) {
            if (i + 1 < argc) {
                int mb = std::atoi(argv[++i]);
                target_size = mb * 1024 * 1024;
            }
            else { std::cerr << "Error: Missing value for --target_size.\n"; return -1; }
        }
        else if (strcmp(argv[i], "--scale") == 0) {
            if (i + 1 < argc) { scale = std::atof(argv[++i]); }
            else { std::cerr << "Error: Missing value for --scale.\n"; return -1; }
        }
    }

    // Determine if input is a WebP file (by extension).
    std::string lowerInput = inputFilename;
    std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(), ::tolower);
    bool inputIsWebP = (lowerInput.size() >= 5 && lowerInput.substr(lowerInput.size() - 5) == ".webp");

    std::vector<ConvertedFrame> convertedFrames;
    int total_duration_ms = 0;
    double inputFPS = 0.0;
    int origWidth = 0, origHeight = 0, scaledWidth = 0, scaledHeight = 0;

    if (inputIsWebP) {
        // --- WebP-to-WebP Conversion Branch ---
        std::ifstream fileStream(inputFilename, std::ios::binary);
        if (!fileStream) {
            std::cerr << "Error: Could not open input file.\n";
            return -1;
        }
        std::vector<uint8_t> fileBuffer((std::istreambuf_iterator<char>(fileStream)),
            std::istreambuf_iterator<char>());
        fileStream.close();

        WebPData webpData;
        WebPDataInit(&webpData);
        webpData.bytes = fileBuffer.data();
        webpData.size = fileBuffer.size();

        WebPAnimDecoderOptions dec_options;
        if (!WebPAnimDecoderOptionsInit(&dec_options)) {
            std::cerr << "Error initializing WebPAnimDecoderOptions.\n";
            return -1;
        }
        WebPAnimDecoder* decoder = WebPAnimDecoderNew(&webpData, &dec_options);
        if (!decoder) {
            std::cerr << "Error: Could not create WebP animation decoder.\n";
            return -1;
        }

        WebPAnimInfo anim_info;
        if (!WebPAnimDecoderGetInfo(decoder, &anim_info)) {
            std::cerr << "Error: Could not get WebP animation info.\n";
            WebPAnimDecoderDelete(decoder);
            return -1;
        }
        origWidth = anim_info.canvas_width;
        origHeight = anim_info.canvas_height;
        int frameCount = anim_info.frame_count;  // No total_duration member

        scaledWidth = static_cast<int>(origWidth * scale);
        scaledHeight = static_cast<int>(origHeight * scale);

        // Create a scaling context if needed.
        SwsContext* sws_ctx = nullptr;
        if (scale != 1.0) {
            sws_ctx = sws_getContext(
                origWidth, origHeight, AV_PIX_FMT_RGBA,
                scaledWidth, scaledHeight, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );
            if (!sws_ctx) {
                std::cerr << "Error: Could not create scaling context.\n";
                WebPAnimDecoderDelete(decoder);
                return -1;
            }
        }

        // Decode all frames.
        while (WebPAnimDecoderHasMoreFrames(decoder)) {
            uint8_t* buf;
            int timestamp;
            if (!WebPAnimDecoderGetNext(decoder, &buf, &timestamp)) {
                break;
            }
            total_duration_ms = timestamp;  // Update duration with current frame's timestamp
            int frameId = originalFrameCounter.fetch_add(1);
            std::vector<uint8_t> rgbaData;
            if (scale == 1.0) {
                rgbaData.assign(buf, buf + (origWidth * origHeight * 4));
            }
            else {
                int dstBufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, scaledWidth, scaledHeight, 1);
                std::vector<uint8_t> dstBuffer(dstBufferSize);
                AVFrame* dstFrame = av_frame_alloc();
                if (!dstFrame) {
                    std::cerr << "Error: Could not allocate destination frame.\n";
                    continue;
                }
                av_image_fill_arrays(dstFrame->data, dstFrame->linesize, dstBuffer.data(),
                    AV_PIX_FMT_RGBA, scaledWidth, scaledHeight, 1);
                // Use const_cast to match sws_scale signature.
                uint8_t* srcSlice[1] = { const_cast<uint8_t*>(buf) };
                int srcStride[1] = { origWidth * 4 };
                sws_scale(sws_ctx, srcSlice, srcStride, 0, origHeight, dstFrame->data, dstFrame->linesize);
                rgbaData = std::move(dstBuffer);
                av_frame_free(&dstFrame);
            }
            convertedFrames.push_back(ConvertedFrame{ timestamp, std::move(rgbaData), frameId });
        }
        WebPAnimDecoderDelete(decoder);
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
        }

        // Calculate inputFPS using the last frame's timestamp.
        if (total_duration_ms > 0 && !convertedFrames.empty()) {
            inputFPS = static_cast<double>(convertedFrames.size()) / (total_duration_ms / 1000.0);
        }
    }
    else {
        // --- FFmpeg Branch for Video Input (MP4/MKV) ---
        AVFormatContext* fmt_ctx = nullptr;
        if (avformat_open_input(&fmt_ctx, inputFilename.c_str(), nullptr, nullptr) < 0) {
            std::cerr << "Error opening input file.\n";
            return -1;
        }
        if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
            std::cerr << "Error finding stream info.\n";
            return -1;
        }
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
        AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            std::cerr << "Unsupported codec.\n";
            return -1;
        }
        AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cerr << "Could not allocate codec context.\n";
            return -1;
        }
        codec_ctx->thread_count = std::thread::hardware_concurrency();
        codec_ctx->thread_type = FF_THREAD_FRAME;
        if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
            std::cerr << "Could not copy codec parameters.\n";
            return -1;
        }
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Could not open codec.\n";
            return -1;
        }
        origWidth = codec_ctx->width;
        origHeight = codec_ctx->height;
        scaledWidth = static_cast<int>(origWidth * scale);
        scaledHeight = static_cast<int>(origHeight * scale);
        SwsContext* sws_ctx_main = sws_getContext(
            origWidth, origHeight, codec_ctx->pix_fmt,
            scaledWidth, scaledHeight, AV_PIX_FMT_RGBA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!sws_ctx_main) {
            std::cerr << "Could not initialize the conversion context.\n";
            return -1;
        }
        total_duration_ms = (fmt_ctx->duration > 0) ? fmt_ctx->duration / 1000 : 0;
        inputFPS = av_q2d(fmt_ctx->streams[video_stream_index]->r_frame_rate);
        uintmax_t fileSize = fs::file_size(inputFilename);

        std::cout << "DEBUG: Input FPS: " << inputFPS << "\n";
        std::cout << "DEBUG: Output FPS: " << outputFramerate << "\n";
        double frameInterval = 1000.0 / outputFramerate;
        std::cout << "DEBUG: Frame Interval (ms): " << frameInterval << "\n";

        std::mutex framesMutex;
        ThreadPool pool(std::thread::hardware_concurrency());
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!packet || !frame) {
            std::cerr << "Could not allocate packet or frame.\n";
            return -1;
        }

        auto process_frame = [&, codec_ctx, origWidth, origHeight, scaledWidth, scaledHeight](AVFrame* localFrame, int64_t pts) {
            int origTimestamp = static_cast<int>(pts * av_q2d(fmt_ctx->streams[video_stream_index]->time_base) * 1000);
            int frameId = originalFrameCounter.fetch_add(1);
            thread_local SwsContext* local_sws_ctx = nullptr;
            thread_local std::vector<uint8_t> local_buffer;
            thread_local AVFrame* local_rgba = nullptr;
            if (!local_sws_ctx) {
                local_sws_ctx = sws_getContext(
                    origWidth, origHeight, codec_ctx->pix_fmt,
                    scaledWidth, scaledHeight, AV_PIX_FMT_RGBA,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                );
                if (!local_sws_ctx) {
                    std::cerr << "Could not create local sws context in thread.\n";
                    av_frame_free(&localFrame);
                    return;
                }
            }
            int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, scaledWidth, scaledHeight, 1);
            if (local_buffer.size() != static_cast<size_t>(num_bytes)) {
                local_buffer.resize(num_bytes);
            }
            if (!local_rgba) {
                local_rgba = av_frame_alloc();
                if (!local_rgba) {
                    std::cerr << "Could not allocate local RGBA frame.\n";
                    av_frame_free(&localFrame);
                    return;
                }
            }
            av_image_fill_arrays(local_rgba->data, local_rgba->linesize, local_buffer.data(),
                AV_PIX_FMT_RGBA, scaledWidth, scaledHeight, 1);
            sws_scale(local_sws_ctx,
                localFrame->data,
                localFrame->linesize,
                0,
                origHeight,
                local_rgba->data,
                local_rgba->linesize);
            std::vector<uint8_t> rgbaVec(local_buffer.begin(), local_buffer.end());
            {
                std::lock_guard<std::mutex> lock(framesMutex);
                convertedFrames.push_back(ConvertedFrame{ origTimestamp, std::move(rgbaVec), frameId });
            }
            av_frame_free(&localFrame);
            };

        while (av_read_frame(fmt_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream_index) {
                if (avcodec_send_packet(codec_ctx, packet) == 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        int64_t pts = (frame->pts == AV_NOPTS_VALUE) ? 0 : frame->pts;
                        int current_time_ms = static_cast<int>(pts * av_q2d(fmt_ctx->streams[video_stream_index]->time_base) * 1000);
                        int currentOutputFrame = (1000 / outputFramerate > 0) ? (current_time_ms / (1000 / outputFramerate)) + 1 : 0;
                        double progress = (total_duration_ms > 0) ? static_cast<double>(current_time_ms) / total_duration_ms : 0.0;
                        if (progress > 1.0)
                            progress = 1.0;
                        print_progress_bar(progress, currentOutputFrame, (total_duration_ms > 0) ? (total_duration_ms / (1000 / outputFramerate)) + 1 : 0,
                            inputFilename, outputFilename, quality, fileSize);
                        AVFrame* frame_copy = av_frame_clone(frame);
                        pool.enqueue([frame_copy, pts, process_frame]() {
                            process_frame(frame_copy, pts);
                            });
                    }
                }
            }
            av_packet_unref(packet);
        }
        if (avcodec_send_packet(codec_ctx, nullptr) == 0) {
            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                int64_t pts = (frame->pts == AV_NOPTS_VALUE) ? 0 : frame->pts;
                int current_time_ms = static_cast<int>(pts * av_q2d(fmt_ctx->streams[video_stream_index]->time_base) * 1000);
                int currentOutputFrame = (1000 / outputFramerate > 0) ? (current_time_ms / (1000 / outputFramerate)) + 1 : 0;
                double progress = (total_duration_ms > 0) ? static_cast<double>(current_time_ms) / total_duration_ms : 0.0;
                if (progress > 1.0)
                    progress = 1.0;
                print_progress_bar(progress, currentOutputFrame, (total_duration_ms > 0) ? (total_duration_ms / (1000 / outputFramerate)) + 1 : 0,
                    inputFilename, outputFilename, quality, fileSize);
                AVFrame* frame_copy = av_frame_clone(frame);
                pool.enqueue([frame_copy, pts, process_frame]() {
                    process_frame(frame_copy, pts);
                    });
            }
        }
        pool.wait();
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
    }

    // ----- Optimized Sampling Step with Correct Frame Timing -----
    std::sort(convertedFrames.begin(), convertedFrames.end(), [](const ConvertedFrame& a, const ConvertedFrame& b) {
        return a.origTimestamp < b.origTimestamp;
        });

    std::vector<FrameData> framesData;
    if (inputFPS < outputFramerate) {
        size_t totalFrames = convertedFrames.size();
        for (size_t i = 0; i < totalFrames; i++) {
            int outTimestamp = (totalFrames > 1) ? static_cast<int>(std::round(i * total_duration_ms / static_cast<double>(totalFrames - 1))) : 0;
            framesData.push_back(FrameData{ outTimestamp, std::move(convertedFrames[i].rgba), convertedFrames[i].id });
        }
    }
    else {
        double frameInterval = 1000.0 / outputFramerate;
        double nextFrameTime = 0.0;
        int outputFrameCount = 0;
        for (auto& cf : convertedFrames) {
            if (cf.origTimestamp >= nextFrameTime) {
                int outTimestamp = static_cast<int>(std::round(outputFrameCount * frameInterval));
                framesData.push_back(FrameData{ outTimestamp, std::move(cf.rgba), cf.id });
                outputFrameCount++;
                nextFrameTime += frameInterval;
            }
        }
    }

    // ----- Set Up WebP Animation Encoder and Write Output -----
    WebPAnimEncoderOptions enc_options;
    if (!WebPAnimEncoderOptionsInit(&enc_options)) {
        std::cerr << "Could not initialize WebP animation encoder options.\n";
        return -1;
    }
    WebPAnimEncoder* encoder = WebPAnimEncoderNew(scaledWidth, scaledHeight, &enc_options);
    if (!encoder) {
        std::cerr << "Could not create WebP animation encoder.\n";
        return -1;
    }
    WebPConfig config;
    if (!WebPConfigInit(&config)) {
        std::cerr << "Could not initialize WebP config.\n";
        return -1;
    }
    config.quality = quality;
    config.lossless = lossless ? 1 : 0;
    config.method = method;
    config.target_size = target_size;
    config.thread_level = (extra_thread_level >= 0) ? extra_thread_level : 0;
    if (!WebPValidateConfig(&config)) {
        std::cerr << "Invalid WebP config. Please try a different set of options.\n";
        return -1;
    }

    int expectedOutputFrames = (total_duration_ms > 0) ? (total_duration_ms / (1000 / outputFramerate)) + 1 : framesData.size();
    int totalOutputFramesFinal = framesData.size();
    int frameIndex = 0;
    WebPPicture picture;
    if (!WebPPictureInit(&picture)) {
        std::cerr << "Could not initialize WebP picture.\n";
        return -1;
    }
    picture.width = scaledWidth;
    picture.height = scaledHeight;
    picture.use_argb = 1;

    for (const auto& fd : framesData) {
        std::cout << "Adding frame " << (frameIndex + 1) << " of " << totalOutputFramesFinal
            << " at " << fd.timestamp << " ms, id: " << fd.id << "...\n";
        size_t expectedSize = static_cast<size_t>(scaledWidth * scaledHeight * 4);
        if (fd.rgba.size() != expectedSize) {
            std::cerr << "Warning: Expected buffer size " << expectedSize
                << " but got " << fd.rgba.size() << ".\n";
        }
        if (!ResetPicture(&picture)) {
            std::cerr << "Could not reset WebP picture for frame " << frameIndex << ".\n";
            frameIndex++;
            continue;
        }
        picture.width = scaledWidth;
        picture.height = scaledHeight;
        picture.use_argb = 1;
        if (!WebPPictureImportRGBA(&picture, fd.rgba.data(), scaledWidth * 4)) {
            std::cerr << "Failed to import RGBA data for frame at timestamp " << fd.timestamp << " ms, id: " << fd.id << ".\n";
            frameIndex++;
            continue;
        }
        if (!WebPAnimEncoderAdd(encoder, &picture, fd.timestamp, &config)) {
            std::cerr << "Failed to add a frame at timestamp " << fd.timestamp << " ms, id: " << fd.id << ".\n";
        }
        frameIndex++;
    }
    WebPPictureFree(&picture);

    WebPData webp_data;
    WebPDataInit(&webp_data);
    if (!WebPAnimEncoderAssemble(encoder, &webp_data)) {
        std::cerr << "\nFailed to assemble WebP animation.\n";
        return -1;
    }

    FILE* out_file = nullptr;
    errno_t err = fopen_s(&out_file, outputFilename.c_str(), "wb");
    if (err != 0 || out_file == nullptr) {
        std::cerr << "Could not open output file for writing.\n";
        return -1;
    }
    std::fwrite(webp_data.bytes, webp_data.size, 1, out_file);
    std::fclose(out_file);
    std::cout << "\nConversion complete. Output saved to " << outputFilename << "\n";

    WebPAnimEncoderDelete(encoder);
    WebPDataClear(&webp_data);
    return 0;
}
