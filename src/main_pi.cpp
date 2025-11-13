// Linux/GTK version: Tối ưu cho độ trễ thấp nhất, bỏ qua tài nguyên.
// Sử dụng decodebin để tự động chọn phần cứng giải mã (hardware decoder)
// và gtksink để render bằng GPU (Zero-Copy nếu có thể).

#include <gst/gst.h>
#include <gtk/gtk.h>

#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <algorithm>

// Kích thước widget mặc định (gtksink sẽ tự scale)
static const int SUB_W = 640;
static const int SUB_H = 360;

struct StreamPipeline {
    std::string name;
    std::string url;

    GstElement* pipeline {nullptr};
    GstElement* src {nullptr};      // rtspsrc
    GstElement* decode {nullptr};   // decodebin
    GstElement* sink {nullptr};     // gtksink
    GtkWidget* widget {nullptr};

    int backoff_ms {500};
};

// Kiểm tra xem pad có phải là video không (để bỏ qua audio)
static gboolean pad_has_video_caps(GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    gboolean is_video = FALSE;
    if (caps) {
        const GstStructure* st = gst_caps_get_structure(caps, 0);
        const gchar* name = gst_structure_get_name(st);
        if (g_str_has_prefix(name, "video/") || g_str_has_prefix(name, "video/x-raw") ||
            (g_str_has_prefix(name, "application/x-rtp") &&
             g_strcmp0(gst_structure_get_string(st, "media"), "video") == 0)) {
            is_video = TRUE;
        }
        gst_caps_unref(caps);
    }
    return is_video;
}

// Callback khi decodebin tạo ra một pad (đã giải mã)
static void on_decode_pad_added(GstElement* decode, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    
    // Chỉ link pad video, bỏ qua audio
    if (!pad_has_video_caps(pad)) {
        g_printerr("[%s] Bỏ qua pad không phải video\n", sp->name.c_str());
        return;
    }

    // Link pad mới này trực tiếp tới gtksink
    GstPad* sinkpad = gst_element_get_static_pad(sp->sink, "sink");
    if (!sinkpad) {
        g_printerr("[%s] Không lấy được sink pad của gtksink\n", sp->name.c_str());
        return;
    }
    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link decodebin -> gtksink: %d\n", sp->name.c_str(), ret);
    } else {
        g_printerr("[%s] Đã link decodebin -> gtksink\n", sp->name.c_str());
    }
    gst_object_unref(sinkpad);
}

// Callback khi rtspsrc tạo ra một pad (H264/H265)
static void on_src_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);

    // Chỉ link pad video
    if (!pad_has_video_caps(pad)) return;

    // Link pad mới này trực tiếp tới decodebin
    GstPad* sinkpad = gst_element_get_static_pad(sp->decode, "sink");
    if (!sinkpad) {
         g_printerr("[%s] Không lấy được sink pad của decodebin\n", sp->name.c_str());
        return;
    }
    if (gst_pad_is_linked(sinkpad)) {
        gst_object_unref(sinkpad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link rtspsrc -> decodebin: %d\n", sp->name.c_str(), ret);
    } else {
         g_printerr("[%s] Đã link rtspsrc -> decodebin\n", sp->name.c_str());
    }
    gst_object_unref(sinkpad);
}

// Logic khởi động lại pipeline (giữ nguyên)
static gboolean restart_pipeline_cb(gpointer user_data) {
    StreamPipeline* sp = static_cast<StreamPipeline*>(user_data);
    if (!sp || !sp->pipeline) return G_SOURCE_REMOVE;
    gst_element_set_state(sp->pipeline, GST_STATE_READY);
    GstStateChangeReturn s1 = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
    if (s1 == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s] Restart failed; will retry\n", sp->name.c_str());
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 5000);
        g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
    } else {
        sp->backoff_ms = 500;
        g_printerr("[%s] Restarted\n", sp->name.c_str());
    }
    return G_SOURCE_REMOVE;
}

// Logic theo dõi bus (giữ nguyên)
static gboolean on_bus_msg(GstBus* bus, GstMessage* msg, gpointer user_data) {
    StreamPipeline* sp = static_cast<StreamPipeline*>(user_data);
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
        g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 5000);
        break;
    }
    case GST_MESSAGE_EOS:
        g_printerr("[%s] EOS\n", sp->name.c_str());
        g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 5000);
        break;
    default: break;
    }
    return TRUE; // keep watching
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GStreamer 2x2 (LOW-LATENCY)");
    gtk_window_set_default_size(GTK_WINDOW(window), SUB_W * 2, SUB_H * 2);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_container_add(GTK_CONTAINER(window), grid);

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

        // --- Tạo Pipeline Tối giản ---
        sp->pipeline = gst_pipeline_new((sp->name + "_pipe").c_str());
        sp->src      = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());
        sp->decode   = gst_element_factory_make("decodebin", (sp->name + "_decbin").c_str());
        sp->sink     = gst_element_factory_make("gtksink", (sp->name + "_sink").c_str());
        
        if (!sp->pipeline || !sp->src || !sp->decode || !sp->sink) {
            g_printerr("[%s] Failed to create elements\n", sp->name.c_str());
            return -1;
        }

        // --- Cấu hình CỰC ĐOAN (AGGRESSIVE) cho độ trễ thấp ---
        g_object_set(G_OBJECT(sp->src), 
            "location", sp->url.c_str(),
            "latency", 0,                  // Yêu cầu buffer = 0
            "drop-on-lateness", TRUE,      // Bỏ khung hình trễ
            "ntp-sync", FALSE,             // Không đồng bộ với server NTP
            "protocols", 4, /* TCP */      // Ưu tiên TCP để tránh mất gói (rtspt://)
            NULL);
        
        g_object_set(G_OBJECT(sp->sink), 
            "sync", FALSE,                 // TẮT đồng bộ (vẽ ngay lập tức)
            "async", FALSE,                // TẮT đồng bộ (vẽ ngay lập tức)
            "qos", FALSE,                  // TẮT Quality-of-Service (không chờ, không báo cáo)
            "force-aspect-ratio", TRUE,
            NULL);

        // Lấy widget từ gtksink và thêm vào grid
        g_object_get(G_OBJECT(sp->sink), "widget", &sp->widget, NULL);
        if (!sp->widget) {
            g_printerr("[%s] gtksink did not provide widget\n", sp->name.c_str());
            return -1;
        }
        gtk_widget_set_size_request(sp->widget, SUB_W, SUB_H);
        gtk_grid_attach(GTK_GRID(grid), sp->widget, i % 2, i / 2, 1, 1);

        // Thêm elements vào pipeline
        gst_bin_add_many(GST_BIN(sp->pipeline), sp->src, sp->decode, sp->sink, NULL);

        // --- Kết nối động (Dynamic Linking) ---
        // 1. rtspsrc sẽ link tới decodebin
        g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp.get());
        // 2. decodebin sẽ link tới gtksink
        g_signal_connect(sp->decode, "pad-added", G_CALLBACK(on_decode_pad_added), sp.get());
        // (Không có link tĩnh nào cả)

        // Theo dõi lỗi
        GstBus* bus = gst_element_get_bus(sp->pipeline);
        gst_bus_add_watch(bus, on_bus_msg, sp.get());
        gst_object_unref(bus);

        // Chạy
        GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
        if (sret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("[%s] Failed to set PLAYING\n", sp->name.c_str());
        }

        pipes.push_back(std::move(sp));
    }

    gtk_widget_show_all(window);
    gtk_main();

    // Cleanup
    for (auto& sp : pipes) {
        if (sp->pipeline) {
            gst_element_set_state(sp->pipeline, GST_STATE_NULL);
            gst_object_unref(sp->pipeline);
            sp->pipeline = nullptr;
        }
    }

    return 0;
}