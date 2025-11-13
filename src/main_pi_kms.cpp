// main_pi_kms.cpp
// Tối ưu cho Pi 4 2GB, chạy từ TTY, dùng kmssink Zero-Copy
//
// Cách build nhanh (trên Raspberry Pi / Linux):
//   g++ main_pi_kms.cpp -o rtsp_kms_viewer \
//       -std=c++17 -pthread \
//       $(pkg-config --cflags --libs gstreamer-1.0 glib-2.0)
//
// Gợi ý gói cần thiết (tùy distro):
//   sudo apt-get install -y build-essential pkg-config libglib2.0-dev \
//       gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
//       gstreamer1.0-plugins-bad

#include <gst/gst.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <algorithm>
#include <unistd.h> // Cho sleep()

// Độ phân giải màn hình của bạn
static const int SCREEN_W = 1920;
static const int SCREEN_H = 1080;

struct StreamPipeline {
    std::string name;
    std::string url;
    int index {0}; // 0, 1, 2, hoặc 3

    GstElement* pipeline {nullptr};
    GstElement* src {nullptr};
    GstElement* depay {nullptr};
    GstElement* parse {nullptr};
    GstElement* dec {nullptr};
    GstElement* sink {nullptr};

    std::thread worker;
    std::atomic<bool> running {false};
    int backoff_ms {500};
};

static gboolean pad_has_video_caps(GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    gboolean is_video = FALSE;
    if (caps) {
        const GstStructure* st = gst_caps_get_structure(caps, 0);
        const gchar* name = gst_structure_get_name(st);
        if (g_str_has_prefix(name, "application/x-rtp")) {
             const gchar* media = gst_structure_get_string(st, "media");
             if (media && g_strcmp0(media, "video") == 0) is_video = TRUE;
        }
        gst_caps_unref(caps);
    }
    return is_video;
}

static void on_src_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!pad_has_video_caps(pad)) return;
    
    // Tìm codec (H265 hay H264?)
    GstCaps* caps = gst_pad_get_current_caps(pad);
    const gchar* encoding_name = gst_structure_get_string(gst_caps_get_structure(caps, 0), "encoding-name");
    
    // Tự động tạo depay và parse dựa trên codec
    if (g_str_equal(encoding_name, "H265")) {
        sp->depay = gst_element_factory_make("rtph265depay", (sp->name + "_depay").c_str());
        sp->parse = gst_element_factory_make("h265parse", (sp->name + "_parse").c_str());
        // Thử bộ giải mã phần cứng H.265 (tùy nền tảng có sẵn)
        sp->dec   = gst_element_factory_make("v4l2slh265dec", (sp->name + "_dec").c_str());
        if (!sp->dec) sp->dec = gst_element_factory_make("v4l2h265dec", (sp->name + "_dec").c_str());
        if (!sp->dec) sp->dec = gst_element_factory_make("avdec_h265", (sp->name + "_dec").c_str());
    } else if (g_str_equal(encoding_name, "H264")) {
        sp->depay = gst_element_factory_make("rtph264depay", (sp->name + "_depay").c_str());
        sp->parse = gst_element_factory_make("h264parse", (sp->name + "_parse").c_str());
        // Thử bộ giải mã phần cứng H.264, fallback sang phần mềm
        sp->dec   = gst_element_factory_make("v4l2h264dec", (sp->name + "_dec").c_str());
        if (!sp->dec) sp->dec = gst_element_factory_make("avdec_h264", (sp->name + "_dec").c_str());
    } else {
        g_printerr("[%s] Codec không được hỗ trợ: %s\n", sp->name.c_str(), encoding_name);
        gst_caps_unref(caps);
        return;
    }
    gst_caps_unref(caps);

    if (!sp->depay || !sp->parse || !sp->dec) {
        g_printerr("[%s] Không thể tạo elements cho codec!\n", sp->name.c_str());
        return;
    }

    // Thêm các elements mới vào pipeline
    gst_bin_add_many(GST_BIN(sp->pipeline), sp->depay, sp->parse, sp->dec, NULL);
    // Link chúng lại với nhau và với sink
    if (!gst_element_link_many(sp->depay, sp->parse, sp->dec, sp->sink, NULL)) {
         g_printerr("[%s] Failed to link depay->parse->dec->sink\n", sp->name.c_str());
    }
    gst_element_sync_state_with_parent(sp->depay);
    gst_element_sync_state_with_parent(sp->parse);
    gst_element_sync_state_with_parent(sp->dec);

    // Link rtspsrc với depay
    GstPad* sinkpad = gst_element_get_static_pad(sp->depay, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link rtspsrc->depay: %d\n", sp->name.c_str(), ret);
    }
    gst_object_unref(sinkpad);
}

static bool build_and_play(StreamPipeline* sp) {
    sp->pipeline = gst_pipeline_new((sp->name + "_pipe").c_str());
    sp->src      = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());
    sp->sink     = gst_element_factory_make("kmssink", (sp->name + "_sink").c_str());
    
    if (!sp->pipeline || !sp->src || !sp->sink) {
        g_printerr("[%s] Failed to create core elements\n", sp->name.c_str());
        return false;
    }

    // --- Cấu hình RTSP cho độ trễ thấp ---
    g_object_set(G_OBJECT(sp->src), "location", sp->url.c_str(), NULL);
    g_object_set(G_OBJECT(sp->src), "latency", 0, NULL);
    // 'drop-on-lateness' không phải property của rtspsrc; dùng QoS/queue/sink để xử lý late frames
    g_object_set(G_OBJECT(sp->src), "protocols", 4 /* TCP */, NULL); // Ưu tiên TCP

    // --- Cấu hình KMSSink cho Zero-Copy và Grid ---
    int w2 = SCREEN_W / 2;
    int h2 = SCREEN_H / 2;
    int x = (sp->index % 2) * w2;
    int y = (sp->index / 2) * h2;
    
    gchar* rect = g_strdup_printf("<%d,%d,%d,%d>", x, y, w2, h2);
    
    g_object_set(G_OBJECT(sp->sink), 
        // "plane-id", 3 + sp->index, // Dùng các "lớp" (plane) khác nhau nếu cần
        "render-rectangle", rect,
        "sync", FALSE,       // Vẽ ngay khi có
        "async", FALSE,      // Giảm độ trễ
        "force-aspect-ratio", TRUE,
        NULL);
    g_free(rect);
    
    // Thêm các elements cốt lõi (depay, parse, dec sẽ được thêm trong on_src_pad_added)
    gst_bin_add_many(GST_BIN(sp->pipeline), sp->src, sp->sink, NULL);

    // Connect dynamic pad handler
    // CHÚ Ý: Chúng ta không link trước, chúng ta link MỌI THỨ trong callback
    g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp);

    GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s] Failed to set PLAYING\n", sp->name.c_str());
        return false;
    }

    sp->backoff_ms = 500; // reset backoff on success
    return true;
}

static void stop_and_cleanup(StreamPipeline* sp) {
    if (sp->pipeline) {
        gst_element_set_state(sp->pipeline, GST_STATE_NULL);
        gst_object_unref(sp->pipeline);
    }
    // GStreamer tự dọn dẹp các element con khi pipeline bị unref
    sp->pipeline = sp->src = sp->depay = sp->parse = sp->dec = sp->sink = nullptr;
}

static void pipeline_worker(StreamPipeline* sp) {
    sp->running = true;
    while (sp->running) {
        if (!build_and_play(sp)) {
            g_printerr("[%s] Build failed, retrying in %dms\n", sp->name.c_str(), sp->backoff_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(sp->backoff_ms));
            sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000); // Backoff tối đa 10s
            continue;
        }

        GstBus* bus = gst_element_get_bus(sp->pipeline);
        bool need_restart = false;
        while (sp->running && !need_restart) {
            GstMessage* msg = gst_bus_timed_pop_filtered(bus, 250 * GST_MSECOND,
                (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
            if (!msg) continue;
            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_WARNING: {
                GError* err = nullptr; gchar* dbg = nullptr;
                gst_message_parse_warning(msg, &err, &dbg);
                g_printerr("[%s][WARN] %s | %s\n", sp->name.c_str(), err?err->message:"", dbg?dbg:"");
                if (err) g_error_free(err); if (dbg) g_free(dbg);
                break;
            }
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr; gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                g_printerr("[%s][ERROR] %s | %s\n", sp->name.c_str(), err?err->message:"", dbg?dbg:"");
                if (err) g_error_free(err); if (dbg) g_free(dbg);
                need_restart = true;
                break;
            }
            case GST_MESSAGE_EOS:
                g_printerr("[%s] EOS\n", sp->name.c_str());
                need_restart = true;
                break;
            default: break;
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);

        // Dọn dẹp trước khi restart
        stop_and_cleanup(sp);
        if (sp->running) {
            g_printerr("[%s] Restarting in %dms\n", sp->name.c_str(), sp->backoff_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(sp->backoff_ms));
            sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        }
    }
    // Dọn dẹp lần cuối khi thoát
    stop_and_cleanup(sp);
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    // DÙNG LUỒNG PHỤ (SUBSTREAM) NẾU CÓ!
    // Các URL này có thể là luồng chính 1080p (nếu bạn đã đổi)
    // Hoặc luồng phụ 640x360 (ví dụ: .../channels/102)
    std::vector<std::string> urls = {
        "rtsp://admin:tni%40123456@192.168.1.226/Streaming/channels/101",
        "rtsp://admin:tni%40123456@192.168.1.225/Streaming/channels/101",
        "rtspt://admin:TpcomsNOC107@103.141.176.254:7072/Streaming/Channels/101",
        "rtspt://hctech:Admin%40123@quangminhhome.dssddns.net:8889/Streaming/Channels/101"
    };

    std::vector<std::unique_ptr<StreamPipeline>> pipes;
    pipes.reserve(4);
    for (int i = 0; i < 4; ++i) {
        auto sp = std::make_unique<StreamPipeline>();
        sp->name = std::string("cam") + std::to_string(i+1);
        sp->url  = urls[i];
        sp->index = i;
        pipes.push_back(std::move(sp));
    }

    // Start workers
    for (auto& sp : pipes) {
        sp->worker = std::thread(pipeline_worker, sp.get());
    }

    // Vòng lặp chính (chỉ để giữ chương trình chạy)
    // Bấm Ctrl+C để thoát
    g_print("Đang chạy 4 luồng. Bấm Ctrl+C để thoát.\n");
    std::atomic<bool> app_running = true;
    // (Chúng ta có thể thêm trình xử lý signal cho Ctrl+C, nhưng sleep là đủ)
    while (app_running) {
        sleep(1); // Ngủ 1 giây
    }

    // Tín hiệu tắt (ví dụ: Ctrl+C) sẽ không tới đây trừ khi bạn bắt signal
    // Nhưng khi chương trình bị kill, các thread cũng sẽ tắt
    // Để cho sạch, chúng ta nên bắt Ctrl+C
    g_print("Đang tắt...\n");
    for (auto& sp : pipes) sp->running = false;
    for (auto& sp : pipes) {
        if (sp->worker.joinable()) sp->worker.join();
    }

    return 0;
}
