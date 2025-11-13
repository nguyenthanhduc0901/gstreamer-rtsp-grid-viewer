#include <gst/gst.h>
#include <gtk/gtk.h>

#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <algorithm>

static const int SUB_W = 640;
static const int SUB_H = 360;

struct StreamPipeline {
    std::string name;
    std::string url;

    GstElement* pipeline {nullptr};
    GstElement* src {nullptr};
    GstElement* decode {nullptr}; // Sẽ luôn sử dụng decodebin
    GstElement* conv {nullptr};
    GstElement* sink {nullptr};
    GtkWidget*  widget {nullptr};

    int backoff_ms {500};
};

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

static void on_decode_pad_added(GstElement* decode, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!pad_has_video_caps(pad)) return;
    GstPad* sinkpad = gst_element_get_static_pad(sp->conv, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link decodebin->conv: %d\n", sp->name.c_str(), ret);
    }
    gst_object_unref(sinkpad);
}

static void on_src_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!pad_has_video_caps(pad)) return;
    GstElement* target = sp->decode; // Luôn là decodebin
    if (!target) return;
    GstPad* sinkpad = gst_element_get_static_pad(target, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link rtspsrc->decodebin: %d\n", sp->name.c_str(), ret);
    }
    gst_object_unref(sinkpad);
}
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
    gtk_window_set_title(GTK_WINDOW(window), "GStreamer 2x2 (GTK)");
    gtk_window_set_default_size(GTK_WINDOW(window), SUB_W * 2, SUB_H * 2);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_container_add(GTK_CONTAINER(window), grid);

    // Thay thế bằng URL thực của bạn
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

        sp->pipeline = gst_pipeline_new((sp->name + "_pipe").c_str());
        sp->src      = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());
        // === THAY ĐỔI 1: Luôn sử dụng 'decodebin' để tận dụng giải mã phần cứng ===
        sp->decode   = gst_element_factory_make("decodebin", (sp->name + "_decbin").c_str());
        sp->conv     = gst_element_factory_make("videoconvert", (sp->name + "_conv").c_str());
        sp->sink     = gst_element_factory_make("gtksink", (sp->name + "_sink").c_str());
        
        if (!sp->pipeline || !sp->src || !sp->decode || !sp->conv || !sp->sink) {
            g_printerr("[%s] Failed to create elements\n", sp->name.c_str());
            return -1;
        }

        g_object_set(G_OBJECT(sp->src), "location", sp->url.c_str(), "latency", 200, NULL); // Đặt latency nhỏ để giảm độ trễ
    
        // Cấu hình QoS để giảm độ trễ và bỏ frame khi cần thiết
        g_object_set(G_OBJECT(sp->sink), 
            "sync", FALSE,
            "async", FALSE,
            "qos", TRUE,
            "max-lateness", G_GUINT64_CONSTANT(50000000), // 50ms
            NULL);
    
        // === THAY ĐỔI 2: Xóa bỏ khối cấu hình "skip-frame" vì không còn cần thiết và gây lỗi ===
        // Không còn khối g_object_set cho sp->dec ở đây

        // Lấy GtkWidget từ gtksink và đặt vào grid
        g_object_get(G_OBJECT(sp->sink), "widget", &sp->widget, NULL);
        if (!sp->widget) {
            g_printerr("[%s] gtksink did not provide widget (install gstreamer1.0-gtk3)\n", sp->name.c_str());
            return -1;
        }
        gtk_widget_set_size_request(sp->widget, SUB_W, SUB_H);
        gtk_grid_attach(GTK_GRID(grid), sp->widget, i % 2, i / 2, 1, 1);

        // === THAY ĐỔI 3: Đơn giản hóa việc thêm và liên kết các element ===
        gst_bin_add_many(GST_BIN(sp->pipeline), sp->src, sp->decode, sp->conv, sp->sink, NULL);
        
        // Liên kết tĩnh conv -> sink
        if (!gst_element_link(sp->conv, sp->sink)) {
            g_printerr("[%s] Failed to link conv->sink\n", sp->name.c_str());
            return -1;
        }

        // Liên kết động rtspsrc -> decodebin -> videoconvert
        g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp.get());
        g_signal_connect(sp->decode, "pad-added", G_CALLBACK(on_decode_pad_added), sp.get());


        GstBus* bus = gst_element_get_bus(sp->pipeline);
        gst_bus_add_watch(bus, on_bus_msg, sp.get());
        gst_object_unref(bus);

        GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
        if (sret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("[%s] Failed to set PLAYING\n", sp->name.c_str());
            return -1;
        }

        pipes.push_back(std::move(sp));
    }

    gtk_widget_show_all(window);
    gtk_main();

    // Dọn dẹp
    for (auto& sp : pipes) {
        if (sp->pipeline) {
            gst_element_set_state(sp->pipeline, GST_STATE_NULL);
            gst_object_unref(sp->pipeline);
            sp->pipeline = nullptr;
        }
    }

    return 0;
}