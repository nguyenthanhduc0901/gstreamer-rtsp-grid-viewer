#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Simple thread-safe logger writing to per-stream files under ./logs
class Logger {
public:
    explicit Logger(const std::string& filename)
        : filePath_(filename) {
        // Ensure directory exists (best-effort)
        CreateDirectoryW(L"logs", nullptr);
    }

    void log(const std::string& level, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mu_);
        std::ofstream ofs(filePath_, std::ios::app);
        ofs << timestamp() << " [" << level << "] " << msg << "\n";
    }

private:
    std::string timestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto t = system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms.count();
        return oss.str();
    }

    std::mutex mu_;
    std::string filePath_;
};

static bool pad_has_media_video(GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    bool is_video = false;
    if (caps) {
        GstStructure const* st = gst_caps_get_structure(caps, 0);
        const gchar* name = gst_structure_get_name(st);
        // Accept RTP or raw video
        if (g_str_has_prefix(name, "application/x-rtp")) {
            const gchar* media = gst_structure_get_string(st, "media");
            if (media && g_strcmp0(media, "video") == 0) is_video = true;
        } else if (g_str_has_prefix(name, "video/")) {
            is_video = true;
        } else if (g_str_has_prefix(name, "video/x-raw")) {
            is_video = true;
        }
        gst_caps_unref(caps);
    }
    return is_video;
}

struct StreamPipeline {
    std::string name;
    std::string url;
    HWND targetHwnd { nullptr };

    GstElement* pipeline { nullptr };
    GstElement* rtspsrc { nullptr };
    GstElement* decodebin { nullptr };
    GstElement* queue { nullptr };
    GstElement* convert { nullptr };
    GstElement* sink { nullptr };
    GstBus* bus { nullptr };

    std::atomic<bool> running { false };
    std::atomic<bool> rebuilding { false };
    std::thread busThread;

    Logger logger;

    int backoff_ms { 2000 };

    StreamPipeline(const std::string& nm, const std::string& u, HWND h)
        : name(nm), url(u), targetHwnd(h), logger(std::string("logs/") + nm + ".log") {}
};

static void set_overlay_handle(GstElement* sink, HWND hwnd) {
    if (!sink || !hwnd) return;
    if (GST_IS_VIDEO_OVERLAY(sink)) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), (guintptr)hwnd);
        gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
    }
}

static void link_decodebin_to_chain(StreamPipeline* sp, GstPad* pad) {
    if (!pad_has_media_video(pad)) return;

    GstPad* sinkpad = gst_element_get_static_pad(sp->queue, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        sp->logger.log("ERROR", "Failed to link decodebin src to queue: " + std::to_string(ret));
    } else {
        sp->logger.log("INFO", "Linked decodebin -> queue");
    }
    gst_object_unref(sinkpad);
}

static void on_decodebin_pad_added(GstElement* decodebin, GstPad* pad, gpointer user_data) {
    auto* sp = reinterpret_cast<StreamPipeline*>(user_data);
    link_decodebin_to_chain(sp, pad);
}

static void on_rtspsrc_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    auto* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!pad_has_media_video(pad)) return;

    GstPad* sinkpad = gst_element_get_static_pad(sp->decodebin, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        sp->logger.log("ERROR", "Failed to link rtspsrc -> decodebin: " + std::to_string(ret));
    } else {
        sp->logger.log("INFO", "Linked rtspsrc -> decodebin");
    }
    gst_object_unref(sinkpad);
}

static GstElement* try_make_sink(Logger& logger) {
    GstElement* sink = gst_element_factory_make("d3dvideosink", nullptr);
    if (sink) return sink;
    logger.log("WARN", "d3dvideosink not available, trying glimagesink");
    sink = gst_element_factory_make("glimagesink", nullptr);
    if (sink) return sink;
    logger.log("WARN", "glimagesink not available, falling back to autovideosink");
    return gst_element_factory_make("autovideosink", nullptr);
}

static bool build_pipeline(StreamPipeline* sp) {
    // Create elements
    sp->pipeline   = gst_pipeline_new(sp->name.c_str());
    sp->rtspsrc    = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());
    sp->decodebin  = gst_element_factory_make("decodebin", (sp->name + "_dec").c_str());
    sp->queue      = gst_element_factory_make("queue", (sp->name + "_q").c_str());
    sp->convert    = gst_element_factory_make("videoconvert", (sp->name + "_conv").c_str());
    sp->sink       = try_make_sink(sp->logger);

    if (!sp->pipeline || !sp->rtspsrc || !sp->decodebin || !sp->queue || !sp->convert || !sp->sink) {
        sp->logger.log("ERROR", "Failed to create one or more GStreamer elements");
        return false;
    }

    // Configure elements
    g_object_set(G_OBJECT(sp->rtspsrc), "location", sp->url.c_str(), nullptr);
    g_object_set(G_OBJECT(sp->rtspsrc), "latency", 0, nullptr); // low latency per user request

    // Improve visuals
    g_object_set(G_OBJECT(sp->sink), "force-aspect-ratio", TRUE, "sync", TRUE, nullptr);
    // For slightly lower latency you could set sync=false, but keep sync for smoother playback
    // g_object_set(G_OBJECT(sp->sink), "sync", FALSE, nullptr);

    // Assemble pipeline
    gst_bin_add_many(GST_BIN(sp->pipeline), sp->rtspsrc, sp->decodebin, sp->queue, sp->convert, sp->sink, nullptr);

    // Link static parts: decodebin->queue->convert->sink (rtspsrc and decodebin need pad-added)
    if (!gst_element_link_many(sp->queue, sp->convert, sp->sink, nullptr)) {
        sp->logger.log("ERROR", "Failed to link queue->videoconvert->sink");
        return false;
    }

    // Signals
    g_signal_connect(sp->rtspsrc, "pad-added", G_CALLBACK(on_rtspsrc_pad_added), sp);
    g_signal_connect(sp->decodebin, "pad-added", G_CALLBACK(on_decodebin_pad_added), sp);

    // Link rtspsrc to decodebin dynamically in callback; also need to link rtspsrc->decodebin once pad appears

    // Overlay window
    set_overlay_handle(sp->sink, sp->targetHwnd);

    // Bus
    sp->bus = gst_element_get_bus(sp->pipeline);

    return true;
}

static void teardown_pipeline(StreamPipeline* sp) {
    if (!sp->pipeline) return;

    gst_element_set_state(sp->pipeline, GST_STATE_NULL);
    if (sp->bus) { gst_object_unref(sp->bus); sp->bus = nullptr; }

    if (sp->pipeline) {
        gst_object_unref(sp->pipeline);
        sp->pipeline = nullptr;
        sp->rtspsrc = sp->decodebin = sp->queue = sp->convert = sp->sink = nullptr;
    }
}

static bool start_pipeline(StreamPipeline* sp) {
    if (!build_pipeline(sp)) return false;
    GstStateChangeReturn ret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        sp->logger.log("ERROR", "Failed to set pipeline to PLAYING");
        teardown_pipeline(sp);
        return false;
    }
    sp->running = true;
    return true;
}

static void bus_loop(StreamPipeline* sp) {
    sp->logger.log("INFO", "Bus loop started");
    while (sp->running) {
        if (!sp->bus) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        GstMessage* msg = gst_bus_timed_pop_filtered(
            sp->bus, 200 * GST_MSECOND,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING | GST_MESSAGE_STATE_CHANGED));
        if (!msg) continue;

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr; gchar* dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            std::ostringstream oss; oss << "Warning: " << (err ? err->message : "")
                                        << (dbg ? std::string(" | ") + dbg : "");
            sp->logger.log("WARN", oss.str());
            if (err) g_error_free(err); if (dbg) g_free(dbg);
            break;
        }
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::ostringstream oss; oss << "Error: " << (err ? err->message : "")
                                        << (dbg ? std::string(" | ") + dbg : "");
            sp->logger.log("ERROR", oss.str());
            if (err) g_error_free(err); if (dbg) g_free(dbg);
            // Trigger reconnect
            sp->rebuilding = true;
            teardown_pipeline(sp);
            std::this_thread::sleep_for(std::chrono::milliseconds(sp->backoff_ms));
            sp->backoff_ms = std::min(sp->backoff_ms * 2, 30000); // exponential backoff up to 30s
            if (!start_pipeline(sp)) {
                sp->logger.log("ERROR", "Reconnect attempt failed, will retry");
            } else {
                sp->logger.log("INFO", "Reconnected successfully");
                sp->backoff_ms = 2000; // reset on success
            }
            sp->rebuilding = false;
            break;
        }
        case GST_MESSAGE_EOS: {
            sp->logger.log("INFO", "EOS received; reconnecting");
            sp->rebuilding = true;
            teardown_pipeline(sp);
            std::this_thread::sleep_for(std::chrono::milliseconds(sp->backoff_ms));
            if (!start_pipeline(sp)) {
                sp->logger.log("ERROR", "Reconnect attempt after EOS failed");
            } else {
                sp->logger.log("INFO", "Reconnected after EOS");
                sp->backoff_ms = 2000;
            }
            sp->rebuilding = false;
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(sp->pipeline)) {
                GstState old_s, new_s, pending_s;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending_s);
                std::ostringstream oss; oss << "Pipeline state: "
                    << gst_element_state_get_name(old_s) << " -> " << gst_element_state_get_name(new_s);
                sp->logger.log("INFO", oss.str());
            }
            break;
        }
        default: break;
        }
        gst_message_unref(msg);
    }
    sp->logger.log("INFO", "Bus loop ended");
}

// ------------------- Win32 UI -------------------

struct AppContext {
    HINSTANCE hInst { nullptr };
    HWND mainHwnd { nullptr };
    HWND cells[4] { nullptr, nullptr, nullptr, nullptr };
    std::vector<StreamPipeline*> streams;
};

static void layout_cells(HWND parent, HWND cells[4]) {
    RECT rc{}; GetClientRect(parent, &rc);
    int w = (rc.right - rc.left);
    int h = (rc.bottom - rc.top);
    int w2 = w / 2; int h2 = h / 2;
    MoveWindow(cells[0], 0, 0, w2, h2, TRUE);
    MoveWindow(cells[1], w2, 0, w - w2, h2, TRUE);
    MoveWindow(cells[2], 0, h2, w2, h - h2, TRUE);
    MoveWindow(cells[3], w2, h2, w - w2, h - h2, TRUE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppContext* ctx = reinterpret_cast<AppContext*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (ctx && ctx->mainHwnd) {
            layout_cells(ctx->mainHwnd, ctx->cells);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static HWND create_main_window(AppContext* ctx) {
    const wchar_t* clsName = L"Gst4RtspClass";

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ctx->hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = clsName;

    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        0, clsName, L"GStreamer 2x2 RTSP Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, ctx->hInst, nullptr);

    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

    // Create 4 child windows (targets for overlays)
    for (int i = 0; i < 4; ++i) {
        ctx->cells[i] = CreateWindowEx(
            0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, 100, 100, hwnd, nullptr, ctx->hInst, nullptr);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    layout_cells(hwnd, ctx->cells);
    return hwnd;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    int argc = 0; wchar_t** argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)argvW; // unused

    // Init GStreamer
    gst_init(nullptr, nullptr);

    AppContext ctx{};
    ctx.hInst = hInstance;
    ctx.mainHwnd = create_main_window(&ctx);

    // Stream URLs (placeholders per requirements)
    const std::vector<std::string> urls = {
        "rtsp://admin:tni%40123456@192.168.1.226/Streaming/channels/101",
        "rtsp://admin:tni%40123456@192.168.1.225/Streaming/channels/101",
        "rtspt://admin:TpcomsNOC107@103.141.176.254:7072/Streaming/Channels/101",
        "rtspt://hctech:Admin%40123@quangminhhome.dssddns.net:8889/Streaming/Channels/101"
    };

    // Create pipelines
    for (int i = 0; i < 4; ++i) {
        std::string name = std::string("cam") + std::to_string(i+1);
        auto* sp = new StreamPipeline(name, urls[i], ctx.cells[i]);
        if (!start_pipeline(sp)) {
            sp->logger.log("ERROR", "Initial start failed");
        }
        sp->busThread = std::thread([sp]() { bus_loop(sp); });
        ctx.streams.push_back(sp);
    }

    // Standard Win32 message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    for (auto* sp : ctx.streams) {
        sp->running = false;
        if (sp->busThread.joinable()) sp->busThread.join();
        teardown_pipeline(sp);
        delete sp;
    }

    gst_deinit();
    return 0;
}

// Provide a standard main() so the Console subsystem links successfully in VS generators.
// It forwards to wWinMain and shows the main window normally.
int main() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    return wWinMain(hInst, nullptr, GetCommandLineW(), SW_SHOWDEFAULT);
}
