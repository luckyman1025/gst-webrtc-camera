#ifndef _DATA_STRUCT_H
#define _DATA_STRUCT_H
#include <glib.h>

struct _webrtc {
    gboolean enable;
    struct _turnserver {
        gchar *url;
        gchar *user;
        gchar *pwd;
    } turn;
    struct _udpsink {
        gboolean multicast;
        int port;
        gchar *addr;
    } udpsink;
};

struct _http_data {
    int port;
    gchar *host;
    gchar *user;
    gchar *password;
};

struct _GstConfigData {
    struct _v4l2src_data {
        gchar *device;
        int width;
        int height;
        int framerate;
        int io_mode;
        gchar *type;
        gchar *format;
    } v4l2src_data;
    gchar *root_dir;   // streams output root path;
    gboolean showdot; // generate gstreamer pipeline graphs;
    gboolean splitfile_sink; // splitmuxsink save multipart file.
    gboolean app_sink;       // appsink for filesink save.
    struct _hls_onoff {
        gboolean av_hlssink;         // audio and video hls output.
        gboolean motion_hlssink;     // motioncells video hls output.
        gboolean facedetect_hlssink; // facedetect video hls output.
        gboolean edge_hlssink;       // edge detect video hls output.
        gboolean cvtracker_hlssink;  // cvtracker video hls output.
    } hls_onoff;
    struct _http_data http;
    struct _udp_data { // udp multicastsink hls output.
        gboolean enable;
        gboolean multicast;
        int port;
        gchar *host;
    } udp;
    struct _hls_data {
        int files;
        int duration;
        gboolean showtext; // show some custom text overlay video;
    } hls;
    struct _audio_data {
        int path;
        int buf_time;
    } audio;
    int rec_len; // motion detect record duration, seconds.
    struct _webrtc webrtc;
};

// } config_data_init = {
//     .v4l2src_data.device = "/dev/vidoe0",
//     .v4l2src_data.width = 1280,
//     .v4l2src_data.height = 720,
//     .v4l2src_data.framerate = 30,
//     .v4l2src_data.type = "video/x-raw",
//     .v4l2src_data.format = "NV12",
//     .pipewire_path = 0,
//     .root_dir = "/tmp/output",
//     .showtext = FALSE,
//     .streams_onoff.udp_multicastsink = FALSE,
//     .streams_onoff.av_hlssink = FALSE,
//     .streams_onoff.motion_hlssink = FALSE,
//     .streams_onoff.splitfile_sink = FALSE,
//     .streams_onoff.facedetect_sink = FALSE,
//     .streams_onoff.app_sink = FALSE,
// };

typedef struct _GstConfigData GstConfigData;
#endif