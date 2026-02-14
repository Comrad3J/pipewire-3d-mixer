#define _GNU_SOURCE
#include <pipewire/pipewire.h>
#include <pipewire/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct port_info {
    uint32_t global_id;
    uint32_t node_id;
    int direction; /* 0=in, 1=out */
    int port_id;
};

struct link_info {
    uint32_t link_id;
    uint32_t out_port_gid;
    uint32_t in_port_gid;
};

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
    uint32_t target_filter_in_gid;
    uint32_t stream_node_id;
    uint32_t stream_out_gid;
    uint32_t own_link_id;

    struct port_info *ports;
    size_t ports_len;
    size_t ports_cap;

    struct link_info *links;
    size_t links_len;
    size_t links_cap;

    int channel_idx; /* 0..7 */
    float azimuth;
    float elevation;
    float gain;

    uint32_t rate;
    float *pcm;
    size_t pcm_frames;
    size_t pcm_cursor;
    int64_t tail_frames;
    int64_t wait_link_frames;

    bool link_requested;
    bool link_created;
    bool position_set;
    bool failed;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --file <path.mp3> --channel <1-8> --azimuth <deg> [options]\n"
            "Options:\n"
            "  -m, --file <path>       Audio file to play (MP3 or anything ffmpeg decodes)\n"
            "  -c, --channel <1-8>     Spatial channel (spk1..spk8)\n"
            "  -a, --azimuth <deg>     Azimuth in degrees (0..360)\n"
            "  -e, --elevation <deg>   Elevation in degrees (-90..90), default 0\n"
            "  -g, --gain <amp>        Output gain multiplier, default 1.0\n"
            "  -r, --rate <Hz>         Output sample rate, default 48000\n"
            "  -h, --help              Show this help\n",
            prog);
}

static float mirror_azimuth(float az)
{
    float m = 360.0f - az;
    if (m >= 360.0f) m -= 360.0f;
    if (m < 0.0f) m += 360.0f;
    return m;
}

static void upsert_port(struct state *st, const struct port_info *pi)
{
    for (size_t i = 0; i < st->ports_len; i++) {
        if (st->ports[i].global_id == pi->global_id) {
            st->ports[i] = *pi;
            return;
        }
    }

    if (st->ports_len == st->ports_cap) {
        size_t new_cap = (st->ports_cap == 0) ? 64 : st->ports_cap * 2;
        struct port_info *tmp = realloc(st->ports, new_cap * sizeof(*tmp));
        if (!tmp) {
            fprintf(stderr, "Out of memory while growing port table\n");
            st->failed = true;
            pw_main_loop_quit(st->loop);
            return;
        }
        st->ports = tmp;
        st->ports_cap = new_cap;
    }

    st->ports[st->ports_len++] = *pi;
}

static const struct port_info *find_port(const struct state *st, uint32_t global_id)
{
    for (size_t i = 0; i < st->ports_len; i++) {
        if (st->ports[i].global_id == global_id)
            return &st->ports[i];
    }
    return NULL;
}

static void remove_port(struct state *st, uint32_t global_id)
{
    for (size_t i = 0; i < st->ports_len; i++) {
        if (st->ports[i].global_id == global_id) {
            st->ports[i] = st->ports[st->ports_len - 1];
            st->ports_len--;
            return;
        }
    }
}

static void upsert_link(struct state *st, const struct link_info *li)
{
    for (size_t i = 0; i < st->links_len; i++) {
        if (st->links[i].link_id == li->link_id) {
            st->links[i] = *li;
            return;
        }
    }

    if (st->links_len == st->links_cap) {
        size_t new_cap = (st->links_cap == 0) ? 64 : st->links_cap * 2;
        struct link_info *tmp = realloc(st->links, new_cap * sizeof(*tmp));
        if (!tmp) {
            fprintf(stderr, "Out of memory while growing link table\n");
            st->failed = true;
            pw_main_loop_quit(st->loop);
            return;
        }
        st->links = tmp;
        st->links_cap = new_cap;
    }

    st->links[st->links_len++] = *li;
}

static void remove_link(struct state *st, uint32_t link_id)
{
    for (size_t i = 0; i < st->links_len; i++) {
        if (st->links[i].link_id == link_id) {
            st->links[i] = st->links[st->links_len - 1];
            st->links_len--;
            return;
        }
    }
}

static const struct link_info *find_link(const struct state *st, uint32_t link_id)
{
    for (size_t i = 0; i < st->links_len; i++) {
        if (st->links[i].link_id == link_id) {
            return &st->links[i];
        }
    }
    return NULL;
}

static int destroy_link(struct state *st, uint32_t link_id)
{
    if (!st->registry)
        return -ENOENT;

    int res = pw_registry_destroy(st->registry, link_id);
    if (res < 0) {
        fprintf(stderr, "pw_registry_destroy(%u) failed: %s\n", link_id, spa_strerror(res));
    }
    return res;
}

static void set_param(struct state *st, const char *name, float value)
{
    if (!st->filter_proxy)
        return;

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
    pw_node_set_param((struct pw_node *)st->filter_proxy, SPA_PARAM_Props, 0, pod);
}

static void set_channel_position(struct state *st)
{
    if (!st->filter_proxy || st->channel_idx < 0 || st->channel_idx >= 8)
        return;

    char speaker[16];
    snprintf(speaker, sizeof(speaker), "spk%d", st->channel_idx + 1);

    float az = mirror_azimuth(st->azimuth);
    float el = st->elevation;

    char name[64];
    snprintf(name, sizeof(name), "%s:Azimuth", speaker);
    set_param(st, name, az);

    snprintf(name, sizeof(name), "%s:Elevation", speaker);
    set_param(st, name, el);

    fprintf(stderr, "Set %s position: azimuth=%.1f elevation=%.1f\n", speaker, az, el);
    st->position_set = true;
}

static struct pw_proxy *create_link(struct state *st, uint32_t out_port_gid, uint32_t in_port_gid)
{
    char out_port_str[16];
    char in_port_str[16];
    snprintf(out_port_str, sizeof(out_port_str), "%u", out_port_gid);
    snprintf(in_port_str, sizeof(in_port_str), "%u", in_port_gid);

    const struct spa_dict_item items[] = {
        { PW_KEY_LINK_OUTPUT_PORT, out_port_str },
        { PW_KEY_LINK_INPUT_PORT, in_port_str },
    };
    struct spa_dict dict = SPA_DICT_INIT(items, (uint32_t)(sizeof(items) / sizeof(items[0])));

    return pw_core_create_object(st->core,
                                 "link-factory",
                                 PW_TYPE_INTERFACE_Link,
                                 PW_VERSION_LINK,
                                 &dict,
                                 0);
}

static void destroy_conflicting_links(struct state *st)
{
    if (st->target_filter_in_gid == 0)
        return;

    for (size_t i = 0; i < st->links_len; i++) {
        const struct link_info *li = &st->links[i];
        if (li->in_port_gid != st->target_filter_in_gid)
            continue;

        const struct port_info *out_pi = find_port(st, li->out_port_gid);
        if (out_pi && out_pi->node_id == st->stream_node_id)
            continue;

        fprintf(stderr, "Detaching existing link %u from target input\n", li->link_id);
        destroy_link(st, li->link_id);
    }
}

static void try_link(struct state *st)
{
    if (st->failed || st->link_requested)
        return;

    if (st->target_filter_in_gid == 0)
        return;

    if (st->stream_out_gid == 0)
        return;

    destroy_conflicting_links(st);

    struct pw_proxy *proxy = create_link(st, st->stream_out_gid, st->target_filter_in_gid);
    if (!proxy) {
        fprintf(stderr, "Failed to create link stream_out=%u -> input=%u\n",
                st->stream_out_gid, st->target_filter_in_gid);
        st->failed = true;
        pw_main_loop_quit(st->loop);
        return;
    }

    st->link_requested = true;
}

static bool decode_audio_with_ffmpeg(struct state *st, const char *path)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        char rate_str[16];
        snprintf(rate_str, sizeof(rate_str), "%u", st->rate);

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        (void)dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        char *const argv[] = {
            "ffmpeg", "-nostdin", "-v", "error",
            "-i", (char *)path,
            "-f", "f32le",
            "-ac", "1",
            "-ar", rate_str,
            "pipe:1",
            NULL
        };
        execvp("ffmpeg", argv);
        _exit(127);
    }

    close(pipefd[1]);

    size_t cap = 64 * 1024;
    uint8_t *bytes = malloc(cap);
    if (!bytes) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        fprintf(stderr, "Out of memory while decoding\n");
        return false;
    }

    size_t len = 0;
    uint8_t tmp[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], tmp, sizeof(tmp));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            free(bytes);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            return false;
        }

        if (len + (size_t)n > cap) {
            size_t new_cap = cap;
            while (len + (size_t)n > new_cap) {
                new_cap *= 2;
            }
            uint8_t *grown = realloc(bytes, new_cap);
            if (!grown) {
                fprintf(stderr, "Out of memory while growing decode buffer\n");
                free(bytes);
                close(pipefd[0]);
                waitpid(pid, NULL, 0);
                return false;
            }
            bytes = grown;
            cap = new_cap;
        }

        memcpy(bytes + len, tmp, (size_t)n);
        len += (size_t)n;
    }

    close(pipefd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        free(bytes);
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ffmpeg decode failed (is ffmpeg installed?)\n");
        free(bytes);
        return false;
    }

    len -= (len % sizeof(float));
    if (len == 0) {
        fprintf(stderr, "Decoded audio is empty\n");
        free(bytes);
        return false;
    }

    size_t sample_count = len / sizeof(float);
    float *pcm = (float *)bytes;

    if (fabsf(st->gain - 1.0f) > 0.0001f) {
        for (size_t i = 0; i < sample_count; i++) {
            float v = pcm[i] * st->gain;
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = v;
        }
    }

    st->pcm = pcm;
    st->pcm_frames = sample_count;
    st->pcm_cursor = 0;
    return true;
}

static void on_process(void *data)
{
    struct state *st = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(st->stream);
    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    if (!buf->datas[0].data) {
        pw_stream_queue_buffer(st->stream, b);
        return;
    }

    float *dst = buf->datas[0].data;
    uint32_t n_frames = (uint32_t)(buf->datas[0].maxsize / sizeof(float));

    if (!st->link_created) {
        memset(dst, 0, n_frames * sizeof(float));
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = sizeof(float);
        buf->datas[0].chunk->size = n_frames * sizeof(float);
        pw_stream_queue_buffer(st->stream, b);

        if (st->wait_link_frames > 0) {
            st->wait_link_frames -= (int64_t)n_frames;
            if (st->wait_link_frames <= 0) {
                fprintf(stderr, "Timed out waiting for link to channel %d\n", st->channel_idx + 1);
                st->failed = true;
                pw_main_loop_quit(st->loop);
            }
        }
        return;
    }

    size_t remaining = (st->pcm_cursor < st->pcm_frames) ? (st->pcm_frames - st->pcm_cursor) : 0;
    uint32_t to_copy = (remaining < n_frames) ? (uint32_t)remaining : n_frames;

    if (to_copy > 0) {
        memcpy(dst, st->pcm + st->pcm_cursor, to_copy * sizeof(float));
        st->pcm_cursor += to_copy;
    }

    if (to_copy < n_frames) {
        memset(dst + to_copy, 0, (n_frames - to_copy) * sizeof(float));
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float);
    buf->datas[0].chunk->size = n_frames * sizeof(float);
    pw_stream_queue_buffer(st->stream, b);

    if (st->pcm_cursor >= st->pcm_frames && !st->tail_frames) {
        st->tail_frames = (int64_t)st->rate / 10; /* 100 ms tail */
    }

    if (st->tail_frames > 0) {
        st->tail_frames -= (int64_t)n_frames;
        if (st->tail_frames <= 0) {
            pw_main_loop_quit(st->loop);
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
        st->failed = true;
        pw_main_loop_quit(st->loop);
        return;
    }

    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        if (st->stream_node_id == 0) {
            st->stream_node_id = pw_stream_get_node_id(st->stream);
            try_link(st);
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
            if (!st->position_set) {
                set_channel_position(st);
            }
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char *node_id_s = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *dir_s = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char *port_id_s = spa_dict_lookup(props, PW_KEY_PORT_ID);

        if (!node_id_s || !dir_s || !port_id_s)
            return;

        struct port_info pi;
        pi.global_id = id;
        pi.node_id = (uint32_t)atoi(node_id_s);
        pi.direction = (strcmp(dir_s, "in") == 0) ? 0 : 1;
        pi.port_id = atoi(port_id_s);
        upsert_port(st, &pi);

        if (pi.node_id == st->filter_node_id && pi.direction == 0 &&
            pi.port_id >= 0 && pi.port_id < 8) {
            st->filter_in_gid[pi.port_id] = pi.global_id;
            if (pi.port_id == st->channel_idx) {
                st->target_filter_in_gid = pi.global_id;
            }
        }

        if (pi.node_id == st->stream_node_id && pi.direction == 1 && pi.port_id == 0) {
            st->stream_out_gid = pi.global_id;
        }

        try_link(st);
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char *out_port_s = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT);
        const char *in_port_s = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT);
        if (!out_port_s || !in_port_s)
            return;

        struct link_info li;
        li.link_id = id;
        li.out_port_gid = (uint32_t)atoi(out_port_s);
        li.in_port_gid = (uint32_t)atoi(in_port_s);
        upsert_link(st, &li);

        if (st->target_filter_in_gid != 0 && li.in_port_gid == st->target_filter_in_gid) {
            const struct port_info *out_pi = find_port(st, li.out_port_gid);
            bool from_our_stream = out_pi && out_pi->node_id == st->stream_node_id;
            if (from_our_stream) {
                st->own_link_id = li.link_id;
                st->link_created = true;
            } else {
                fprintf(stderr, "Replacing existing channel link id=%u\n", li.link_id);
                destroy_link(st, li.link_id);
            }
        }

        return;
    }
}

static void registry_event_global_remove(void *data, uint32_t id)
{
    struct state *st = data;
    bool removed_target_link = false;
    const struct link_info *old_link = find_link(st, id);
    if (old_link && old_link->in_port_gid == st->target_filter_in_gid) {
        removed_target_link = true;
    }

    if (id == st->own_link_id) {
        st->own_link_id = 0;
        st->link_created = false;
        st->link_requested = false;
        try_link(st);
    }

    remove_link(st, id);
    remove_port(st, id);

    if (id == st->filter_node_id) {
        fprintf(stderr, "Spatializer node disappeared\n");
        st->failed = true;
        pw_main_loop_quit(st->loop);
    }

    if (removed_target_link && !st->link_created) {
        st->link_requested = false;
        try_link(st);
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

int main(int argc, char *argv[])
{
    struct state st;
    memset(&st, 0, sizeof(st));
    st.channel_idx = -1;
    st.azimuth = 0.0f;
    st.elevation = 0.0f;
    st.gain = 1.0f;
    st.rate = 48000;
    st.wait_link_frames = (int64_t)st.rate * 3; /* 3s */

    const char *file_path = NULL;

    static const struct option long_opts[] = {
        { "file", required_argument, NULL, 'm' },
        { "mp3", required_argument, NULL, 'm' },
        { "channel", required_argument, NULL, 'c' },
        { "azimuth", required_argument, NULL, 'a' },
        { "elevation", required_argument, NULL, 'e' },
        { "gain", required_argument, NULL, 'g' },
        { "rate", required_argument, NULL, 'r' },
        { "help", no_argument, NULL, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "m:c:a:e:g:r:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'm':
            file_path = optarg;
            break;
        case 'c':
            st.channel_idx = atoi(optarg) - 1;
            break;
        case 'a':
            st.azimuth = (float)strtod(optarg, NULL);
            break;
        case 'e':
            st.elevation = (float)strtod(optarg, NULL);
            break;
        case 'g':
            st.gain = (float)strtod(optarg, NULL);
            break;
        case 'r':
            st.rate = (uint32_t)atoi(optarg);
            if (st.rate == 0) st.rate = 48000;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!file_path) {
        fprintf(stderr, "Missing --file\n");
        usage(argv[0]);
        return 1;
    }

    if (st.channel_idx < 0 || st.channel_idx >= 8) {
        fprintf(stderr, "Invalid --channel, use 1..8\n");
        return 1;
    }

    while (st.azimuth < 0.0f) st.azimuth += 360.0f;
    while (st.azimuth >= 360.0f) st.azimuth -= 360.0f;

    if (st.elevation < -90.0f) st.elevation = -90.0f;
    if (st.elevation > 90.0f) st.elevation = 90.0f;

    if (st.gain < 0.0f) st.gain = 0.0f;
    if (st.gain > 4.0f) st.gain = 4.0f;

    st.wait_link_frames = (int64_t)st.rate * 3;

    if (!decode_audio_with_ffmpeg(&st, file_path)) {
        return 1;
    }

    pw_init(&argc, &argv);

    st.loop = pw_main_loop_new(NULL);
    st.context = pw_context_new(pw_main_loop_get_loop(st.loop), NULL, 0);
    st.core = pw_context_connect(st.context, NULL, 0);
    if (!st.core) {
        fprintf(stderr, "Failed to connect to PipeWire core\n");
        free(st.pcm);
        return 1;
    }

    st.registry = pw_core_get_registry(st.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(st.registry, &st.registry_listener, &registry_events, &st);

    char node_name[64];
    snprintf(node_name, sizeof(node_name), "spatial_notification_ch%d_%d",
             st.channel_idx + 1, (int)getpid());

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Notification",
        PW_KEY_NODE_NAME, node_name,
        PW_KEY_NODE_DESCRIPTION, "Spatial Notification",
        NULL);

    st.stream = pw_stream_new(st.core, "spatial_notification", props);
    if (!st.stream) {
        fprintf(stderr, "pw_stream_new failed\n");
        st.failed = true;
    } else {
        pw_stream_add_listener(st.stream, &st.stream_listener, &stream_events, &st);

        struct spa_audio_info_raw info = {
            .format = SPA_AUDIO_FORMAT_F32,
            .rate = st.rate,
            .channels = 1,
            .position = { SPA_AUDIO_CHANNEL_MONO },
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
            st.failed = true;
        }
    }

    if (!st.failed) {
        pw_main_loop_run(st.loop);
    }

    if (st.stream)
        pw_stream_destroy(st.stream);
    if (st.filter_proxy)
        pw_proxy_destroy(st.filter_proxy);
    if (st.registry)
        pw_proxy_destroy((struct pw_proxy *)st.registry);
    if (st.core)
        pw_core_disconnect(st.core);
    if (st.context)
        pw_context_destroy(st.context);
    if (st.loop)
        pw_main_loop_destroy(st.loop);

    free(st.ports);
    free(st.links);
    free(st.pcm);

    pw_deinit();
    return st.failed ? 1 : 0;
}
