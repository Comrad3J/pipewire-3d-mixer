#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <pipewire/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct state {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_stream *stream;
    struct pw_proxy *filter_proxy;

    struct spa_hook registry_listener;
    struct spa_hook stream_listener;

    uint32_t filter_node_id;
    uint32_t filter_in_gid[8];
    uint32_t stream_node_id;
    uint32_t stream_out_gid[2];

    int source_idx;
    float azimuth;

    uint32_t rate;
    uint32_t channels;
    float freq;
    float amplitude;
    double phase;
    double phase_inc;

    int64_t frames_left;
    int64_t tail_frames;
    bool finishing;
    bool links_created;
    bool azimuth_set;
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -s <source 1-4> -a <azimuth deg> [options]\n"
        "Options:\n"
        "  -s <n>     Source index (1-4 or 0-3)\n"
        "  -a <deg>   Azimuth in degrees (0-360)\n"
        "  -d <sec>   Duration in seconds (default 0.5)\n"
        "  -f <Hz>    Frequency in Hz (default 1000)\n"
        "  -r <Hz>    Sample rate (default 48000)\n"
        "  -g <amp>   Amplitude 0..1 (default 0.2)\n"
        "  -h         Show this help\n",
        prog);
}

static float mirror_azimuth(float az)
{
    float m = 360.0f - az;
    if (m >= 360.0f) m -= 360.0f;
    if (m < 0.0f) m += 360.0f;
    return m;
}

static void set_param(struct state *st, const char *name, float value)
{
    if (!st->filter_proxy) return;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame f, f_struct;

    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&b, &f_struct);
    spa_pod_builder_string(&b, name);
    spa_pod_builder_float(&b, value);
    spa_pod_builder_pop(&b, &f_struct);
    spa_pod_builder_pop(&b, &f);

    struct spa_pod *pod = spa_pod_builder_deref(&b, 0);
    pw_node_set_param((struct pw_node*)st->filter_proxy, SPA_PARAM_Props, 0, pod);
}

static void set_source_azimuth(struct state *st)
{
    static const char *left_names[]  = {"spk1", "spk3", "spk5", "spk7"};
    static const char *right_names[] = {"spk2", "spk4", "spk6", "spk8"};

    if (st->source_idx < 0 || st->source_idx >= 4)
        return;

    float az = mirror_azimuth(st->azimuth);

    char name[64];
    snprintf(name, sizeof(name), "%s:Azimuth", left_names[st->source_idx]);
    set_param(st, name, az);

    snprintf(name, sizeof(name), "%s:Azimuth", right_names[st->source_idx]);
    set_param(st, name, az);

    st->azimuth_set = true;
}

static struct pw_proxy *create_link(struct state *st, uint32_t out_port_gid, uint32_t in_port_gid)
{
    char out_port_str[16], in_port_str[16];
    snprintf(out_port_str, sizeof(out_port_str), "%u", out_port_gid);
    snprintf(in_port_str,  sizeof(in_port_str),  "%u", in_port_gid);

    const struct spa_dict_item items[] = {
        { PW_KEY_LINK_OUTPUT_PORT, out_port_str },
        { PW_KEY_LINK_INPUT_PORT,  in_port_str  },
    };
    struct spa_dict dict = SPA_DICT_INIT(items, (uint32_t)(sizeof(items)/sizeof(items[0])));

    return pw_core_create_object(st->core,
                                 "link-factory",
                                 PW_TYPE_INTERFACE_Link,
                                 PW_VERSION_LINK,
                                 &dict,
                                 0);
}

static void try_link(struct state *st)
{
    if (st->links_created)
        return;

    int base = st->source_idx * 2;
    if (base < 0 || base + 1 >= 8)
        return;

    if (st->filter_in_gid[base] == 0 || st->filter_in_gid[base + 1] == 0)
        return;

    if (st->stream_out_gid[0] == 0 || st->stream_out_gid[1] == 0)
        return;

    create_link(st, st->stream_out_gid[0], st->filter_in_gid[base]);
    create_link(st, st->stream_out_gid[1], st->filter_in_gid[base + 1]);
    st->links_created = true;
}

static void on_process(void *data)
{
    struct state *st = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(st->stream);
    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    if (buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(st->stream, b);
        return;
    }

    float *dst = buf->datas[0].data;
    uint32_t max_samples = buf->datas[0].maxsize / sizeof(float);
    uint32_t n_frames = max_samples / st->channels;
    uint32_t frames_to_tone = 0;

    if (st->frames_left > 0) {
        frames_to_tone = (uint32_t)((st->frames_left < (int64_t)n_frames) ? st->frames_left : (int64_t)n_frames);
    }

    for (uint32_t i = 0; i < frames_to_tone; i++) {
        float s = sinf((float)st->phase) * st->amplitude;
        for (uint32_t c = 0; c < st->channels; c++) {
            dst[i * st->channels + c] = s;
        }
        st->phase += st->phase_inc;
        if (st->phase >= 2.0 * M_PI)
            st->phase -= 2.0 * M_PI;
    }

    if (frames_to_tone < n_frames) {
        memset(dst + frames_to_tone * st->channels, 0,
               (n_frames - frames_to_tone) * st->channels * sizeof(float));
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = st->channels * sizeof(float);
    buf->datas[0].chunk->size = n_frames * st->channels * sizeof(float);

    pw_stream_queue_buffer(st->stream, b);

    if (st->frames_left > 0) {
        st->frames_left -= (int64_t)frames_to_tone;
        if (st->frames_left == 0) {
            st->finishing = true;
            st->tail_frames = (int64_t)st->rate / 10; /* 100ms tail */
        }
    }

    if (st->finishing) {
        if (st->tail_frames <= (int64_t)n_frames) {
            st->tail_frames = 0;
            pw_main_loop_quit(st->loop);
        } else {
            st->tail_frames -= (int64_t)n_frames;
        }
    }
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
    struct state *st = data;
    (void)old;

    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "stream error: %s\n", error ? error : "unknown");
        pw_main_loop_quit(st->loop);
        return;
    }

    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        if (st->stream_node_id == 0) {
            st->stream_node_id = pw_stream_get_node_id(st->stream);
        }
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_process,
};

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
                                  const char *type, uint32_t version,
                                  const struct spa_dict *props)
{
    (void)permissions;
    (void)version;

    struct state *st = data;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (node_name && strcmp(node_name, "effect_input.multi_spatial") == 0) {
            st->filter_node_id = id;
            st->filter_proxy = pw_registry_bind(st->registry,
                                                id,
                                                PW_TYPE_INTERFACE_Node,
                                                PW_VERSION_NODE,
                                                0);
            if (!st->azimuth_set)
                set_source_azimuth(st);
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char *node_id_s = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *dir_s     = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char *port_id_s = spa_dict_lookup(props, PW_KEY_PORT_ID);

        if (!node_id_s || !dir_s || !port_id_s)
            return;

        uint32_t node_id = (uint32_t)atoi(node_id_s);
        int port_id = atoi(port_id_s);

        if (node_id == st->filter_node_id && strcmp(dir_s, "in") == 0) {
            if (port_id >= 0 && port_id < 8)
                st->filter_in_gid[port_id] = id;
            try_link(st);
            return;
        }

        if (node_id == st->stream_node_id && strcmp(dir_s, "out") == 0) {
            if (port_id >= 0 && port_id < 2)
                st->stream_out_gid[port_id] = id;
            try_link(st);
            return;
        }
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
};

int main(int argc, char *argv[])
{
    struct state st;
    memset(&st, 0, sizeof(st));

    st.source_idx = -1;
    st.azimuth = 0.0f;
    st.rate = 48000;
    st.channels = 2;
    st.freq = 1000.0f;
    st.amplitude = 0.2f;
    double duration = 0.5;

    int opt;
    while ((opt = getopt(argc, argv, "s:a:d:f:r:g:h")) != -1) {
        switch (opt) {
        case 's':
            st.source_idx = atoi(optarg);
            break;
        case 'a':
            st.azimuth = (float)strtod(optarg, NULL);
            break;
        case 'd':
            duration = strtod(optarg, NULL);
            break;
        case 'f':
            st.freq = (float)strtod(optarg, NULL);
            break;
        case 'r':
            st.rate = (uint32_t)atoi(optarg);
            break;
        case 'g':
            st.amplitude = (float)strtod(optarg, NULL);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (st.source_idx >= 1 && st.source_idx <= 4) {
        st.source_idx -= 1;
    } else if (st.source_idx >= 0 && st.source_idx <= 3) {
        /* already zero-based */
    } else {
        fprintf(stderr, "Invalid source index. Use 1-4 or 0-3.\n");
        usage(argv[0]);
        return 1;
    }

    if (duration <= 0.0) {
        fprintf(stderr, "Duration must be > 0.\n");
        return 1;
    }

    while (st.azimuth < 0.0f) st.azimuth += 360.0f;
    while (st.azimuth >= 360.0f) st.azimuth -= 360.0f;

    st.phase = 0.0;
    st.phase_inc = 2.0 * M_PI * ((double)st.freq / (double)st.rate);
    st.frames_left = (int64_t)(duration * (double)st.rate);

    pw_init(&argc, &argv);

    st.loop = pw_main_loop_new(NULL);
    st.context = pw_context_new(pw_main_loop_get_loop(st.loop), NULL, 0);
    st.core = pw_context_connect(st.context, NULL, 0);
    if (!st.core) {
        fprintf(stderr, "Failed to connect to PipeWire core.\n");
        return 1;
    }

    st.registry = pw_core_get_registry(st.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(st.registry, &st.registry_listener, &registry_events, &st);

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Notification",
        PW_KEY_NODE_NAME, "spatial_notification",
        PW_KEY_NODE_DESCRIPTION, "Spatial Notification",
        NULL);

    st.stream = pw_stream_new(st.core, "spatial_notification", props);
    pw_stream_add_listener(st.stream, &st.stream_listener, &stream_events, &st);

    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = st.rate,
        .channels = st.channels,
        .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
    };

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(st.stream,
                                PW_DIRECTION_OUTPUT,
                                PW_ID_ANY,
                                PW_STREAM_FLAG_MAP_BUFFERS,
                                params,
                                1);
    if (res < 0) {
        fprintf(stderr, "pw_stream_connect failed: %s\n", spa_strerror(res));
        return 1;
    }

    pw_main_loop_run(st.loop);

    if (st.stream)
        pw_stream_destroy(st.stream);
    if (st.registry)
        pw_proxy_destroy((struct pw_proxy*)st.registry);
    if (st.core)
        pw_core_disconnect(st.core);
    if (st.context)
        pw_context_destroy(st.context);
    if (st.loop)
        pw_main_loop_destroy(st.loop);

    pw_deinit();

    return 0;
}
