// Linux/GTK version: four independent pipelines rendered via gtksink into a 2x2 GtkGrid

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
    GstElement* decode {nullptr};
    GstElement* depay {nullptr};
    GstElement* parse {nullptr};
    GstElement* dec {nullptr};
    GstElement* queue1 {nullptr};
    GstElement* queue2 {nullptr};
    GstElement* scale {nullptr};
    GstElement* capsf {nullptr};
    GstElement* conv {nullptr};
    GstElement* sink {nullptr};
    GtkWidget*  widget {nullptr};

    int backoff_ms {500};
    bool use_decodebin {false};
    guint watch_id {0};
    guint restart_id {0};
};

static GstCaps* g_video_caps = nullptr;

static void init_global_caps() {
    if (!g_video_caps) {
        g_video_caps = gst_caps_new_simple("video/x-raw",
            "width", G_TYPE_INT, SUB_W,
            "height", G_TYPE_INT, SUB_H,
            "framerate", GST_TYPE_FRACTION, 30, 1,
            "format", G_TYPE_STRING, "I420",
            NULL);
        gst_caps_ref(g_video_caps); // Giữ reference
    }
}

static void cleanup_global_caps() {
    if (g_video_caps) {
        gst_caps_unref(g_video_caps);
        g_video_caps = nullptr;
    }
}

static gboolean pad_has_video_caps(GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    
    gboolean is_video = FALSE;
    if (caps) {
        const GstStructure* st = gst_caps_get_structure(caps, 0);
        const gchar* name = gst_structure_get_name(st);
        if (g_str_has_prefix(name, "video/") || 
            g_str_has_prefix(name, "video/x-raw") ||
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
    if (!sp || !pad_has_video_caps(pad)) return;
    
    GstElement* next = sp->queue2;
    if (!next) return;
    
    GstPad* sinkpad = gst_element_get_static_pad(next, "sink");
    if (!sinkpad) {
        g_printerr("[%s] Failed to get sink pad from queue2\n", sp->name.c_str());
        return;
    }
    
    if (gst_pad_is_linked(sinkpad)) { 
        gst_object_unref(sinkpad); 
        return; 
    }
    
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link decodebin->queue2: %d\n", sp->name.c_str(), ret);
    } else {
        g_print("[%s] Successfully linked decodebin->queue2\n", sp->name.c_str());
    }
    gst_object_unref(sinkpad);
}

static void on_src_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!sp || !pad_has_video_caps(pad)) return;
    
    GstElement* target = sp->use_decodebin ? sp->decode : sp->queue1;
    if (!target) {
        g_printerr("[%s] No target element for pad-added\n", sp->name.c_str());
        return;
    }
    
    GstPad* sinkpad = gst_element_get_static_pad(target, "sink");
    if (!sinkpad) {
        g_printerr("[%s] Failed to get sink pad from target\n", sp->name.c_str());
        return;
    }
    
    if (gst_pad_is_linked(sinkpad)) { 
        gst_object_unref(sinkpad); 
        return; 
    }
    
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link rtspsrc->%s: %d\n", sp->name.c_str(), 
                   sp->use_decodebin ? "decodebin" : "queue1", ret);
    } else {
        g_print("[%s] Successfully linked rtspsrc->%s\n", sp->name.c_str(),
                sp->use_decodebin ? "decodebin" : "queue1");
    }
    gst_object_unref(sinkpad);
}

static gboolean restart_pipeline_cb(gpointer user_data) {
    StreamPipeline* sp = static_cast<StreamPipeline*>(user_data);
    if (!sp || !sp->pipeline) {
        g_printerr("Restart callback: invalid pipeline\n");
        return G_SOURCE_REMOVE;
    }
    
    g_print("[%s] Attempting restart...\n", sp->name.c_str());
    
    // Dừng pipeline
    gst_element_set_state(sp->pipeline, GST_STATE_NULL);
    
    // Reset backoff nếu restart thành công
    sp->backoff_ms = 500;
    
    // Khởi động lại
    GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s] Restart failed; will retry in %dms\n", sp->name.c_str(), sp->backoff_ms);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        sp->restart_id = g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
    } else {
        g_print("[%s] Restarted successfully\n", sp->name.c_str());
        sp->restart_id = 0;
    }
    
    return G_SOURCE_REMOVE;
}

static gboolean on_bus_msg(GstBus* bus, GstMessage* msg, gpointer user_data) {
    StreamPipeline* sp = static_cast<StreamPipeline*>(user_data);
    if (!sp) return TRUE;
    
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_WARNING: {
        GError* err = nullptr; 
        gchar* dbg = nullptr;
        gst_message_parse_warning(msg, &err, &dbg);
        g_printerr("[%s][WARN] %s | %s\n", sp->name.c_str(), err ? err->message : "no error", dbg ? dbg : "no debug");
        if (err) g_error_free(err); 
        if (dbg) g_free(dbg);
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr; 
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("[%s][ERROR] %s | %s\n", sp->name.c_str(), err ? err->message : "no error", dbg ? dbg : "no debug");
        
        // Hủy restart scheduled trước đó nếu có
        if (sp->restart_id) {
            g_source_remove(sp->restart_id);
            sp->restart_id = 0;
        }
        
        // Schedule restart mới
        sp->restart_id = g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        
        if (err) g_error_free(err); 
        if (dbg) g_free(dbg);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("[%s] EOS - Restarting\n", sp->name.c_str());
        
        if (sp->restart_id) {
            g_source_remove(sp->restart_id);
        }
        sp->restart_id = g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        break;
        
    case GST_MESSAGE_STATE_CHANGED: {
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(sp->pipeline)) {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
            g_print("[%s] State changed: %s -> %s\n", sp->name.c_str(),
                   gst_element_state_get_name(old_state),
                   gst_element_state_get_name(new_state));
        }
        break;
    }
        
    default: 
        break;
    }
    return TRUE;
}

static GstElement* create_optimal_decoder(const std::string& name, bool use_h265) {
    // Danh sách decoder ưu tiên cho Raspberry Pi
    const gchar* decoder_names[] = {
        // Hardware decoders
        "omxh265dec", "v4l2h265dec",  // H265 hardware
        "omxh264dec", "v4l2h264dec",  // H264 hardware
        // Software decoders
        "avdec_h265", "avdec_h264",   // Software fallback
        nullptr
    };
    
    int start_idx = use_h265 ? 0 : 2;
    GstElement* decoder = nullptr;
    
    for (int i = start_idx; decoder_names[i] && !decoder; i++) {
        if (i < 4) { // Hardware decoders
            decoder = gst_element_factory_make(decoder_names[i], name.c_str());
            if (decoder) {
                g_print("Using hardware decoder: %s\n", decoder_names[i]);
            }
        } else { // Software decoders
            decoder = gst_element_factory_make(decoder_names[i], name.c_str());
            if (decoder) {
                g_print("Using software decoder: %s\n", decoder_names[i]);
            }
        }
    }
    
    if (!decoder) {
        g_printerr("Failed to create any decoder for %s\n", name.c_str());
    }
    
    return decoder;
}

static void configure_pipeline_for_performance(StreamPipeline* sp) {
    if (!sp || !sp->pipeline) return;
    
    // Cấu hình rtspsrc cho low latency
    if (sp->src) {
        g_object_set(G_OBJECT(sp->src), 
            "latency", 0,
            "drop-on-lateness", TRUE,
            "do-retransmission", FALSE,
            "buffer-mode", 0,
            "ntp-sync", FALSE,
            "protocols", "tcp",
            "timeout", 5000000, // 5 second timeout
            NULL);
    }
    
    // Cấu hình queue trước decoder
    if (sp->queue1) {
        g_object_set(G_OBJECT(sp->queue1),
            "leaky", 2,
            "max-size-buffers", 1,
            "max-size-bytes", 0, 
            "max-size-time", 0,
            "silent", TRUE,
            NULL);
    }
    
    // Cấu hình queue sau decoder
    if (sp->queue2) {
        g_object_set(G_OBJECT(sp->queue2),
            "leaky", 2,  
            "max-size-buffers", 2,
            "max-size-bytes", 0,
            "max-size-time", 0,
            "silent", TRUE,
            NULL);
    }
    
    // Cấu hình decoder
    if (sp->dec) {
        g_object_set(G_OBJECT(sp->dec),
            "skip-frame", 0,
            "output-corrupt", FALSE,
            "threads", 1,
            NULL);
    }
    
    // Cấu hình decodebin nếu có
    if (sp->decode) {
        g_object_set(G_OBJECT(sp->decode),
            "max-size-buffers", 1,
            "max-size-time", 0,
            NULL);
    }
    
    // Cấu hình videoscale
    if (sp->scale) {
        g_object_set(G_OBJECT(sp->scale),
            "method", 0, // Nearest neighbour - fastest
            "sharpness", 0,
            NULL);
    }
    
    // Cấu hình caps filter
    if (sp->capsf && g_video_caps) {
        g_object_set(G_OBJECT(sp->capsf), "caps", g_video_caps, NULL);
    }
    
    // Cấu hình videoconvert
    if (sp->conv) {
        g_object_set(G_OBJECT(sp->conv),
            "n-threads", 1,
            "dither", 0,
            NULL);
    }
    
    // Cấu hình sink cho low latency
    if (sp->sink) {
        g_object_set(G_OBJECT(sp->sink),
            "sync", FALSE,
            "async", FALSE,
            "max-lateness", -1,
            "qos", FALSE,
            "enable-last-sample", FALSE,
            "force-aspect-ratio", TRUE,
            NULL);
    }
}

static void cleanup_pipeline(StreamPipeline* sp) {
    if (!sp) return;
    
    // Hủy các scheduled tasks
    if (sp->restart_id) {
        g_source_remove(sp->restart_id);
        sp->restart_id = 0;
    }
    
    if (sp->watch_id) {
        g_source_remove(sp->watch_id);
        sp->watch_id = 0;
    }
    
    // Dừng và hủy pipeline
    if (sp->pipeline) {
        gst_element_set_state(sp->pipeline, GST_STATE_NULL);
        gst_object_unref(sp->pipeline);
        sp->pipeline = nullptr;
    }
    
    // Widget sẽ được GTK tự động cleanup khi parent bị destroy
    if (sp->widget) {
        sp->widget = nullptr; // Không unref vì thuộc về GTK
    }
}

int main(int argc, char** argv) {
    // Tối ưu hóa khởi tạo
    gtk_disable_setlocale();
    gtk_init(&argc, &argv);
    
    gst_init(&argc, &argv);
    
    // Tối ưu hóa môi trường
    setenv("GST_REGISTRY_FORK", "no", 1);
    setenv("GST_DEBUG", "2", 0); // Chỉ hiển thị warning và error
    
    // Khởi tạo global caps
    init_global_caps();
    
    // Tạo window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "GStreamer 2x2 Ultra Low-Latency (RPi4)");
    gtk_window_set_default_size(GTK_WINDOW(window), SUB_W * 2, SUB_H * 2);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    // Tạo grid
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
    gtk_container_add(GTK_CONTAINER(window), grid);
    
    // URLs - thay thế bằng URLs thực tế của bạn
    std::vector<std::string> urls = {
        "rtsp://admin:tni%40123456@192.168.1.226/Streaming/channels/101",
        "rtsp://admin:tni%40123456@192.168.1.225/Streaming/channels/101", 
        "rtspt://admin:TpcomsNOC107@103.141.176.254:7072/Streaming/Channels/101",
        "rtspt://hctech:Admin%40123@quangminhhome.dssddns.net:8889/Streaming/Channels/101"
    };
    
    std::vector<std::unique_ptr<StreamPipeline>> pipes;
    pipes.reserve(4);
    
    bool all_pipelines_ok = true;
    
    for (int i = 0; i < 4; ++i) {
        auto sp = std::make_unique<StreamPipeline>();
        sp->name = "cam" + std::to_string(i+1);
        sp->url = urls[i];
        
        // Xác định loại pipeline
        sp->use_decodebin = (i == 2); // Chỉ cam3 dùng decodebin
        
        g_print("Creating pipeline for %s (decodebin: %s)\n", 
                sp->name.c_str(), sp->use_decodebin ? "yes" : "no");
        
        // Tạo pipeline và elements
        sp->pipeline = gst_pipeline_new((sp->name + "_pipe").c_str());
        if (!sp->pipeline) {
            g_printerr("[%s] Failed to create pipeline\n", sp->name.c_str());
            all_pipelines_ok = false;
            continue;
        }
        
        sp->src = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());
        sp->queue1 = gst_element_factory_make("queue", (sp->name + "_q1").c_str());
        sp->queue2 = gst_element_factory_make("queue", (sp->name + "_q2").c_str());
        sp->scale = gst_element_factory_make("videoscale", (sp->name + "_scale").c_str());
        sp->capsf = gst_element_factory_make("capsfilter", (sp->name + "_caps").c_str());
        sp->conv = gst_element_factory_make("videoconvert", (sp->name + "_conv").c_str());
        sp->sink = gst_element_factory_make("gtksink", (sp->name + "_sink").c_str());
        
        // Kiểm tra elements cơ bản
        if (!sp->src || !sp->queue1 || !sp->queue2 || !sp->scale || !sp->capsf || !sp->conv || !sp->sink) {
            g_printerr("[%s] Failed to create basic elements\n", sp->name.c_str());
            all_pipelines_ok = false;
            continue;
        }
        
        // Tạo elements riêng cho từng loại pipeline
        if (sp->use_decodebin) {
            sp->decode = gst_element_factory_make("decodebin", (sp->name + "_decodebin").c_str());
            if (!sp->decode) {
                g_printerr("[%s] Failed to create decodebin\n", sp->name.c_str());
                all_pipelines_ok = false;
                continue;
            }
        } else {
            sp->depay = gst_element_factory_make("rtph265depay", (sp->name + "_depay").c_str());
            sp->parse = gst_element_factory_make("h265parse", (sp->name + "_parse").c_str());
            sp->dec = create_optimal_decoder(sp->name + "_dec", true);
            
            if (!sp->depay || !sp->parse || !sp->dec) {
                g_printerr("[%s] Failed to create H265 elements\n", sp->name.c_str());
                all_pipelines_ok = false;
                continue;
            }
        }
        
        // Lấy widget từ gtksink
        g_object_get(G_OBJECT(sp->sink), "widget", &sp->widget, NULL);
        if (!sp->widget) {
            g_printerr("[%s] gtksink did not provide widget\n", sp->name.c_str());
            all_pipelines_ok = false;
            continue;
        }
        
        // Thêm widget vào grid
        gtk_widget_set_size_request(sp->widget, SUB_W, SUB_H);
        gtk_grid_attach(GTK_GRID(grid), sp->widget, i % 2, i / 2, 1, 1);
        
        // Thêm elements vào pipeline và kết nối
        if (sp->use_decodebin) {
            gst_bin_add_many(GST_BIN(sp->pipeline), 
                sp->src, sp->queue1, sp->decode, sp->queue2, 
                sp->scale, sp->capsf, sp->conv, sp->sink, NULL);
                
            // Kết nối chain cố định
            if (!gst_element_link_many(sp->queue1, sp->decode, NULL) ||
                !gst_element_link_many(sp->queue2, sp->scale, sp->capsf, sp->conv, sp->sink, NULL)) {
                g_printerr("[%s] Failed to link decodebin pipeline\n", sp->name.c_str());
                all_pipelines_ok = false;
                continue;
            }
            
            // Kết nối signals
            g_signal_connect(sp->decode, "pad-added", G_CALLBACK(on_decode_pad_added), sp.get());
            g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp.get());
            
        } else {
            gst_bin_add_many(GST_BIN(sp->pipeline), 
                sp->src, sp->queue1, sp->depay, sp->parse, sp->dec, sp->queue2,
                sp->scale, sp->capsf, sp->conv, sp->sink, NULL);
                
            // Kết nối toàn bộ pipeline
            if (!gst_element_link_many(sp->queue1, sp->depay, sp->parse, sp->dec, 
                                      sp->queue2, sp->scale, sp->capsf, sp->conv, sp->sink, NULL)) {
                g_printerr("[%s] Failed to link H265 pipeline\n", sp->name.c_str());
                all_pipelines_ok = false;
                continue;
            }
            
            g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp.get());
        }
        
        // Cấu hình URL
        g_object_set(G_OBJECT(sp->src), "location", sp->url.c_str(), NULL);
        
        // Cấu hình hiệu suất
        configure_pipeline_for_performance(sp.get());
        
        // Thiết lập bus watch
        GstBus* bus = gst_element_get_bus(sp->pipeline);
        sp->watch_id = gst_bus_add_watch(bus, on_bus_msg, sp.get());
        gst_object_unref(bus);
        
        // Khởi động pipeline
        GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
        if (sret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("[%s] Failed to start pipeline\n", sp->name.c_str());
            all_pipelines_ok = false;
            continue;
        }
        
        pipes.push_back(std::move(sp));
        g_print("[%s] Pipeline started successfully\n", urls[i].c_str());
    }
    
    if (pipes.empty()) {
        g_printerr("No pipelines could be started. Exiting.\n");
        cleanup_global_caps();
        return -1;
    }
    
    if (!all_pipelines_ok) {
        g_printerr("Warning: Some pipelines failed to start\n");
    }
    
    // Hiển thị window
    gtk_widget_show_all(window);
    
    // Tối ưu hóa GTK
    GtkSettings* settings = gtk_settings_get_default();
    if (settings) {
        g_object_set(settings, 
            "gtk-enable-animations", FALSE,
            "gtk-application-prefer-dark-theme", TRUE, // Dark theme có thể tiết kiệm năng lượng
            NULL);
    }
    
    g_print("All pipelines running. Use Ctrl+C to exit.\n");
    
    // Chạy main loop
    gtk_main();
    
    // Cleanup
    g_print("Cleaning up...\n");
    for (auto& sp : pipes) {
        if (sp) {
            cleanup_pipeline(sp.get());
        }
    }
    pipes.clear();
    
    cleanup_global_caps();
    
    g_print("Cleanup completed. Exiting.\n");
    return 0;
}