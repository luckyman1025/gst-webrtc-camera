#include "gst-app.h"
#include "data_struct.h"
#include "soup.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/types.h>

static GstElement *pipeline;
static GstElement *video_source, *audio_source, *h264_encoder, *va_pp;
static gboolean is_initial = FALSE;

static volatile int threads_running = 0;
static volatile int cmd_recording = 0;
static int record_time = 7;
static volatile gboolean reading_inotify = TRUE;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cmd_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    GstElement *video_src;
    GstElement *audio_src;
} AppSrcAVPair;

static GMutex G_appsrc_lock;
static GList *G_AppsrcList;

GstConfigData config_data;

static CustomAppData app_data;

#define MAKE_ELEMENT_AND_ADD(elem, name)                          \
    G_STMT_START {                                                \
        GstElement *_elem = gst_element_factory_make(name, NULL); \
        if (!_elem) {                                             \
            gst_printerrln("%s is not available", name);          \
            return -1;                                            \
        }                                                         \
        elem = _elem;                                             \
        gst_bin_add(GST_BIN(pipeline), elem);                     \
    }                                                             \
    G_STMT_END

#define SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, elem, name)             \
    G_STMT_START {                                                \
        GstElement *_elem = gst_element_factory_make(name, NULL); \
        if (!_elem) {                                             \
            gst_printerrln("%s is not available", name);          \
            return -1;                                            \
        }                                                         \
        elem = _elem;                                             \
        gst_bin_add(GST_BIN(bin), elem);                          \
    }                                                             \
    G_STMT_END

static void
_initial_device();
static int start_udpsrc_rec();
static void start_appsrc_record();

#if 0
static GstCaps *_getVideoCaps(gchar *type, gchar *format, int framerate, int width, int height) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "framerate", GST_TYPE_FRACTION, framerate, 1,
                               "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
                               "width", G_TYPE_INT, width,
                               "height", G_TYPE_INT, height,
                               NULL);
}

static GstCaps *_getAudioCaps(gchar *type, gchar *format, int rate, int channel) {
    return gst_caps_new_simple(type,
                               "format", G_TYPE_STRING, format,
                               "rate", G_TYPE_INT, rate,
                               "channel", G_TYPE_INT, channel,
                               NULL);
}

#endif

static void _mkdir(const char *path, int mask) {

#if 0
    struct stat st = {0};
    int result = 0;
    if (stat(path, &st) == -1) {
        result = mkdir(path, mask);
    }
    return result;
#endif
    gchar *tmp;
    char *p = NULL;
    size_t len;

    tmp = g_strdup_printf("%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    mkdir(tmp, S_IRWXU);
    g_free(tmp);
}

static gboolean _check_initial_status() {
    if (!is_initial) {
        g_printerr("Must be initialized device before using it.\n");
    }
    return is_initial;
}

// https://gstreamer.freedesktop.org/documentation/gstreamer/gi-index.html?gi-language=c
// https://gstreamer.freedesktop.org/documentation/tutorials/basic/index.html?gi-language=c
// https://github.com/hgtcs/gstreamer/blob/master/v4l2-enc-appsink.c
// https://gstreamer.freedesktop.org/documentation/tutorials/basic/dynamic-pipelines.html?gi-language=c

GstElement *
launch_from_shell(char *pipeline) {
    GError *error = NULL;
    return gst_parse_launch(pipeline, &error);
}

static gchar *get_format_current_time() {
    GDateTime *datetime;
    gchar *time_str;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F %T");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
}

static gchar *get_current_time_str() {
    GDateTime *datetime;
    gchar *time_str;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F_%H-%M-%S");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
}

static gchar *get_today_str() {
    GDateTime *datetime;
    gchar *time_str;

    datetime = g_date_time_new_now_local();

    time_str = g_date_time_format(datetime, "%F");

    g_date_time_unref(datetime);
    return g_strdup(time_str);
}

static GstElement *video_src() {
    GstCaps *srcCaps;
    gchar *capBuf;
    GstElement *teesrc, *source, *srcvconvert, *capsfilter, *queue;
    source = gst_element_factory_make("v4l2src", NULL);

    teesrc = gst_element_factory_make("tee", NULL);
    srcvconvert = gst_element_factory_make("videoconvert", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);

    queue = gst_element_factory_make("queue", NULL);

    if (!teesrc || !source || !srcvconvert || !capsfilter || !queue) {
        g_printerr("video_src all elements could be created.\n");
        return NULL;
    }
    // srcCaps = _getVideoCaps("image/jpeg", "NV12", 30, 1280, 720);
    g_print("device: %s, Type: %s, format: %s\n", config_data.v4l2src_data.device, config_data.v4l2src_data.type, config_data.v4l2src_data.format);

    capBuf = g_strdup_printf("%s, width=%d, height=%d, framerate=(fraction)%d/1",
                             config_data.v4l2src_data.type,
                             config_data.v4l2src_data.width,
                             config_data.v4l2src_data.height,
                             config_data.v4l2src_data.framerate);
    srcCaps = gst_caps_from_string(capBuf);
    g_free(capBuf);

    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    g_object_set(G_OBJECT(source),
                 "device", config_data.v4l2src_data.device,
                 "io-mode", config_data.v4l2src_data.io_mode,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, srcvconvert, teesrc, queue, NULL);

    if (!strncmp(config_data.v4l2src_data.type, "image/jpeg", 10)) {
        GstElement *jpegparse, *jpegdec;
        jpegparse = gst_element_factory_make("jpegparse", NULL);
        jpegdec = gst_element_factory_make("jpegdec", NULL);
        if (!jpegdec || !jpegparse) {
            g_printerr("video_src all elements could be created.\n");
            return NULL;
        }
        gst_bin_add_many(GST_BIN(pipeline), jpegparse, jpegdec, NULL);
        if (!gst_element_link_many(source, capsfilter, jpegparse, jpegdec, queue, srcvconvert, teesrc, NULL)) {
            g_error("Failed to link elements video mjpg src\n");
            return NULL;
        }
    } else {
        if (!gst_element_link_many(source, queue, srcvconvert, teesrc, NULL)) {
            g_error("Failed to link elements video src\n");
            return NULL;
        }
    }

    return teesrc;
}

static GstElement *audio_src() {
    GstElement *teesrc, *source, *srcvconvert, *enc, *resample;
    teesrc = gst_element_factory_make("tee", NULL);
    source = gst_element_factory_make("pipewiresrc", NULL);
    srcvconvert = gst_element_factory_make("audioconvert", NULL);
    resample = gst_element_factory_make("audioresample", NULL);
    enc = gst_element_factory_make("opusenc", NULL);

    if (!teesrc || !source || !srcvconvert || !resample || !enc) {
        g_printerr("audio source all elements could be created.\n");
        return NULL;
    }

    gst_bin_add_many(GST_BIN(pipeline), source, teesrc, srcvconvert, enc, resample, NULL);
    if (!gst_element_link_many(source, srcvconvert, resample, enc, teesrc, NULL)) {
        g_error("Failed to link elements audio src.\n");
        return NULL;
    }

    return teesrc;
}

static GstElement *encoder_h264() {
    GstElement *encoder, *clock, *teeh264, *queue, *capsfilter;
    GstPad *src_pad, *queue_pad;
    GstCaps *srcCaps;
    clock = gst_element_factory_make("clockoverlay", NULL);
    teeh264 = gst_element_factory_make("tee", NULL);
    queue = gst_element_factory_make("queue", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    g_object_set(G_OBJECT(queue), "max-size-buffers", 1, NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        encoder = gst_element_factory_make("vaapih264enc", NULL);
        g_object_set(G_OBJECT(encoder), "bitrate", 8000, NULL);
    } else {
        encoder = gst_element_factory_make("x264enc", NULL);
        // g_object_set(G_OBJECT(encoder), "key-int-max", 2, NULL);
        g_object_set(G_OBJECT(encoder), "bitrate", 8000, "speed-preset", 1, "tune", 4, "key-int-max", 30, NULL);
    }

    srcCaps = gst_caps_from_string("video/x-h264,profile=constrained-baseline");
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    if (!encoder || !clock || !teeh264 || !queue || !capsfilter) {
        g_printerr("encoder_h264 all elements could not be created.\n");
        // g_printerr("encoder %x ; clock %x.\n", encoder, clock);
        return NULL;
    }

    gst_bin_add_many(GST_BIN(pipeline), clock, encoder, teeh264, queue, capsfilter, NULL);
    if (!gst_element_link_many(queue, clock, encoder, capsfilter, teeh264, NULL)) {
        g_error("Failed to link elements encoder \n");
        return NULL;
    }

    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);

    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("Obtained request pad %s for from device source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee source could not be linked.\n");
        return NULL;
    }
    gst_object_unref(src_pad);
    gst_object_unref(queue_pad);

    return teeh264;
}

static GstElement *vaapi_postproc() {
    GstElement *vaapipostproc, *capsfilter, *clock, *tee;
    GstPad *src_pad, *queue_pad;
    gchar *capBuf;

    if (!gst_element_factory_find("vaapipostproc")) {
        return video_source;
    }
    vaapipostproc = gst_element_factory_make("vaapipostproc", NULL);
    capsfilter = gst_element_factory_make("capsfilter", NULL);
    clock = gst_element_factory_make("clockoverlay", NULL);
    tee = gst_element_factory_make("tee", NULL);

    if (!vaapipostproc || !capsfilter || !clock || !tee) {
        g_printerr("splitfile_sink not all elements could be created.\n");
        return NULL;
    }
    gst_bin_add_many(GST_BIN(pipeline), clock, capsfilter, vaapipostproc, tee, NULL);
    if (!gst_element_link_many(vaapipostproc, capsfilter, clock, tee, NULL)) {
        g_error("Failed to link elements vaapi post proc.\n");
        return NULL;
    }

    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);

    // GstCaps *srcCaps = _getVideoCaps("video/x-raw", "NV12", 30, 1280, 720);
    // GstCaps *srcCaps = _getVideoCaps(
    //     config_data.v4l2src_data.type,
    //     config_data.v4l2src_data.format,
    //     config_data.v4l2src_data.framerate,
    //     config_data.v4l2src_data.width,
    //     config_data.v4l2src_data.height);

    capBuf = g_strdup_printf("%s, width=%d, height=%d, framerate=(fraction)%d/1",
                             "video/x-raw",
                             config_data.v4l2src_data.width,
                             config_data.v4l2src_data.height,
                             config_data.v4l2src_data.framerate);
    GstCaps *srcCaps = gst_caps_from_string(capBuf);
    g_free(capBuf);
    g_object_set(G_OBJECT(capsfilter), "caps", srcCaps, NULL);
    gst_caps_unref(srcCaps);

    g_object_set(G_OBJECT(vaapipostproc), "format", 23, NULL);

    src_pad = gst_element_request_pad_simple(video_source, "src_%u");
    g_print("Obtained request pad %s for from device source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(vaapipostproc, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee split file sink could not be linked.\n");
        return NULL;
    }
    gst_object_unref(queue_pad);
    gst_object_unref(src_pad);
    return tee;
}

static void *_inotify_thread(void *filename) {
    static int inotifyFd, wd;
    int ret;
    ssize_t rsize;
    static char buf[4096];
    struct inotify_event *event;

    inotifyFd = inotify_init1(IN_NONBLOCK);

    if (inotifyFd == -1) {
        gst_printerr("inotify init failed %d.\n", errno);
        exit(EXIT_FAILURE);
    }
    // gst_println("inotify monitor for file: %s .\n", (char *)filename);
    if (!g_file_test((char *)filename, G_FILE_TEST_EXISTS)) {
        gst_print(" inotify monitor exists: %s ?\n", (char *)filename);
        FILE *file;
        file = fopen((char *)filename, "w");
        fclose(file);
    }
    wd = inotify_add_watch(inotifyFd, (char *)filename, IN_ALL_EVENTS);
    if (wd == -1) {
        gst_printerr("inotify_add_watch failed, errno: %d.\n", errno);
        exit(EXIT_FAILURE);
    }
    g_free(filename);
    for (;;) {
        rsize = read(inotifyFd, buf, sizeof(buf));
        if (rsize == -1 && errno != EAGAIN) {
            perror("read ");
            // exit(EXIT_FAILURE);
        }

        if (!rsize) {
            gst_printerr("read() from intify fd returned 0 !.\n");
        }

        for (char *ptr = buf; ptr < buf + rsize;) {
            event = (struct inotify_event *)ptr;
            if (event->mask & IN_MODIFY || event->mask & IN_OPEN) {
                if (!threads_running) {
                    ret = pthread_mutex_lock(&mtx);
                    if (ret) {
                        g_error("Failed to lock on mutex.\n");
                    }
                    threads_running = TRUE;
                    ret = pthread_mutex_unlock(&mtx);
                    if (ret) {
                        g_error("Failed to lock on mutex.\n");
                    }
                    if (config_data.app_sink) {
                        g_thread_new("start_record_mkv", (GThreadFunc)start_appsrc_record, NULL);
                    } else {
                        g_thread_new("start_record_mkv", (GThreadFunc)start_udpsrc_rec, NULL);
                    }

                    // start_motion_record();
                }
                break;
            }
            ptr += sizeof(struct inotify_event) + event->len;
        }
    }

    gst_println("Exiting inotify thread..., errno: %d .\n", errno);
}

#if 0
/* called when the appsink notifies us that there is a new buffer ready for
 * processing */
static GstFlowReturn
on_new_sample_from_sink(GstElement *elt, CustomAppData *data) {
    GstSample *sample;
    GstBuffer *app_buffer, *buffer;
    GstElement *source;
    GstFlowReturn ret = GST_FLOW_NOT_LINKED;

    /* get the sample from appsink */
    sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
    buffer = gst_sample_get_buffer(sample);

    /* make a copy */
    app_buffer = gst_buffer_copy(buffer);

    /* we don't need the appsink sample anymore */
    gst_sample_unref(sample);
    /* get source an push new buffer */
    if (data->appsrc == NULL)
        return ret;
    source = gst_bin_get_by_name(GST_BIN(data->appsrc), src_name);
    if (source) {
        ret = gst_app_src_push_buffer(GST_APP_SRC(source), app_buffer);
        gst_object_unref(source);
    }
    return ret;
}

#endif

/* called when we get a GstMessage from the sink pipeline when we get EOS, we
 * exit the mainloop and this testapp. */
static gboolean
on_sink_message(GstBus *bus, GstMessage *message, CustomAppData *data) {
    gchar *name;
    name = gst_object_get_path_string(message->src);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
        g_print("Finished playback, src: %s\n", name);
        break;
    case GST_MESSAGE_ERROR:
        g_print("Received error, src: %s\n", name);
        break;
    default:
        break;
    }
    g_free(name);
    return TRUE;
}

static gboolean
on_source_message(GstBus *bus, GstMessage *message, CustomAppData *data) {
    gchar *name;
    name = gst_object_get_path_string(message->src);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS: {
        g_print("Finished record, src: %s\n", name);
    }

    break;
    case GST_MESSAGE_ERROR:
        g_print("Received error, src: %s\n", name);
        break;
    default:
        break;
    }
    g_free(name);
    return TRUE;
}

void udpsrc_cmd_rec_stop(gpointer user_data) {
    RecordItem *item = (RecordItem *)user_data;
    GstBus *bus;
    gst_element_set_state(GST_ELEMENT(item->pipeline),
                          GST_STATE_NULL);
    g_print("stop udpsrc record.\n");

    bus = gst_pipeline_get_bus(GST_PIPELINE(item->pipeline));
    if (bus != NULL) {
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
    }

    gst_object_unref(GST_OBJECT(item->pipeline));
    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = FALSE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    item->pipeline = NULL;
}

int get_record_state() { return cmd_recording ? 1 : 0; }

void udpsrc_cmd_rec_start(gpointer user_data) {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    gchar *fullpath;
    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = TRUE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    RecordItem *item = (RecordItem *)user_data;
    gchar *timestr = NULL;
    gchar *cmdline = NULL;
    gchar *today = get_today_str();

    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("starting record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();
    gchar *filename = g_strdup_printf("/webrtc_record-%s.mkv", timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(filename);
    g_free(timestr);

    gchar *audio_src = g_strdup_printf("udpsrc port=6001 name=audio_save  ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! opusparse ! queue ! mux.");
    gchar *video_src = g_strdup_printf("udpsrc port=6000  name=video_save ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! queue ! mux. ");
    cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
    g_free(fullpath);
    g_free(outdir);
    g_print("record cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);
    gst_element_set_state(item->pipeline, GST_STATE_PLAYING);
}

static gboolean stop_udpsrc_rec(GstElement *rec_pipeline) {
    // GstBus *bus;
    gst_element_set_state(GST_ELEMENT(rec_pipeline),
                          GST_STATE_NULL);
    g_print("stop udpsrc record.\n");

    // bus = gst_pipeline_get_bus(GST_PIPELINE(rec_pipeline));
    // gst_bus_remove_watch(bus);
    // gst_object_unref(bus);

    gst_object_unref(GST_OBJECT(rec_pipeline));
    if (pthread_mutex_lock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    threads_running = FALSE;
    if (pthread_mutex_unlock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    return TRUE;
}

static int start_udpsrc_rec(gpointer user_data) {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    GstElement *rec_pipeline;
    gchar *fullpath;
    gchar *timestr = NULL;
    gchar *cmdline = NULL;
    gchar *today = get_today_str();

    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("starting record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();
    gchar *filename = g_strdup_printf("/motion-%s.mkv", timestr);
    g_free(timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(outdir);

    gchar *audio_src = g_strdup_printf("udpsrc port=6001 name=audio_save  ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! opusparse ! queue ! mux.");
    gchar *video_src = g_strdup_printf("udpsrc port=6000  name=video_save ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! queue ! mux. ");
    cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
    g_free(fullpath);
    g_print("record cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    rec_pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);
    gst_element_set_state(rec_pipeline, GST_STATE_PLAYING);
    g_timeout_add_once(record_time * 1000, (GSourceOnceFunc)stop_udpsrc_rec, rec_pipeline);
    return 0;
}

#if 0
/** the need-data function does not work on multiple threads. Becuase the appsink will become a race condition. */
static pthread_mutex_t appsink_mtx = PTHREAD_MUTEX_INITIALIZER;
static void
need_data(GstElement *appsrc, guint unused, GstElement *appsink) {
    GstSample *sample;
    GstFlowReturn ret;

    if (pthread_mutex_lock(&appsink_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
    if (pthread_mutex_unlock(&appsink_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    if (sample) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstSegment *seg = gst_sample_get_segment(sample);
        GstClockTime pts, dts;

        /* Convert the PTS/DTS to running time so they start from 0 */
        pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts))
            pts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, pts);

        dts = GST_BUFFER_DTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(dts))
            dts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, dts);

        if (buffer) {
            /* Make writable so we can adjust the timestamps */
            buffer = gst_buffer_copy(buffer);
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = dts;
            g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
            gst_buffer_unref(buffer);
        }

        /* we don't need the appsink sample anymore */
        gst_sample_unref(sample);
    }
}
#endif

static void
on_ice_gathering_state_notify(GstElement *webrtcbin, GParamSpec *pspec,
                              gpointer user_data) {
    GstWebRTCICEGatheringState ice_gather_state;
    const gchar *new_state = "unknown";
    gchar *biname = gst_element_get_name(webrtcbin);

    g_object_get(webrtcbin, "ice-gathering-state", &ice_gather_state, NULL);
    switch (ice_gather_state) {
    case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
        new_state = "new";
        break;
    case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
        new_state = "gathering";
        break;
    case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
        new_state = "complete";
        break;
    }
    gst_print("%s ICE gathering state changed to %s\n", biname, new_state);
    g_free(biname);
}

static void
on_peer_connection_state_notify(GstElement *webrtcbin, GParamSpec *pspec,
                                gpointer user_data) {
    GstWebRTCPeerConnectionState ice_gather_state;
    const gchar *new_state = "unknown";
    gchar *biname = gst_element_get_name(webrtcbin);

    g_object_get(webrtcbin, "connection-state", &ice_gather_state, NULL);
    switch (ice_gather_state) {
    case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:
        new_state = "new";
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:
        new_state = "connecting";
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
        new_state = "connected";
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
        new_state = "disconnected";
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
        new_state = "failed";
        break;
    case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
        new_state = "closed";
        break;
    }
    gst_print("%s webrtc connection state changed to %s\n", biname, new_state);
    g_free(biname);
}

void appsrc_cmd_rec_stop(gpointer user_data) {
    RecordItem *item = (RecordItem *)user_data;
    GstBus *bus;
    gst_element_set_state(GST_ELEMENT(item->pipeline),
                          GST_STATE_NULL);
    g_print("stop appsrc record.\n");

    bus = gst_pipeline_get_bus(GST_PIPELINE(item->pipeline));
    if (bus != NULL) {
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
    }

    gst_object_unref(GST_OBJECT(item->pipeline));
    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = FALSE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_remove(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
    gst_object_unref(item->rec_avpair.audio_src);
    gst_object_unref(item->rec_avpair.video_src);
    item->pipeline = NULL;
}

static void appsrc_cmd_rec_start(gpointer user_data) {
    /**
     * @brief I want to create a module for recording, but it cannot be dynamically added and deleted while the pipeline is running。
     * Maybe it's because I'm not familiar with its mechanics.
     *
     */
    RecordItem *item = (RecordItem *)user_data;
    gchar *fullpath;
    gchar *cmdline;
    gchar *timestr = NULL;
    gchar *today = NULL;
    GstBus *bus;

    if (pthread_mutex_lock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    cmd_recording = TRUE;
    if (pthread_mutex_unlock(&cmd_mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    today = get_today_str();
    const gchar *vid_str = "video_appsrc_cmd_rec";
    const gchar *aid_str = "audio_appsrc_cmd_rec";

    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("start record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();

    /** here just work for mpegtsmux */
    gchar *filename = g_strdup_printf("/webrtc-%s.mkv", timestr);
    g_free(timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(outdir);

    gchar *audio_src = g_strdup_printf("appsrc name=%s  format=3 ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay  ! opusparse ! queue  ! mux.",
                                       aid_str);
    gchar *video_src = g_strdup_printf("appsrc  name=%s format=3 ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse !  queue ! mux. ",
                                       vid_str);
    cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
    g_free(fullpath);
    g_free(filename);
    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);

    bus = gst_pipeline_get_bus(GST_PIPELINE(item->pipeline));
    gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(item->pipeline, GST_STATE_PLAYING);
    item->rec_avpair.video_src = gst_bin_get_by_name(GST_BIN(item->pipeline), vid_str);
    // g_signal_connect(appsrc_vid, "need-data", (GCallback)need_data, video_sink);

    item->rec_avpair.audio_src = gst_bin_get_by_name(GST_BIN(item->pipeline), aid_str);
    // g_signal_connect(appsrc_aid, "need-data", (GCallback)need_data, audio_sink);
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_append(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
}

static gboolean stop_appsrc_rec(gpointer user_data) {
    gchar *timestr;
    RecordItem *item = (RecordItem *)user_data;
    gst_element_send_event(item->pipeline, gst_event_new_eos());
    gst_element_set_state(item->pipeline, GST_STATE_NULL);
    timestr = get_format_current_time();
    gst_println("stop appsrc record at: %s .\n", timestr);
    g_free(timestr);

    // gst_println("after stop record pipeline state: %s !!!!\n", gst_element_state_get_name(state));
    if (pthread_mutex_lock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }
    threads_running = FALSE;
    if (pthread_mutex_unlock(&mtx)) {
        g_error("Failed to lock on mutex.\n");
    }

    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_remove(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
    gst_object_unref(item->rec_avpair.audio_src);
    gst_object_unref(item->rec_avpair.video_src);

    gst_object_unref(item->pipeline);
    g_free(item);
    return TRUE;
}

static void start_appsrc_record() {
    RecordItem *item;
    gchar *fullpath;
    gchar *cmdline;
    gchar *timestr = NULL;
    gchar *today = NULL;
    GstBus *bus;
    today = get_today_str();
    const gchar *vid_str = "video_appsrc";
    const gchar *aid_str = "audio_appsrc";
    item = g_new0(RecordItem, 1);
    gchar *outdir = g_strconcat(config_data.root_dir, "/record/", today, NULL);
    g_free(today);

    timestr = get_format_current_time();
    gst_println("start appsrc record at: %s .\n", timestr);
    g_free(timestr);

    _mkdir(outdir, 0755);
    timestr = get_current_time_str();

    /** here just work for mpegtsmux */
    gchar *filename = g_strdup_printf("/motion-%s.mkv", timestr);
    g_free(timestr);
    fullpath = g_strconcat(outdir, filename, NULL);
    g_free(outdir);

    gchar *audio_src = g_strdup_printf("appsrc name=%s  format=3 ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay  ! opusparse ! queue  ! mux.",
                                       aid_str);
    gchar *video_src = g_strdup_printf("appsrc  name=%s format=3 ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse !  queue ! mux. ",
                                       vid_str);
    cmdline = g_strdup_printf(" matroskamux name=mux ! filesink  async=false location=\"%s\" %s %s ", fullpath, audio_src, video_src);
    g_free(fullpath);
    g_free(filename);
    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->pipeline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);

    item->rec_avpair.video_src = gst_bin_get_by_name(GST_BIN(item->pipeline), vid_str);
    // g_signal_connect(appsrc_vid, "need-data", (GCallback)need_data, video_sink);

    item->rec_avpair.audio_src = gst_bin_get_by_name(GST_BIN(item->pipeline), aid_str);
    // g_signal_connect(appsrc_aid, "need-data", (GCallback)need_data, audio_sink);

    bus = gst_pipeline_get_bus(GST_PIPELINE(item->pipeline));
    gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(item->pipeline, GST_STATE_PLAYING);
    g_timeout_add_once(record_time * 1000, (GSourceOnceFunc)stop_appsrc_rec, item);
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_append(G_AppsrcList, &item->rec_avpair);
    g_mutex_unlock(&G_appsrc_lock);
}

static gboolean
has_running_xwindow() {
    const gchar *xdg_stype = g_getenv("XDG_SESSION_TYPE");
    // const gchar *gdm = g_getenv("GDMSESSION");

    return g_strcmp0(xdg_stype, "tty");
}

static void
on_incoming_decodebin_stream(GstElement *decodebin, GstPad *pad,
                             GstElement *pipe) {
    GstStructure *structure;
    GstCaps *caps;
    const gchar *name;
    const gchar *encode_name;
    GstPadLinkReturn ret;
    GstElement *playbin;
    gchar *desc;

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    if (!gst_pad_has_current_caps(pad)) {
        gst_printerr("Pad '%s' has no caps, can't do anything, ignoring\n",
                     GST_PAD_NAME(pad));
        return;
    }
    caps = gst_pad_get_current_caps(pad);
    structure = gst_caps_get_structure(caps, 0);
    // name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    /**
     * @NOTE webrtcbin pad-added caps is below:
     *  application/x-rtp, media=(string)audio, payload=(int)111, clock-rate=(int)48000, encoding-name=(string)OPUS,
     *  encoding-params=(string)2, minptime=(string)10, useinbandfec=(string)1, rtcp-fb-transport-cc=(boolean)true,
     *  ssrc=(uint)1413127686
     */
    name = gst_structure_get_string(structure, "media");
    encode_name = gst_structure_get_string(structure, "encoding-name");

#if 0
    gchar *caps_str = gst_caps_to_string(caps);
    GST_DEBUG("name: %s, caps size: %d, caps : %s \n", name, gst_caps_get_size(caps), caps_str);
    g_free(caps_str);
#endif
    gst_caps_unref(caps);
    if (g_strcmp0(name, "audio") == 0) {
        if (g_strcmp0(encode_name, "OPUS") == 0) {
            desc = g_strdup_printf(" rtpopusdepay ! opusdec ! queue !  audioconvert  ! webrtcechoprobe ! autoaudiosink");
        } else {
            desc = g_strdup_printf(" decodebin ! queue ! audioconvert  ! webrtcechoprobe  ! autoaudiosink");
        }
        playbin = gst_parse_bin_from_description(desc, TRUE, NULL);
        g_free(desc);
        gst_bin_add(GST_BIN(pipe), playbin);
    } else if (g_strcmp0(name, "video") == 0) {
        if (!has_running_xwindow()) {
            gst_printerr("Current system not running on Xwindow. \n");
            return;
        }

        if (g_strcmp0(encode_name, "VP8") == 0) {
            desc = g_strdup_printf(" rtpvp8depay ! vp8dec ! queue ! videoconvert ! autovideosink");
            g_print("recv vp8 stream from remote.\n");
        } else if (g_strcmp0(encode_name, "H264") == 0) {
            const gchar *vah264 = "vaapih264dec";
            const gchar *nvh264 = "nvh264dec";
            desc = g_strdup_printf(" rtph264depay ! h264parse ! %s ! queue ! videoconvert ! autovideosink",
                                   gst_element_factory_find(vah264) ? vah264 : gst_element_factory_find(nvh264) ? nvh264
                                                                                                                : "avdec_h264");
        } else {
            desc = g_strdup_printf(" decodebin ! queue ! videoconvert ! autovideosink");
        }

        playbin = gst_parse_bin_from_description(desc, TRUE, NULL);
        g_free(desc);
        gst_bin_add(GST_BIN(pipe), playbin);
    } else {
        gst_printerr("Unknown pad %s, ignoring", GST_PAD_NAME(pad));
        return;
    }
    gst_element_sync_state_with_parent(playbin);

    ret = gst_pad_link(pad, playbin->sinkpads->data);
    g_assert_cmphex(ret, ==, GST_PAD_LINK_OK);
}

static GObject *send_channel, *receive_channel;

static void
data_channel_on_error(GObject *dc, gpointer user_data) {
    g_printerr("Data channel error \n");
}

static void
data_channel_on_open(GObject *dc, gpointer user_data) {
    GBytes *bytes = g_bytes_new("data", strlen("data"));
    gst_print("data channel opened\n");
    g_signal_emit_by_name(dc, "send-string", "Hi! from GStreamer");
    g_signal_emit_by_name(dc, "send-data", bytes);
    g_bytes_unref(bytes);
}

static void
data_channel_on_close(GObject *dc, gpointer user_data) {
    g_print("Data channel closed\n");
}

static void stop_recv_webrtc(gpointer user_data) {
    GstBus *bus;
    GstIterator *iter = NULL;
    gboolean done;

    RecvItem *recv_entry = (RecvItem *)user_data;

    iter = gst_bin_iterate_elements(GST_BIN(recv_entry->recvpipe));
    if (iter == NULL)
        return;
    done = FALSE;
    while (!done) {
        GValue data = {
            0,
        };

        switch (gst_iterator_next(iter, &data)) {
        case GST_ITERATOR_OK: {
            GstElement *child = g_value_get_object(&data);
            g_print("remove bin: %s \n", gst_element_get_name(child));
            gst_bin_remove(GST_BIN(recv_entry->recvpipe), child);
            gst_element_set_state(child, GST_STATE_NULL);
            // gst_object_unref(child);
            g_value_reset(&data);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iter);
            break;
        case GST_ITERATOR_ERROR:
            done = TRUE;
            break;
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }
    gst_iterator_free(iter);

    if (recv_entry->recvpipe)
        gst_element_set_state(GST_ELEMENT(recv_entry->recvpipe),
                              GST_STATE_NULL);

    bus = gst_pipeline_get_bus(GST_PIPELINE(recv_entry->recvpipe));
    if (bus != NULL) {
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
    }

    gst_object_unref(recv_entry->recvpipe);
    recv_entry->stop_recv = NULL;
    recv_entry->recvpipe = NULL;
}

#if 0
static void
data_channel_on_buffered_amound_low(GObject *channel, gpointer user_data) {
    GstWebRTCDataChannelState state;
    g_object_get(channel, "ready-state", &state, NULL);
    g_print("receive data_channel_on_buffered_amound_low channel state:%d \n", state);
}
#endif

static void
play_voice(gpointer user_data) {
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    GstBus *bus;
    GstMessage *msg;
    GstElement *playline;
    gchar *cmdline = g_strdup_printf("filesrc location=%s ! decodebin ! audioconvert ! autoaudiosink", item_entry->dcfile.filename);
    g_print("play cmdline : %s\n", cmdline);
    playline = gst_parse_launch(cmdline, NULL);
    g_free(cmdline);
    gst_element_set_state(playline, GST_STATE_PLAYING);

    bus = gst_pipeline_get_bus(GST_PIPELINE(playline));
    msg = gst_bus_poll(bus, GST_MESSAGE_EOS | GST_MESSAGE_ERROR, -1);
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS: {
        g_print("EOS\n");
        break;
    }
    case GST_MESSAGE_ERROR: {
        GError *err = NULL; /* error to show to users                 */
        gchar *dbg = NULL;  /* additional debug string for developers */

        gst_message_parse_error(msg, &err, &dbg);
        if (err) {
            g_print("ERROR: %s\n", err->message);
            g_error_free(err);
        }
        if (dbg) {
            g_print("[Debug details: %s]\n", dbg);
            g_free(dbg);
        }
    }
    default:
        g_print("Unexpected message of type %d", GST_MESSAGE_TYPE(msg));
        break;
    }
    gst_message_unref(msg);

    gst_element_set_state(playline, GST_STATE_NULL);
    gst_object_unref(playline);
    gst_object_unref(bus);

    remove(item_entry->dcfile.filename);
    if (item_entry->dcfile.filename != NULL)
    {
        g_free(item_entry->dcfile.filename);
        item_entry->dcfile.filename = NULL;
    }
}

static void
data_channel_on_message_data(GObject *channel, GBytes *bytes, gpointer user_data) {
    gsize size;
    const gchar *data;
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    if (item_entry->dcfile.fd > 0) {
        if (item_entry->dcfile.pos != item_entry->dcfile.fsize) {
            data = g_bytes_get_data(bytes, &size);
            write(item_entry->dcfile.fd, data, size);
            item_entry->dcfile.pos += size;
            if (item_entry->dcfile.pos == item_entry->dcfile.fsize) {
                close(item_entry->dcfile.fd);
                // g_timeout_add_once(50, (GSourceOnceFunc)play_voice, user_data);
                g_thread_new("play_voice", (GThreadFunc)play_voice, user_data);
            }
        }
    }
}

static void
data_channel_on_message_string(GObject *dc, gchar *str, gpointer user_data) {
    JsonNode *root_json;
    JsonObject *root_json_object;
    JsonParser *json_parser = NULL;
    const gchar *type_string;
    WebrtcItem *item_entry = (WebrtcItem *)user_data;
    // direct use str will occur malloc_consolidate(): unaligned fastbin chunk detected.
    gchar *tmp_str = g_strdup(str);

    // gst_print("Received data channel message: %s\n", tmp_str);
    json_parser = json_parser_new();
    if (!json_parser_load_from_data(json_parser, tmp_str, -1, NULL))
        goto unknown_message;

    root_json = json_parser_get_root(json_parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_json))
        goto unknown_message;

    root_json_object = json_node_get_object(root_json);

    if (!json_object_has_member(root_json_object, "type")) {
        g_error("Received message without type field\n");
        goto cleanup;
    }
    type_string = json_object_get_string_member(root_json_object, "type");

    if (json_object_has_member(root_json_object, type_string)) {
        const gchar *cmd_type_string;
        cmd_type_string = json_object_get_string_member(root_json_object, type_string);
        if (!g_strcmp0(cmd_type_string, "sendfile")) {
            JsonObject *file_object = json_object_get_object_member(root_json_object, "file");
            // const gchar *file_type = json_object_get_string_member(file_object, "type");

            item_entry->dcfile.filename = g_strdup_printf("/tmp/%s", json_object_get_string_member(file_object, "name"));
            item_entry->dcfile.fsize = json_object_get_int_member_with_default(file_object, "size", 0);
            g_print("recv msg file: %s\n", item_entry->dcfile.filename);
            item_entry->dcfile.fd = open(item_entry->dcfile.filename, O_RDWR | O_CREAT, 0644);
            item_entry->dcfile.pos = 0;

            // g_print("recv sendfile, name: %s, size: %ld, type: %s\n", file_name, file_size, file_type);
            goto cleanup;
        }
    }
cleanup:
    g_free(tmp_str);
    if (json_parser != NULL)
        g_object_unref(G_OBJECT(json_parser));
    return;

unknown_message:
    g_print("Unknown message \"%s\", ignoring\n", tmp_str);
    goto cleanup;
}

static void
connect_data_channel_signals(GObject *data_channel, gpointer user_data) {
    g_signal_connect(data_channel, "on-error",
                     G_CALLBACK(data_channel_on_error), NULL);
    g_signal_connect(data_channel, "on-open", G_CALLBACK(data_channel_on_open),
                     NULL);
    g_signal_connect(data_channel, "on-close",
                     G_CALLBACK(data_channel_on_close), NULL);
    g_signal_connect(data_channel, "on-message-string",
                     G_CALLBACK(data_channel_on_message_string), user_data);
    g_signal_connect(data_channel, "on-message-data",
                     G_CALLBACK(data_channel_on_message_data), user_data);
#if 0
    g_object_set(data_channel, "buffered-amount-low-threshold", FALSE, NULL);
    g_signal_connect(data_channel, "on-buffered-amount-low",
                     G_CALLBACK(data_channel_on_buffered_amound_low), user_data);
#endif
}

static void
on_data_channel(GstElement *webrtc, GObject *data_channel,
                gpointer user_data) {
    connect_data_channel_signals(data_channel, user_data);
    receive_channel = data_channel;
}

static void
create_data_channel(gpointer user_data) {
    WebrtcItem *item = (WebrtcItem *)user_data;

    g_signal_connect(item->sendbin, "on-data-channel", G_CALLBACK(on_data_channel),
                     (gpointer)item);

    gchar *chname = g_strdup_printf("channel_%ld", item->hash_id);
    g_signal_emit_by_name(item->sendbin, "create-data-channel", chname, NULL,
                          &send_channel);
    g_free(chname);
}

static void
_on_new_transceiver(GstElement *webrtc, GstWebRTCRTPTransceiver *trans) {
    /* If we expected more than one transceiver, we would take a look at
     * trans->mline, and compare it with webrtcbin's local description */
    g_object_set(trans, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, NULL);
}

static void
on_remove_decodebin_stream(GstElement *srcbin, GstPad *pad,
                           GstElement *pipe) {
    gchar *name = gst_pad_get_name(pad);
    g_print("pad removed %s !!! \n", name);
    // gst_bin_remove(GST_BIN_CAST(pipe), srcbin);
    gst_element_set_state(srcbin, GST_STATE_NULL);
    g_free(name);
}

static void start_recv_webrtcbin(gpointer user_data) {
    WebrtcItem *item = (WebrtcItem *)user_data;
    // gchar *turn_srv;
    GstBus *bus;
    gchar *pipe_name = g_strdup_printf("recv_%ld", item->hash_id);
    gchar *bin_name = g_strdup_printf("recvbin_%ld", item->hash_id);

    // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
    item->recv.recvpipe = gst_pipeline_new(pipe_name);
    // item->recv.recvbin = gst_element_factory_make_full("webrtcbin", "name", bin_name,
    //                                                    "turn-server", turn_srv, NULL);

    item->recv.recvbin = gst_element_factory_make_full("webrtcbin", "name", bin_name,
                                                       "stun-server", config_data.webrtc.stun, NULL);
    // g_free(turn_srv);
    g_free(pipe_name);
    g_free(bin_name);

    g_assert_nonnull(item->recv.recvbin);
    item->recv.stop_recv = &stop_recv_webrtc;
    g_object_set(G_OBJECT(item->recv.recvbin), "async-handling", TRUE, NULL);
    g_object_set(G_OBJECT(item->recv.recvbin), "bundle-policy", 3, NULL);

    bus = gst_pipeline_get_bus(GST_PIPELINE(item->recv.recvpipe));
    gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    gst_object_unref(bus);

    /* Takes ownership of each: */
    gst_bin_add(GST_BIN(item->recv.recvpipe), item->recv.recvbin);

    gst_element_set_state(item->recv.recvpipe, GST_STATE_READY);

    g_signal_connect(item->recv.recvbin, "pad-added",
                     G_CALLBACK(on_incoming_decodebin_stream), item->recv.recvpipe);

    g_signal_connect(item->recv.recvbin, "pad-removed",
                     G_CALLBACK(on_remove_decodebin_stream), item->recv.recvpipe);

    g_signal_connect(item->recv.recvbin, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_gathering_state_notify), NULL);

    g_signal_connect(item->recv.recvbin, "notify::ice-connection-state",
                     G_CALLBACK(on_peer_connection_state_notify), NULL);

    g_signal_connect(item->recv.recvbin, "on-new-transceiver",
                     G_CALLBACK(_on_new_transceiver), item->recv.recvpipe);

#if 0
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(item->recv.recvpipe), GST_DEBUG_GRAPH_SHOW_ALL, "webrtc_recv");
#endif
}

void start_udpsrc_webrtcbin(WebrtcItem *item) {
    GError *error = NULL;
    gchar *cmdline = NULL;
    GstBus *bus = NULL;
    // gchar *turn_srv = NULL;
    const gchar *webrtc_name = g_strdup_printf("send_%ld", item->hash_id);
    gchar *audio_src = g_strdup_printf("udpsrc port=%d multicast-group=%s ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! rtpopuspay ! queue ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " queue ! %s.",
                                       config_data.webrtc.udpsink.port + 1, config_data.webrtc.udpsink.addr, webrtc_name);
    gchar *video_src = g_strdup_printf("udpsrc port=%d multicast-group=%s ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! rtph264pay config-interval=-1 ! queue ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " queue ! %s. ",
                                       config_data.webrtc.udpsink.port, config_data.webrtc.udpsink.addr, webrtc_name);

    // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
    // cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
    cmdline = g_strdup_printf("webrtcbin name=%s stun-server=%s %s %s ", webrtc_name, config_data.webrtc.stun, audio_src, video_src);
    // g_free(turn_srv);

    GST_DEBUG("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->sendpipe = gst_parse_launch(cmdline, &error);
    gst_element_set_state(item->sendpipe, GST_STATE_READY);

    g_free(cmdline);
    bus = gst_pipeline_get_bus(GST_PIPELINE(item->sendpipe));
    gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    gst_object_unref(bus);

    item->sendbin = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    item->record.get_rec_state = &get_record_state;
    item->record.start = &udpsrc_cmd_rec_start;
    item->record.stop = &udpsrc_cmd_rec_stop;
    item->recv.addremote = &start_recv_webrtcbin;

    create_data_channel((gpointer)item);
#if 0
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(item->sendpipe), GST_DEBUG_GRAPH_SHOW_ALL, "udpsrc_webrtc");
#endif
}

int start_av_udpsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *video_sink, *audio_sink, *video_pay, *audio_pay, *h264parse;
    GstPad *src_vpad, *src_apad, *sink_vpad, *sink_apad;
    GstPadLinkReturn lret;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(aqueue, "queue");
    MAKE_ELEMENT_AND_ADD(video_pay, "rtph264pay");
    MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");

    MAKE_ELEMENT_AND_ADD(video_sink, "udpsink");
    MAKE_ELEMENT_AND_ADD(audio_sink, "udpsink");

    gst_bin_add_many(GST_BIN(pipeline), video_sink, audio_sink, NULL);

    /* Configure udpsink */
    g_object_set(video_sink, "sync", FALSE, "async", FALSE,
                 "port", config_data.webrtc.udpsink.port,
                 "host", config_data.webrtc.udpsink.addr,
                 "auto-multicast", config_data.webrtc.udpsink.multicast, NULL);
    g_object_set(audio_sink, "sync", FALSE, "async", FALSE,
                 "port", config_data.webrtc.udpsink.port + 1,
                 "host", config_data.webrtc.udpsink.addr,
                 "auto-multicast", config_data.webrtc.udpsink.multicast, NULL);

    g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    g_object_set(audio_pay, "pt", 97, NULL);
    g_object_set(vqueue, "max-size-time", 100000000, NULL);

    /* link to upstream. */
    if (!gst_element_link_many(aqueue, audio_pay, audio_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    if (!gst_element_link_many(vqueue, h264parse, video_pay, video_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");

    if ((lret = gst_pad_link(src_vpad, sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee udp video sink could not be linked. ret: %d \n", lret);
        return -1;
    }

    gst_object_unref(sink_vpad);
    gst_object_unref(src_vpad);

    // add audio to muxer.
    src_apad = gst_element_request_pad_simple(audio_source, "src_%u");
    sink_apad = gst_element_get_static_pad(aqueue, "sink");
    if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
        gst_printerrln("Tee udp audio sink could not be linked, link return :%d .\n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sink_apad));
    gst_object_unref(GST_OBJECT(src_apad));

    return 0;
}

static void stop_appsrc_webrtc(gpointer user_data) {
    GstBus *bus;
    WebrtcItem *webrtc_entry = (WebrtcItem *)user_data;

    gst_element_set_state(GST_ELEMENT(webrtc_entry->sendpipe),
                          GST_STATE_NULL);

    bus = gst_pipeline_get_bus(GST_PIPELINE(webrtc_entry->sendpipe));
    if (bus != NULL) {
        gst_bus_remove_watch(bus);
        gst_object_unref(bus);
    }

    gst_object_unref(GST_OBJECT(webrtc_entry->sendbin));
    gst_object_unref(GST_OBJECT(webrtc_entry->sendpipe));
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_remove(G_AppsrcList, &webrtc_entry->send_avpair);
    g_mutex_unlock(&G_appsrc_lock);
    gst_object_unref(webrtc_entry->send_avpair.video_src);
    gst_object_unref(webrtc_entry->send_avpair.audio_src);
}

static void
check_webrtcbin_state_by_timer(GstElement *webrtcbin)
{
    g_print("timeout to check webrtc connection state\n");
    g_signal_emit_by_name(G_OBJECT(webrtcbin), "notify::ice-connection-state", NULL, NULL);
}

void start_appsrc_webrtcbin(WebrtcItem *item) {
    GError *error = NULL;
    gchar *cmdline = NULL;
    GstBus *bus = NULL;
    // gchar *turn_srv = NULL;

    gchar *webrtc_name = g_strdup_printf("webrtc_appsrc_%ld", item->hash_id);
    // vcaps = gst_caps_from_string("video/x-h264,stream-format=(string)avc,alignment=(string)au,width=(int)1280,height=(int)720,framerate=(fraction)30/1,profile=(string)main");
    // acaps = gst_caps_from_string("audio/x-opus, channels=(int)1,channel-mapping-family=(int)1");
    gchar *audio_src = g_strdup_printf("appsrc name=audio_%ld  format=3 ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " rtpopusdepay ! rtpopuspay ! queue ! "
                                       " application/x-rtp,media=(string)audio,clock-rate=(int)48000,encoding-name=(string)OPUS,payload=(int)97 ! "
                                       " queue ! %s.",
                                       item->hash_id, webrtc_name);
    gchar *video_src = g_strdup_printf("appsrc  name=video_%ld format=3 ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " rtph264depay ! h264parse ! rtph264pay config-interval=-1 ! queue ! "
                                       " application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)H264,payload=(int)96 ! "
                                       " queue ! %s. ",
                                       item->hash_id, webrtc_name);
    // turn_srv = g_strdup_printf("turn://%s:%s@%s", config_data.webrtc.turn.user, config_data.webrtc.turn.pwd, config_data.webrtc.turn.url);
    // cmdline = g_strdup_printf("webrtcbin name=%s turn-server=%s %s %s ", webrtc_name, turn_srv, audio_src, video_src);
    cmdline = g_strdup_printf("webrtcbin name=%s stun-server=%s %s %s ", webrtc_name, config_data.webrtc.stun, audio_src, video_src);

    // g_free(turn_srv);

    g_print("webrtc cmdline: %s \n", cmdline);
    g_free(audio_src);
    g_free(video_src);

    item->sendpipe = gst_parse_launch(cmdline, &error);
    g_free(cmdline);

    item->sendbin = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    g_free(webrtc_name);

    webrtc_name = g_strdup_printf("video_%ld", item->hash_id);
    item->send_avpair.video_src = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    g_free(webrtc_name);

    // g_signal_connect(appsrc_vid, "need-data", (GCallback)need_data, video_sink);

    webrtc_name = g_strdup_printf("audio_%ld", item->hash_id);
    item->send_avpair.audio_src = gst_bin_get_by_name(GST_BIN(item->sendpipe), webrtc_name);
    g_free(webrtc_name);

    // g_signal_conne/ct(appsrc_aid, "need-data", (GCallback)need_data, audio_sink);
    g_mutex_lock(&G_appsrc_lock);
    G_AppsrcList = g_list_append(G_AppsrcList, &item->send_avpair);
    g_mutex_unlock(&G_appsrc_lock);

    bus = gst_pipeline_get_bus(GST_PIPELINE(item->sendpipe));
    gst_bus_add_watch(bus, (GstBusFunc)on_source_message, NULL);
    gst_object_unref(bus);
    gst_element_set_state(item->sendpipe, GST_STATE_READY);
    create_data_channel((gpointer)item);

    item->record.get_rec_state = &get_record_state;
    item->record.start = &appsrc_cmd_rec_start;
    item->record.stop = &appsrc_cmd_rec_stop;
    item->recv.addremote = &start_recv_webrtcbin;
    item->stop_webrtc = &stop_appsrc_webrtc;
    g_signal_connect(item->sendbin, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_gathering_state_notify), NULL);

    g_signal_connect(item->sendbin, "notify::ice-connection-state",
                     G_CALLBACK(on_peer_connection_state_notify), NULL);

    g_signal_connect(item->sendbin, "on-new-transceiver",
                     G_CALLBACK(_on_new_transceiver), item->sendbin);
    g_timeout_add(3 * 1000, (GSourceFunc)check_webrtcbin_state_by_timer, item->sendbin);
}

static GstFlowReturn
on_new_sample_from_sink(GstElement *elt, gpointer user_data) {
    GstSample *sample;
    GstFlowReturn ret;
    gchar *sink_name = gst_element_get_name(elt);
    // g_print("new sample from :%s\n", sink_name);
    gboolean isVideo = g_str_has_prefix(sink_name, "video");
    g_free(sink_name);

    sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
    ret = GST_FLOW_ERROR;
    if (sample) {
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstSegment *seg = gst_sample_get_segment(sample);
        GstClockTime pts, dts;
        ret = GST_FLOW_OK;

        /* Convert the PTS/DTS to running time so they start from 0 */
        pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts))
            pts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, pts);

        dts = GST_BUFFER_DTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(dts))
            dts = gst_segment_to_running_time(seg, GST_FORMAT_TIME, dts);

        if (buffer) {
            /* Make writable so we can adjust the timestamps */
            buffer = gst_buffer_copy(buffer);
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = dts;
            GList *item;

            g_mutex_lock(&G_appsrc_lock);
            for (item = G_AppsrcList; item; item = item->next) {
                AppSrcAVPair *pair = item->data;
                g_signal_emit_by_name(isVideo ? pair->video_src : pair->audio_src, "push-buffer", buffer, &ret);
            }
            g_mutex_unlock(&G_appsrc_lock);

            gst_buffer_unref(buffer);
        }

        /* we don't need the appsink sample anymore */
        gst_sample_unref(sample);
    }
    return ret;
}

int start_av_appsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *aqueue, *vqueue, *video_sink, *audio_sink, *video_pay, *audio_pay, *h264parse;
    GstPad *src_vpad, *src_apad, *sink_vpad, *sink_apad;
    GstPadLinkReturn lret;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(aqueue, "queue");
    MAKE_ELEMENT_AND_ADD(video_pay, "rtph264pay");
    MAKE_ELEMENT_AND_ADD(audio_pay, "rtpopuspay");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    video_sink = gst_element_factory_make("appsink", "video_sink");
    audio_sink = gst_element_factory_make("appsink", "audio_sink");

    /* Configure udpsink */
    g_object_set(video_sink, "sync", FALSE, "async", FALSE, "emit-signals", TRUE, NULL);
    g_object_set(audio_sink, "sync", FALSE, "async", FALSE, "emit-signals", TRUE, NULL);
    g_object_set(video_pay, "config-interval", -1, "aggregate-mode", 1, NULL);
    g_object_set(audio_pay, "pt", 97, NULL);
    g_object_set(vqueue, "max-size-time", 100000000, NULL);

    gst_bin_add_many(GST_BIN(pipeline), video_sink, audio_sink, NULL);

    /* link to upstream. */
    if (!gst_element_link_many(aqueue, audio_pay, audio_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    if (!gst_element_link_many(vqueue, h264parse, video_pay, video_sink, NULL)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");

    if ((lret = gst_pad_link(src_vpad, sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee appsink video sink could not be linked. ret: %d \n", lret);
        return -1;
    }

    gst_object_unref(sink_vpad);
    gst_object_unref(src_vpad);

    sink_apad = gst_element_get_static_pad(aqueue, "sink");
    src_apad = gst_element_request_pad_simple(audio_source, "src_%u");

    if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
        gst_printerrln("Tee appsink audio sink could not be linked, link return :%d .\n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sink_apad));
    gst_object_unref(GST_OBJECT(src_apad));

    g_mutex_init(&G_appsrc_lock);
    g_signal_connect(audio_sink, "new-sample",
                     (GCallback)on_new_sample_from_sink, NULL);
    g_signal_connect(video_sink, "new-sample",
                     (GCallback)on_new_sample_from_sink, NULL);
    return 0;
}

int start_appsink() {
    if (!_check_initial_status())
        return -1;
    GstElement *muxer, *h264parse, *aqueue, *vqueue, *appsink;
    GstPad *src_vpad, *src_apad, *sink_vpad, *sink_apad;
    GstPadLinkReturn lret;
    GstBus *bus = NULL;
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(aqueue, "queue");
    /**
     * @brief matroskamux have a lot of problem.
     *
     * It is normal for the first file to appear in the combination of
     * appsink and appsrc when using matroskamux, but the following files
     * are rawdata of matroska without ebml header, can not play and identify.
     *
     * But I use the mpegtsmux test is no problem.
     */
    MAKE_ELEMENT_AND_ADD(muxer, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(appsink, "appsink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");

    app_data.appsink = appsink;
    app_data.muxer = muxer;

    if (!gst_element_link_many(h264parse, vqueue, muxer, appsink, NULL)) {
        g_error("Failed to link elements to video queue.\n");
        return -1;
    }
    if (!gst_element_link(aqueue, muxer)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    // g_object_set(muxer,
    //              //  "streamable", TRUE,
    //              "fragment-duration", record_time * 1000,
    //              "fragment-mode",1, NULL);

    /* Configure appsink */
    g_object_set(appsink, "sync", FALSE, "emit-signals", TRUE, NULL);

    bus = gst_element_get_bus(appsink);
    gst_bus_add_watch(bus, (GstBusFunc)on_sink_message, &app_data);
    gst_object_unref(bus);

    sink_vpad = gst_element_get_static_pad(h264parse, "sink");
    src_vpad = gst_element_request_pad_simple(h264_encoder, "src_%u");

    if ((lret = gst_pad_link(src_vpad, sink_vpad)) != GST_PAD_LINK_OK) {
        g_error("Tee mkv file video sink could not be linked. ret: %d \n", lret);
        return -1;
    }

    gst_object_unref(sink_vpad);
    gst_object_unref(src_vpad);

    // link second queue to matroskamux, element link not working because the queue default link to the video_%u.
    // src_apad = gst_element_get_static_pad(aqueue, "src");
    // sink_apad = gst_element_request_pad_simple(muxer, "audio_%u");
    // if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
    //     g_error("Tee mkv file audio sink could not be linked. ret: %d \n", lret);
    //     return -1;
    // }

    // gst_object_unref(sink_apad);
    // gst_object_unref(src_apad);

    // add audio to muxer.
    src_apad = gst_element_request_pad_simple(audio_source, "src_%u");
    GST_DEBUG("mkv obtained request pad %s for from audio source.\n", gst_pad_get_name(src_apad));
    sink_apad = gst_element_get_static_pad(aqueue, "sink");
    if ((lret = gst_pad_link(src_apad, sink_apad)) != GST_PAD_LINK_OK) {
        // May be the src and sink are not match the format. ex: aac could not link to matroskamux.
        gst_printerrln("Tee mkv file audio sink could not be linked, link return :%d .\n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sink_apad));
    gst_object_unref(src_apad);
    return 0;
}

int splitfile_sink() {
    if (!_check_initial_status())
        return -1;
    GstElement *splitmuxsink, *h264parse, *vqueue;
    GstPad *src_pad, *queue_pad;
    GstPadLinkReturn lret;
    gchar *tmpfile;

    gchar *outdir = g_strconcat(config_data.root_dir, "/mp4", NULL);
    MAKE_ELEMENT_AND_ADD(splitmuxsink, "splitmuxsink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");

    g_object_set(vqueue, "max-size-time", 100000000, NULL);
    if (!gst_element_link_many(vqueue, h264parse, splitmuxsink, NULL)) {
        g_error("Failed to link elements splitmuxsink.\n");
        return -1;
    }
    tmpfile = g_strconcat(outdir, "/segment-%05d.mp4", NULL);
    g_object_set(splitmuxsink,
                 "async-handling", TRUE,
                 "location",
                 tmpfile,
                 //  "muxer", matroskamux,
                 //  "async-finalize", TRUE, "muxer-factory", "matroskamux",
                 "max-size-time", (guint64)600 * GST_SECOND, // 600000000000,
                 NULL);
    g_free(tmpfile);
    _mkdir(outdir, 0755);
    g_free(outdir);
    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("split file obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(vqueue, "sink");

    if ((lret = gst_pad_link(src_pad, queue_pad)) != GST_PAD_LINK_OK) {
        g_printerr("Split file video sink could not be linked. return: %d \n", lret);
        return -1;
    }
    gst_object_unref(queue_pad);
    gst_object_unref(src_pad);

    // add audio to muxer.
    if (audio_source != NULL) {
        GstPad *tee_pad, *audio_pad;

        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        g_print("split file obtained request pad %s for from audio source.\n", gst_pad_get_name(tee_pad));
        audio_pad = gst_element_request_pad_simple(splitmuxsink, "audio_%u");
        if ((lret = gst_pad_link(tee_pad, audio_pad)) != GST_PAD_LINK_OK) {
            g_printerr("Split file audio sink could not be linked. return: %d .\n", lret);
            return -1;
        }
        gst_object_unref(audio_pad);
        gst_object_unref(tee_pad);
    }

    return 0;
}

static void set_hlssink_object(GstElement *hlssink, gchar *outdir, gchar *location) {
    gchar *tmp1, *tmp2;
    tmp1 = g_strconcat(outdir, location, NULL);
    tmp2 = g_strconcat(outdir, "/playlist.m3u8", NULL);
    g_object_set(hlssink,
                 "max-files", config_data.hls.files,
                 "target-duration", config_data.hls.duration,
                 "location", tmp1,
                 "playlist-location", tmp2,
                 NULL);
    g_free(tmp1);
    g_free(tmp2);
}

int av_hlssink() {
    GstElement *hlssink, *h264parse, *mpegtsmux, *vqueue;
    GstPad *src_pad, *queue_pad;
    GstPadLinkReturn lret;
    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(vqueue, "queue");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (!gst_element_link_many(vqueue, h264parse, mpegtsmux, hlssink, NULL)) {
        g_error("Failed to link elements av hlssink\n");
        return -1;
    }
    g_object_set(vqueue, "max-size-time", 100000000, NULL);
    set_hlssink_object(hlssink, outdir, "/segment%05d.ts");

    _mkdir(outdir, 0755);
    g_free(outdir);
    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    g_print("av obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(vqueue, "sink");
    if ((lret = gst_pad_link(src_pad, queue_pad)) != GST_PAD_LINK_OK) {
        g_error("Tee av hls audio sink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);

    // add audio to muxer.
    if (audio_source != NULL) {
        GstPad *tee_pad;
        GstElement *aqueue, *opusparse;

        MAKE_ELEMENT_AND_ADD(aqueue, "queue");
        MAKE_ELEMENT_AND_ADD(opusparse, "opusparse");

        if (!gst_element_link_many(aqueue, opusparse, mpegtsmux, NULL)) {
            g_error("Failed to link elements audio to mpegtsmux.\n");
            return -1;
        }

        tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");
        GST_DEBUG("av hlssink audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));
        queue_pad = gst_element_get_static_pad(aqueue, "sink");
        if ((lret = gst_pad_link(tee_pad, queue_pad)) != GST_PAD_LINK_OK) {
            g_error("Av hls audio sink could not be linked. return: %d \n", lret);
            return -1;
        }
        gst_object_unref(queue_pad);
        gst_object_unref(tee_pad);
    }
    return 0;
}

int udp_multicastsink() {
    GstElement *udpsink, *rtpmp2tpay, *vqueue, *mpegtsmux, *bin;
    GstPad *src_pad, *sub_sink_vpad;

    GstPad *tee_pad, *sub_sink_apad;
    GstElement *aqueue;
    GstPadLinkReturn lret;
    if (!_check_initial_status())
        return -1;
    bin = gst_bin_new("udp_bin");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, udpsink, "udpsink");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, rtpmp2tpay, "rtpmp2tpay");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, vqueue, "queue");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, mpegtsmux, "mpegtsmux");
    SUB_BIN_MAKE_ELEMENT_AND_ADD(bin, aqueue, "queue");

    if (!gst_element_link_many(vqueue, mpegtsmux, rtpmp2tpay, udpsink, NULL)) {
        g_error("Failed to link elements udpsink\n");
        return -1;
    }
    g_object_set(udpsink,
                 "sync", FALSE, "async", FALSE,
                 "host", config_data.udp.host,
                 "port", config_data.udp.port,
                 "auto-multicast", config_data.udp.multicast, NULL);

    g_object_set(mpegtsmux, "alignment", 7, NULL);
    g_object_set(vqueue, "max-size-time", 100000000, NULL);

    // create ghost pads for sub bin.
    sub_sink_vpad = gst_element_get_static_pad(vqueue, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("videosink", sub_sink_vpad));
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    // set the new bin to PAUSE to preroll
    gst_element_set_state(bin, GST_STATE_PAUSED);
    // gst_element_set_locked_state(udpsink, TRUE);

    src_pad = gst_element_request_pad_simple(h264_encoder, "src_%u");
    GST_DEBUG("udp obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    sub_sink_vpad = gst_element_get_static_pad(bin, "videosink");

    if (!gst_element_link(aqueue, mpegtsmux)) {
        g_error("Failed to link elements audio to mpegtsmux.\n");
        return -1;
    }

    tee_pad = gst_element_request_pad_simple(audio_source, "src_%u");

    GST_DEBUG("udp sink audio obtained request pad %s for from h264 source.\n", gst_pad_get_name(tee_pad));

    sub_sink_apad = gst_element_get_static_pad(aqueue, "sink");
    gst_element_add_pad(bin, gst_ghost_pad_new("audiosink", sub_sink_apad));
    gst_object_unref(GST_OBJECT(sub_sink_apad));

    sub_sink_apad = gst_element_get_static_pad(bin, "audiosink");

    gst_bin_add(GST_BIN(pipeline), bin);
    if (gst_pad_link(src_pad, sub_sink_vpad) != GST_PAD_LINK_OK) {
        g_error("Tee udp hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(GST_OBJECT(sub_sink_vpad));

    if ((lret = gst_pad_link(tee_pad, sub_sink_apad)) != GST_PAD_LINK_OK) {
        g_error("Tee udp sink could not be linked. ret: %d \n", lret);
        return -1;
    }
    gst_object_unref(GST_OBJECT(sub_sink_apad));
    gst_object_unref(GST_OBJECT(tee_pad));

    return 0;
}

int motion_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *motioncells, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;
    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/motion", NULL);
    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(motioncells, "motioncells");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                                   textoverlay, encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! motioncells postallmotion=true ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(pre_convert, motioncells, post_convert,
                                   encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    set_hlssink_object(hlssink, outdir, "/segment%05d.ts");
    gchar *tmp2;
    tmp2 = g_strconcat(outdir, "/motioncells", NULL);
    g_object_set(motioncells,
                 // "postallmotion", TRUE,
                 "datafile", tmp2,
                 NULL);
    g_free(tmp2);
    _mkdir(outdir, 0755);
    g_free(outdir);
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("motion obtained request pad %s for source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee motion hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    gst_object_unref(src_pad);
    return 0;
}

int cvtracker_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *cvtracker, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;
    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/cvtracker", NULL);
    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(cvtracker, "cvtracker");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, cvtracker, post_convert,
                                   textoverlay, encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements cvtracker sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! cvtracker ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(pre_convert, cvtracker, post_convert,
                                   encoder, queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }

    set_hlssink_object(hlssink, outdir, "/cvtracker-%05d.ts");
    _mkdir(outdir, 0755);
    g_free(outdir);
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("cvtracker obtained request pad %s for source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee cvtracker hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    gst_object_unref(src_pad);
    return 0;
}

int facedetect_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert;
    GstElement *queue, *post_queue, *facedetect, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;

    if (!_check_initial_status())
        return -1;
    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/face", NULL);

    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(queue, "queue");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(facedetect, "facedetect");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");

    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                                   textoverlay, encoder, post_queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements facedetect sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "queue ! videoconvert ! facedetect min-stddev=24 scale-factor=2.8 ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);
    } else {
        if (!gst_element_link_many(queue, pre_convert, facedetect, post_convert,
                                   encoder, post_queue, h264parse,
                                   hlssink, NULL)) {
            g_error("Failed to link elements facedetect sink.\n");
            return -1;
        }
    }
    set_hlssink_object(hlssink, outdir, "/face-%05d.ts");

    g_object_set(facedetect, "min-stddev", 24, "scale-factor", 2.8,
                 "eyes-profile", "/usr/share/opencv4/haarcascades/haarcascade_eye_tree_eyeglasses.xml", NULL);

    _mkdir(outdir, 0755);
    g_free(outdir);
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("face obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee av hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    gst_object_unref(src_pad);
    return 0;
}

int edgedect_hlssink() {
    GstElement *hlssink, *h264parse, *pre_convert, *post_convert, *clock;
    GstElement *post_queue, *edgedetect, *encoder, *mpegtsmux;
    GstPad *src_pad, *queue_pad;

    if (!_check_initial_status())
        return -1;

    gchar *outdir = g_strconcat(config_data.root_dir, "/hls/edge", NULL);
    MAKE_ELEMENT_AND_ADD(hlssink, "hlssink");
    MAKE_ELEMENT_AND_ADD(h264parse, "h264parse");
    MAKE_ELEMENT_AND_ADD(post_queue, "queue");
    MAKE_ELEMENT_AND_ADD(pre_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(post_convert, "videoconvert");
    MAKE_ELEMENT_AND_ADD(edgedetect, "edgedetect");
    MAKE_ELEMENT_AND_ADD(mpegtsmux, "mpegtsmux");
    MAKE_ELEMENT_AND_ADD(clock, "clockoverlay");
    g_object_set(clock, "time-format", "%D %H:%M:%S", NULL);

    if (gst_element_factory_find("vaapih264enc")) {
        MAKE_ELEMENT_AND_ADD(encoder, "vaapih264enc");
    } else {
        MAKE_ELEMENT_AND_ADD(encoder, "x264enc");
    }

    if (config_data.hls.showtext) {
        GstElement *textoverlay;
        MAKE_ELEMENT_AND_ADD(textoverlay, "textoverlay");
        if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                                   textoverlay, clock, encoder, post_queue, h264parse,
                                   mpegtsmux, hlssink, NULL)) {
            g_error("Failed to link elements cvtracker sink.\n");
            return -1;
        }
        g_object_set(textoverlay, "text", "videoconvert ! edgedetect threshold1=80 threshold2=240  ! videoconvert",
                     "valignment", 1, // bottom
                     "halignment", 0, // left
                     NULL);

    } else {
        if (!gst_element_link_many(pre_convert, edgedetect, post_convert,
                                   clock, encoder, post_queue, h264parse,
                                   hlssink, NULL)) {
            g_error("Failed to link elements motion sink.\n");
            return -1;
        }
    }
    set_hlssink_object(hlssink, outdir, "/edge-%05d.ts");
    g_object_set(edgedetect, "threshold1", 80, "threshold2", 240, NULL);

    _mkdir(outdir, 0755);
    g_free(outdir);
    src_pad = gst_element_request_pad_simple(va_pp, "src_%u");
    g_print("edge obtained request pad %s for from h264 source.\n", gst_pad_get_name(src_pad));
    queue_pad = gst_element_get_static_pad(pre_convert, "sink");
    if (gst_pad_link(src_pad, queue_pad) != GST_PAD_LINK_OK) {
        g_error("Tee edgedect_hlssink could not be linked.\n");
        return -1;
    }
    gst_object_unref(queue_pad);
    gst_object_unref(src_pad);
    return 0;
}

static void _initial_device() {
    if (is_initial)
        return;
    _mkdir(config_data.root_dir, 0755);
    record_time = config_data.rec_len;

    if (config_data.showdot) {
        gchar *dotdir = g_strconcat(config_data.root_dir, "/dot", NULL);
        _mkdir(dotdir, 0755);
        // https://gstreamer.freedesktop.org/documentation/gstreamer/running.html?gi-language=c
        g_setenv("GST_DEBUG_DUMP_DOT_DIR", dotdir, TRUE);
        g_free(dotdir);
    }

    video_source = video_src();
    if (video_source == NULL) {
        g_printerr("unable to open video device.\n");
        return;
    }

    h264_encoder = encoder_h264();
    if (h264_encoder == NULL) {
        g_printerr("unable to open h264 encoder.\n");
        return;
    }

    audio_source = audio_src();
    if (audio_source == NULL) {
        g_printerr("unable to open audio device.\n");
    }

    va_pp = vaapi_postproc();
    if (va_pp == NULL) {
        g_printerr("unable to open vaapi post proc.\n");
    }

    // mkv_mux = get_mkv_mux();
    is_initial = TRUE;
}

GThread *start_inotify_thread(void) {
    GThread *tid = NULL;
    gchar *fullpath;
    char abpath[PATH_MAX];

    fullpath = g_strconcat(config_data.root_dir, "/hls/motion/motioncells-0.vamc", NULL);
    g_print("Starting inotify watch thread....\n");
    if (!realpath(fullpath, abpath)) {
        g_printerr("Get realpath of %s failed\n", fullpath);
        return NULL;
    }
    g_free(fullpath);
    tid = g_thread_new("_inotify_thread", _inotify_thread, g_strdup(abpath));

    return tid;
}

GstElement *create_instance() {
    pipeline = gst_pipeline_new("pipeline");
    if (!is_initial)
        _initial_device();

    if (config_data.udp.enable)
        udp_multicastsink();

    if (config_data.splitfile_sink)
        splitfile_sink();

    if (config_data.hls_onoff.av_hlssink)
        av_hlssink();

    if (config_data.hls_onoff.edge_hlssink)
        edgedect_hlssink();

    if (config_data.hls_onoff.facedetect_hlssink)
        facedetect_hlssink();

    if (config_data.hls_onoff.motion_hlssink) {
        motion_hlssink();
    }

    if (config_data.app_sink) {
        // start_appsink();
        start_av_appsink();
    }

    if (config_data.webrtc.enable) {
        start_av_udpsink();
    }

    return pipeline;
}
