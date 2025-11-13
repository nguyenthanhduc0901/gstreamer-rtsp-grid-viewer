// Linux/GTK version: four independent pipelines optimized for Raspberry Pi 4 2GB
#include <gst/gst.h>
#include <gtk/gtk.h>
#include <cstdlib>

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
        // Dùng I420 để tiết kiệm bộ nhớ và tương thích tốt
        g_video_caps = gst_caps_new_simple("video/x-raw",
            "width", G_TYPE_INT, SUB_W,
            "height", G_TYPE_INT, SUB_H,
            "framerate", GST_TYPE_FRACTION, 20, 1,
            "format", G_TYPE_STRING, "I420",
            NULL);
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

static void on_decode_pad_added(GstElement* /*decode*/, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!sp || !pad_has_video_caps(pad)) return;
    GstElement* next = sp->scale;
    if (!next) return;
    GstPad* sinkpad = gst_element_get_static_pad(next, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link decodebin->scale: %d\n", sp->name.c_str(), ret);
    }
    gst_object_unref(sinkpad);
}

static void on_src_pad_added(GstElement* /*src*/, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!sp || !pad_has_video_caps(pad)) return;
    GstElement* target = sp->use_decodebin ? sp->decode : sp->depay;
    if (!target) return;
    GstPad* sinkpad = gst_element_get_static_pad(target, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link rtspsrc->%s: %d\n", sp->name.c_str(),
                   sp->use_decodebin ? "decodebin" : "depay", ret);
    }
    gst_object_unref(sinkpad);
}

static gboolean restart_pipeline_cb(gpointer user_data) {
    StreamPipeline* sp = static_cast<StreamPipeline*>(user_data);
    if (!sp || !sp->pipeline) return G_SOURCE_REMOVE;

    gst_element_set_state(sp->pipeline, GST_STATE_NULL);
    GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);

    if (sret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s] Restart failed; will retry\n", sp->name.c_str());
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        sp->restart_id = g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
    } else {
        sp->backoff_ms = 500;
        g_print("[%s] Restarted successfully\n", sp->name.c_str());
        sp->restart_id = 0;
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_bus_msg(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
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

        if (sp->restart_id) g_source_remove(sp->restart_id);
        sp->restart_id = g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("[%s] EOS - Restarting\n", sp->name.c_str());
        if (sp->restart_id) g_source_remove(sp->restart_id);
        sp->restart_id = g_timeout_add(sp->backoff_ms, restart_pipeline_cb, sp);
        sp->backoff_ms = std::min(sp->backoff_ms * 2, 10000);
        break;
    default: break;
    }
    return TRUE;
}

// Tạo decoder tối ưu cho Raspberry Pi 4 dựa trên codec
static GstElement* create_optimal_decoder(const std::string& name, bool use_h265) {
    GstElement* decoder = nullptr;
    if (use_h265) {
        decoder = gst_element_factory_make("v4l2slh265dec", name.c_str());
        if (!decoder) {
            decoder = gst_element_factory_make("avdec_h265", name.c_str());
            if (decoder) g_print("Using software H265 decoder for %s\n", name.c_str());
        } else {
            g_print("Using hardware H265 decoder (v4l2slh265dec) for %s\n", name.c_str());
        }
    } else {
        decoder = gst_element_factory_make("v4l2h264dec", name.c_str());
        if (!decoder) {
            decoder = gst_element_factory_make("avdec_h264", name.c_str());
            if (decoder) g_print("Using software H264 decoder for %s\n", name.c_str());
        } else {
            g_print("Using hardware H264 decoder (v4l2h264dec) for %s\n", name.c_str());
        }
    }
    return decoder;
}

// Cấu hình pipeline tối ưu cho Raspberry Pi 4 2GB
static void configure_pipeline_for_performance(StreamPipeline* sp) {
    if (!sp || !sp->pipeline) return;

    // rtspsrc - chỉ các property hợp lệ
    if (sp->src) {
        g_object_set(G_OBJECT(sp->src),
            "latency", 0,
            "protocols", "tcp",
            NULL);
    }

    // videoscale
    if (sp->scale) {
        g_object_set(G_OBJECT(sp->scale),
            "method", 1,
            NULL);
    }

    // caps filter
    if (sp->capsf && g_video_caps) {
        g_object_set(G_OBJECT(sp->capsf), "caps", g_video_caps, NULL);
    }

    // sink low-latency
    if (sp->sink) {
        g_object_set(G_OBJECT(sp->sink),
            "sync", FALSE,
            "async", FALSE,
            "max-lateness", -1,
            "qos", FALSE,
            NULL);
    }
}

static void cleanup_pipeline(StreamPipeline* sp) {
    if (!sp) return;
    if (sp->restart_id) { g_source_remove(sp->restart_id); sp->restart_id = 0; }
    if (sp->watch_id) { g_source_remove(sp->watch_id); sp->watch_id = 0; }
    if (sp->pipeline) {
        gst_element_set_state(sp->pipeline, GST_STATE_NULL);
        gst_object_unref(sp->pipeline);
        sp->pipeline = nullptr;
    }
}

int main(int argc, char** argv) {
    // init
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);

    // env tweaks for RPi
    setenv("GST_REGISTRY_FORK", "no", 1);
    setenv("GST_V4L2_USE_LIBV4L2", "1", 1);

    init_global_caps();

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "4-Camera Viewer - RPi4 Optimized");
    gtk_window_set_default_size(GTK_WINDOW(window), SUB_W * 2, SUB_H * 2);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 1);
    gtk_container_add(GTK_CONTAINER(window), grid);

    // URLs
    std::vector<std::string> urls = {
        "rtsp://admin:tni%40123456@192.168.1.226/Streaming/channels/101",
        "rtsp://admin:tni%40123456@192.168.1.225/Streaming/channels/101",
        "rtspt://admin:TpcomsNOC107@103.141.176.254:7072/Streaming/Channels/101",
        "rtspt://hctech:Admin%40123@quangminhhome.dssddns.net:8889/Streaming/Channels/101"
    };

    std::vector<std::unique_ptr<StreamPipeline>> pipes;
    pipes.reserve(4);

    bool any_pipeline_ok = false;

    for (int i = 0; i < 4; ++i) {
        auto sp = std::make_unique<StreamPipeline>();
        sp->name = "cam" + std::to_string(i+1);
        sp->url = urls[i];
        sp->use_decodebin = (i == 2); // cam3 decodebin

        g_print("Creating %s pipeline for %s\n",
                sp->use_decodebin ? "decodebin" : "H265", sp->name.c_str());

        sp->pipeline = gst_pipeline_new((sp->name + "_pipe").c_str());
        sp->src = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());

        if (sp->use_decodebin) {
            sp->decode = gst_element_factory_make("decodebin", (sp->name + "_decode").c_str());
        } else {
            sp->depay = gst_element_factory_make("rtph265depay", (sp->name + "_depay").c_str());
            sp->parse = gst_element_factory_make("h265parse", (sp->name + "_parse").c_str());
            sp->dec = create_optimal_decoder(sp->name + "_dec", true);
        }

        sp->scale = gst_element_factory_make("videoscale", (sp->name + "_scale").c_str());
        sp->capsf = gst_element_factory_make("capsfilter", (sp->name + "_caps").c_str());
        sp->conv = gst_element_factory_make("videoconvert", (sp->name + "_conv").c_str());
        sp->sink = gst_element_factory_make("gtksink", (sp->name + "_sink").c_str());

        if (!sp->pipeline || !sp->src || !sp->scale || !sp->capsf || !sp->conv || !sp->sink) {
            g_printerr("[%s] Failed to create basic elements\n", sp->name.c_str());
            continue;
        }
        if (sp->use_decodebin && !sp->decode) {
            g_printerr("[%s] Failed to create decodebin\n", sp->name.c_str());
            continue;
        }
        if (!sp->use_decodebin && (!sp->depay || !sp->parse || !sp->dec)) {
            g_printerr("[%s] Failed to create H265 elements\n", sp->name.c_str());
            continue;
        }

        g_object_get(G_OBJECT(sp->sink), "widget", &sp->widget, NULL);
        if (!sp->widget) {
            g_printerr("[%s] gtksink did not provide widget\n", sp->name.c_str());
            continue;
        }

        gtk_widget_set_size_request(sp->widget, SUB_W, SUB_H);
        gtk_grid_attach(GTK_GRID(grid), sp->widget, i % 2, i / 2, 1, 1);

        if (sp->use_decodebin) {
            gst_bin_add_many(GST_BIN(sp->pipeline),
                sp->src, sp->decode, sp->scale, sp->capsf, sp->conv, sp->sink, NULL);
            if (!gst_element_link_many(sp->scale, sp->capsf, sp->conv, sp->sink, NULL)) {
                g_printerr("[%s] Failed to link scale->caps->conv->sink\n", sp->name.c_str());
                continue;
            }
            g_signal_connect(sp->decode, "pad-added", G_CALLBACK(on_decode_pad_added), sp.get());
            g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp.get());
        } else {
            gst_bin_add_many(GST_BIN(sp->pipeline),
                sp->src, sp->depay, sp->parse, sp->dec, sp->scale, sp->capsf, sp->conv, sp->sink, NULL);
            if (!gst_element_link_many(sp->depay, sp->parse, sp->dec, sp->scale, sp->capsf, sp->conv, sp->sink, NULL)) {
                g_printerr("[%s] Failed to link H265 pipeline\n", sp->name.c_str());
                continue;
            }
            g_signal_connect(sp->src, "pad-added", G_CALLBACK(on_src_pad_added), sp.get());
        }

        g_object_set(G_OBJECT(sp->src), "location", sp->url.c_str(), NULL);
        configure_pipeline_for_performance(sp.get());

        GstBus* bus = gst_element_get_bus(sp->pipeline);
        sp->watch_id = gst_bus_add_watch(bus, on_bus_msg, sp.get());
        gst_object_unref(bus);

        GstStateChangeReturn sret = gst_element_set_state(sp->pipeline, GST_STATE_PLAYING);
        if (sret == GST_STATE_CHANGE_FAILURE) {
            g_printerr("[%s] Failed to start pipeline\n", sp->name.c_str());
            continue;
        }

        pipes.push_back(std::move(sp));
        any_pipeline_ok = true;
        g_print("[%s] Pipeline started successfully\n", urls[i].c_str());
    }

    if (!any_pipeline_ok) {
        g_printerr("No pipelines could be started. Exiting.\n");
        cleanup_global_caps();
        return -1;
    }

    gtk_widget_show_all(window);

    GtkSettings* settings = gtk_settings_get_default();
    if (settings) {
        g_object_set(settings,
            "gtk-enable-animations", FALSE,
            "gtk-application-prefer-dark-theme", TRUE,
            NULL);
    }

    g_print("=== Raspberry Pi 4 Camera Viewer Started ===\n");
    g_print("Low latency mode enabled (sync=false)\n");

    gtk_main();

    g_print("Cleaning up pipelines...\n");
    for (auto& sp : pipes) {
        if (sp) { cleanup_pipeline(sp.get()); }
    }
    pipes.clear();

    cleanup_global_caps();
    g_print("Cleanup completed. Goodbye!\n");
    return 0;
}
