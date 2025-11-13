// Linux/X11 version: four independent pipelines with GstVideoOverlay into a 2x2 X11 window

#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <X11/Xlib.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

static const int SUB_W = 640;
static const int SUB_H = 360;

struct StreamPipeline {
    std::string name;
    std::string url;
    ::Window win {0};

    GstElement* pipeline {nullptr};
    GstElement* src {nullptr};
    GstElement* decode {nullptr};
    GstElement* conv {nullptr};
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
    if (!sp->conv) return;
    GstPad* sinkpad = gst_element_get_static_pad(sp->conv, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link decodebin->videoconvert: %d\n", sp->name.c_str(), ret);
    }
    gst_object_unref(sinkpad);
}

static void on_src_pad_added(GstElement* src, GstPad* pad, gpointer user_data) {
    StreamPipeline* sp = reinterpret_cast<StreamPipeline*>(user_data);
    if (!pad_has_video_caps(pad)) return;
    if (!sp->decode) return;
    GstPad* sinkpad = gst_element_get_static_pad(sp->decode, "sink");
    if (!sinkpad) return;
    if (gst_pad_is_linked(sinkpad)) { gst_object_unref(sinkpad); return; }
    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[%s] Failed to link rtspsrc->decodebin: %d\n", sp->name.c_str(), ret);
    }
    gst_object_unref(sinkpad);
}

static bool build_and_play(StreamPipeline* sp) {
    sp->pipeline = gst_pipeline_new((sp->name + "_pipe").c_str());
    sp->src      = gst_element_factory_make("rtspsrc", (sp->name + "_src").c_str());
    sp->decode   = gst_element_factory_make("decodebin", (sp->name + "_decbin").c_str());
    sp->conv     = gst_element_factory_make("videoconvert", (sp->name + "_conv").c_str());
    sp->sink     = gst_element_factory_make("glimagesink", (sp->name + "_sink").c_str());
    if (!sp->pipeline || !sp->src || !sp->decode || !sp->conv || !sp->sink) {
        g_printerr("[%s] Failed to create elements\n", sp->name.c_str());
        return false;
    }

    g_object_set(G_OBJECT(sp->src), "location", sp->url.c_str(), NULL);
    g_object_set(G_OBJECT(sp->src), "latency", 0, NULL); // low-latency for responsiveness

    g_object_set(G_OBJECT(sp->sink), "sync", TRUE, NULL);
    g_object_set(G_OBJECT(sp->sink), "force-aspect-ratio", TRUE, NULL);

    gst_bin_add_many(GST_BIN(sp->pipeline), sp->src, sp->decode, sp->conv, sp->sink, NULL);

    // decodebin has dynamic src; link decodebin -> conv -> sink (conv->sink static, decode linked in callback)
    if (!gst_element_link_many(sp->conv, sp->sink, NULL)) {
        g_printerr("[%s] Failed to link conv->sink\n", sp->name.c_str());
        return false;
    }

    // Connect dynamic pad handlers
    g_signal_connect(sp->decode, "pad-added", G_CALLBACK(on_decode_pad_added), sp);
    g_signal_connect(sp->src,    "pad-added", G_CALLBACK(on_src_pad_added), sp);

    // Embed into provided X11 child window
    if (GST_IS_VIDEO_OVERLAY(sp->sink) && sp->win != 0) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sp->sink), (guintptr)sp->win);
        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sp->sink), TRUE);
    }

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
    sp->pipeline = sp->src = sp->decode = sp->conv = sp->sink = nullptr;
}

static void pipeline_worker(StreamPipeline* sp) {
    sp->running = true;
    while (sp->running) {
        if (!build_and_play(sp)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sp->backoff_ms));
            sp->backoff_ms = std::min(sp->backoff_ms * 2, 5000);
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

        // Restart with backoff if still running
        stop_and_cleanup(sp);
        if (sp->running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sp->backoff_ms));
            sp->backoff_ms = std::min(sp->backoff_ms * 2, 5000);
        }
    }
    // Final ensure cleanup
    stop_and_cleanup(sp);
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    // Assumes X11 session; for Wayland-only sessions, glimagesink may not support GstVideoOverlay
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::cerr << "Failed to open X display" << std::endl; return -1; }

    int screen = DefaultScreen(dpy);
    int win_w = SUB_W * 2;
    int win_h = SUB_H * 2;

    ::Window root = RootWindow(dpy, screen);
    ::Window main_win = XCreateSimpleWindow(dpy, root, 50, 50, win_w, win_h, 1,
                                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    Atom WM_DELETE_WINDOW = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, main_win, &WM_DELETE_WINDOW, 1);
    XStoreName(dpy, main_win, "GStreamer 2x2 (Linux)");
    XSelectInput(dpy, main_win, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(dpy, main_win);
    XFlush(dpy);

    // Create 4 child windows for embedding
    std::vector<::Window> childWins(4);
    for (int i = 0; i < 4; ++i) {
        int x = (i % 2) * SUB_W;
        int y = (i / 2) * SUB_H;
        childWins[i] = XCreateSimpleWindow(dpy, main_win, x, y, SUB_W, SUB_H, 0,
                                           BlackPixel(dpy, screen), BlackPixel(dpy, screen));
        XMapWindow(dpy, childWins[i]);
    }
    XFlush(dpy);

    // Replace with your real URLs
    std::vector<std::string> urls = {
        "rtsp://admin:tni%40123456@192.168.1.226/Streaming/channels/101",
        "rtsp://admin:tni%40123456@192.168.1.225/Streaming/channels/101",
        "rtsp://admin:TpcomsNOC107@103.141.176.254:7072/Streaming/Channels/101",
        "rtsp://hctech:Admin%40123@quangminhhome.dssddns.net:8889/Streaming/Channels/101"
    };

    // Create 4 pipelines mirroring the Windows approach
    std::vector<std::unique_ptr<StreamPipeline>> pipes;
    pipes.reserve(4);
    for (int i = 0; i < 4; ++i) {
        auto sp = std::make_unique<StreamPipeline>();
        sp->name = std::string("cam") + std::to_string(i+1);
        sp->url  = urls[i];
        sp->win  = childWins[i];
        pipes.push_back(std::move(sp));
    }

    // Start workers
    for (auto& sp : pipes) {
        sp->worker = std::thread(pipeline_worker, sp.get());
    }

    // Basic X11 event loop; press 'q' or close the window to exit
    bool running = true;
    while (running) {
        while (XPending(dpy) > 0) {
            XEvent ev{};
            XNextEvent(dpy, &ev);
            if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == WM_DELETE_WINDOW) {
                    running = false;
                }
            } else if (ev.type == KeyPress) {
                // Exit on any key for simplicity
                running = false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Shutdown pipelines
    for (auto& sp : pipes) sp->running = false;
    for (auto& sp : pipes) {
        if (sp->worker.joinable()) sp->worker.join();
    }

    // Destroy windows
    for (auto w : childWins) XDestroyWindow(dpy, w);
    XDestroyWindow(dpy, main_win);
    XCloseDisplay(dpy);

    return 0;
}
