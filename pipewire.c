#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pipewire/pipewire.h>
#include <pipewire/keys.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/pod/pod.h>
#include <spa/support/loop.h>
#include <spa/utils/result.h>
#include <spa/utils/dict.h>
#include <math.h>
#include "pipewire.h"
#include "ui.h"

static inline gpointer u32key(uint32_t id) {
    return GUINT_TO_POINTER(id);
}

static void set_source_label(AppData *app, int slot, const char *app_name);
static void apply_connection_state(AppData *app, int slot);
static void destroy_link(AppData *app, uint32_t link_id);
static void cleanup_existing_filter_links(AppData *app);
static struct pw_proxy *create_link(AppData *app, uint32_t out_port_gid, uint32_t in_port_gid);
static int set_source_bypass_internal(struct spa_loop *loop, bool async, uint32_t seq,
                                      const void *data, size_t size, void *user_data);
static int cleanup_links_task(struct spa_loop *loop, bool async, uint32_t seq,
                              const void *data, size_t size, void *user_data);
static void run_on_pw_loop(AppData *app, spa_invoke_func_t func, void *data, size_t size);
static void set_slot_gain(AppData *data, int slot, float gain);
static float mirror_azimuth(float az);

static float radius_to_gain(float radius_pct)
{
    /* radius_pct in 0..100 → normalize to 0..1 */
    float r = radius_pct * 0.01f;
    if (r < 0.01f) r = 0.01f;
    if (r > 1.0f) r = 1.0f;

    /* Simple near→far taper: 1.0 at center, down to 0.1 at max radius */
    float gain = 1.0f - (r * 0.9f);
    if (gain < 0.1f) gain = 0.1f;
    return gain;
}

static float mirror_azimuth(float az)
{
    float m = 360.0f - az;
    if (m >= 360.0f) m -= 360.0f;
    if (m < 0.0f) m += 360.0f;
    return m;
}

static bool params_changed(const AudioSource *s, float az, float el, float rad, float width, float gain)
{
    if (!s->last_valid) return true;
    if (fabsf(az - s->last_azimuth) > 0.25f) return true;
    if (fabsf(el - s->last_elevation) > 0.25f) return true;
    if (fabsf(rad - s->last_radius) > 0.25f) return true;
    if (fabsf(width - s->last_width) > 0.25f) return true;
    if (fabsf(gain - s->last_gain) > 0.05f) return true;
    return false;
}

static void remember_params(AudioSource *s, float az, float el, float rad, float width, float gain)
{
    s->last_valid = true;
    s->last_azimuth = az;
    s->last_elevation = el;
    s->last_radius = rad;
    s->last_width = width;
    s->last_gain = gain;
}

static void set_source_label(AppData *app, int slot, const char *app_name)
{
    if (slot < 0 || slot >= MAX_SOURCES) return;

    if (app_name) {
        g_strlcpy(app->sources[slot].app_label, app_name, sizeof(app->sources[slot].app_label));
    } else {
        app->sources[slot].app_label[0] = '\0';
    }

    if (app->playing_labels[slot]) {
        if (app->sources[slot].app_label[0] != '\0') {
            char buf[160];
            snprintf(buf, sizeof(buf), "Playing: %s", app->sources[slot].app_label);
            gtk_label_set_text(GTK_LABEL(app->playing_labels[slot]), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(app->playing_labels[slot]), "No audio");
        }
    }
}

static void apply_connection_state(AppData *app, int slot)
{
    if (slot < 0 || slot >= MAX_SOURCES)
        return;

    bool was_playing = app->sources[slot].is_playing;
    int base = slot * 2;
    bool connected = false;

    if (base >= 0 && base + 1 < 8) {
        connected = app->filter_in_occupied[base] || app->filter_in_occupied[base + 1];
    }

    /* When bypassed, keep UI treated as connected */
    if (app->sources[slot].bypass) {
        connected = true;
    }

    if (app->sources[slot].is_playing == connected && !app->sources[slot].bypass)
        return;

    app->sources[slot].is_playing = connected;

    if (app->elevation_sliders[slot]) {
        gtk_widget_set_sensitive(app->elevation_sliders[slot], connected);
    }
    if (app->width_sliders[slot]) {
        gtk_widget_set_sensitive(app->width_sliders[slot], connected);
    }
    if (app->bypass_checkboxes[slot]) {
        gtk_widget_set_sensitive(app->bypass_checkboxes[slot], connected);
    }

    if (!connected) {
        set_source_label(app, slot, NULL);
        set_slot_gain(app, slot, 0.0f);
    }

    if (connected && !was_playing && !app->sources[slot].initial_position_set) {
        static const float default_azimuths[MAX_SOURCES] = {0.0f, 90.0f, 180.0f, 270.0f};
        app->sources[slot].azimuth = default_azimuths[slot];
        app->sources[slot].radius = 55.0f;
        app->sources[slot].width = 25.0f;
        app->sources[slot].initial_position_set = true;
        send_sofa_control(app, slot);
        refresh_canvas(app);
    }

    if (!connected && app->active_source == slot) {
        app->active_source = -1;
    }

    refresh_canvas(app);
}

static void destroy_link(AppData *app, uint32_t link_id)
{
    int res;

    if (!app->registry)
        return;

    printf("[linkmgr] destroying link id=%u\n", link_id);

    res = pw_registry_destroy(app->registry, link_id);
    if (res < 0) {
        fprintf(stderr, "[linkmgr] pw_registry_destroy(%u) failed: %d (%s)\n",
                link_id, res, spa_strerror(res));
    }
}

static NodeInfo *node_info_new(const struct spa_dict *props)
{
    NodeInfo *ni = g_new0(NodeInfo, 1);
    const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *node_desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *icon = spa_dict_lookup(props, PW_KEY_APP_ICON_NAME);

    if (node_name) ni->name = g_strdup(node_name);
    if (node_desc) ni->desc = g_strdup(node_desc);
    if (media_class) ni->media_class = g_strdup(media_class);
    if (icon) ni->icon_name = g_strdup(icon);
    return ni;
}

static void node_info_free(gpointer data)
{
    NodeInfo *ni = data;
    if (!ni) return;
    g_free(ni->name);
    g_free(ni->desc);
    g_free(ni->media_class);
    g_free(ni->icon_name);
    g_free(ni);
}

static const char *node_label(NodeInfo *ni)
{
    if (!ni) return NULL;
    if (ni->desc && ni->desc[0]) return ni->desc;
    if (ni->name && ni->name[0]) return ni->name;
    return NULL;
}

static void replace_node_info(AppData *app, uint32_t id, NodeInfo *ni)
{
    NodeInfo *old = g_hash_table_lookup(app->nodes, u32key(id));
    if (old) node_info_free(old);
    g_hash_table_replace(app->nodes, u32key(id), ni);
}

static uint32_t find_source_node_id(const AppData *app, int slot)
{
    if (slot < 0 || slot >= MAX_SOURCES) return 0;
    return app->sources[slot].source_node_id;
}

static uint32_t find_sink_input_gid(const AppData *app, uint32_t sink_node_id, int port_id)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->ports);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PortInfo *pi = value;
        if (pi->node_id == sink_node_id && pi->direction == 0 && pi->port_id == port_id) {
            return pi->global_id;
        }
    }
    return 0;
}

static uint32_t find_source_output_gid(const AppData *app, uint32_t src_node_id, int port_id)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, app->ports);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        PortInfo *pi = value;
        if (pi->node_id == src_node_id && pi->direction == 1 && pi->port_id == port_id) {
            return pi->global_id;
        }
    }
    return 0;
}

static void destroy_links_from_node(AppData *app, uint32_t node_id)
{
    GHashTableIter iter;
    gpointer key, value;
    GList *to_destroy = NULL;

    g_hash_table_iter_init(&iter, app->links);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LinkInfo *li = value;
        if (li->out_node_id == node_id) {
            if (li->filter_in_port_id >= 0 && li->filter_in_port_id < 8) {
                app->filter_in_occupied[li->filter_in_port_id] = false;
            }
            to_destroy = g_list_prepend(to_destroy, GUINT_TO_POINTER(li->link_id));
        }
    }

    for (GList *l = to_destroy; l; l = l->next) {
        uint32_t lid = GPOINTER_TO_UINT(l->data);
        g_hash_table_remove(app->links, u32key(lid));
        destroy_link(app, lid);
    }
    g_list_free(to_destroy);
}

static void create_sink_links(AppData *app, int slot)
{
    uint32_t src_node = find_source_node_id(app, slot);
    if (!src_node || app->default_sink_node_id == 0) return;

    for (int ch = 0; ch < 2; ch++) {
        uint32_t out_gid = find_source_output_gid(app, src_node, ch);
        uint32_t in_gid  = find_sink_input_gid(app, app->default_sink_node_id, ch);
        if (out_gid && in_gid) {
            create_link(app, out_gid, in_gid);
        }
    }
}

static void create_filter_links(AppData *app, int slot)
{
    uint32_t src_node = find_source_node_id(app, slot);
    if (!src_node || app->filter_node_id == 0) return;

    int base_input = slot * 2;
    for (int ch = 0; ch < 2; ch++) {
        uint32_t out_gid = find_source_output_gid(app, src_node, ch);
        uint32_t in_gid  = app->filter_in_gid[base_input + ch];
        if (out_gid && in_gid) {
            create_link(app, out_gid, in_gid);
            app->filter_in_occupied[base_input + ch] = true;
        }
    }
}

void relink_stereo_to_filter(AppData *app, int source_idx)
{
    struct {AppData *a; int idx; bool bp;} payload = {app, source_idx, false};
    run_on_pw_loop(app, set_source_bypass_internal, &payload, sizeof(payload));
}

void unlink_all_filter_inputs(AppData *app)
{
    AppData *payload = app;
    run_on_pw_loop(app, cleanup_links_task, &payload, sizeof(payload));
}

void set_source_bypass(AppData *app, int source_idx, bool bypass)
{
    struct {AppData *a; int idx; bool bp;} payload = {app, source_idx, bypass};
    run_on_pw_loop(app, set_source_bypass_internal, &payload, sizeof(payload));
}

static int set_source_bypass_internal(struct spa_loop *loop, bool async, uint32_t seq,
                                      const void *data, size_t size, void *user_data)
{
    (void)loop; (void)async; (void)seq; (void)size; (void)user_data;
    const struct {AppData *a; int idx; bool bp;} *p = data;
    AppData *app = p->a;
    int source_idx = p->idx;
    bool bypass = p->bp;

    if (!app || source_idx < 0 || source_idx >= MAX_SOURCES) return 0;

    app->sources[source_idx].bypass = bypass;

    destroy_links_from_node(app, find_source_node_id(app, source_idx));
    if (bypass) {
        create_sink_links(app, source_idx);
        app->sources[source_idx].is_playing = true;
    } else {
        create_filter_links(app, source_idx);
        int base = source_idx * 2;
        if (app->filter_in_gid[base])     app->filter_in_occupied[base] = true;
        if (app->filter_in_gid[base + 1]) app->filter_in_occupied[base + 1] = true;
        apply_connection_state(app, source_idx);
        send_sofa_control(app, source_idx);
    }
    apply_connection_state(app, source_idx);
    return 0;
}

static int cleanup_links_task(struct spa_loop *loop, bool async, uint32_t seq,
                              const void *data, size_t size, void *user_data)
{
    (void)loop; (void)async; (void)seq; (void)size; (void)user_data;
    AppData *app = *(AppData * const *)data;
    cleanup_existing_filter_links(app);
    return 0;
}

static void run_on_pw_loop(AppData *app, spa_invoke_func_t func, void *data, size_t size)
{
    if (!app || !app->loop) return;
    pw_loop_invoke(pw_main_loop_get_loop(app->loop), func, 0, data, size, true, NULL);
}
static void cleanup_existing_filter_links(AppData *app)
{
    GHashTableIter iter;
    gpointer key, value;

    printf("[startup] cleaning up existing links into spatializer\n");

    g_hash_table_iter_init(&iter, app->links);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LinkInfo *li = value;

        PortInfo *in_pi =
            g_hash_table_lookup(app->ports, u32key(li->in_port_gid));

        if (!in_pi)
            continue;

        if (in_pi->node_id == app->filter_node_id &&
            in_pi->direction == 0) {

            printf("[startup] destroying existing link id=%u\n",
                   li->link_id);

            destroy_link(app, li->link_id);
        }
    }

    for (int i = 0; i < 8; i++)
        app->filter_in_occupied[i] = false;

    for (int i = 0; i < MAX_STEREO_SLOTS; i++) {
        app->stereo_slots[i].occupied = false;
        app->stereo_slots[i].out_node_id = 0;
    }
}

static struct pw_proxy *create_link(AppData *app, uint32_t out_port_gid, uint32_t in_port_gid)
{
    if (!app->core) return NULL;

    char out_port_str[16], in_port_str[16];
    snprintf(out_port_str, sizeof(out_port_str), "%u", out_port_gid);
    snprintf(in_port_str,  sizeof(in_port_str),  "%u", in_port_gid);

    const struct spa_dict_item items[] = {
        { PW_KEY_LINK_OUTPUT_PORT, out_port_str },
        { PW_KEY_LINK_INPUT_PORT,  in_port_str  },
    };
    struct spa_dict dict = SPA_DICT_INIT(items, (uint32_t)(sizeof(items)/sizeof(items[0])));

    printf("[linkmgr] creating link out_port=%u -> in_port=%u\n", out_port_gid, in_port_gid);

    return pw_core_create_object(app->core,
                                "link-factory",
                                PW_TYPE_INTERFACE_Link,
                                PW_VERSION_LINK,
                                &dict,
                                0);
}

static int find_or_allocate_stereo_slot(AppData *app, uint32_t out_node_id)
{
    for (int i = 0; i < MAX_STEREO_SLOTS; i++) {
        if (app->stereo_slots[i].occupied &&
            app->stereo_slots[i].out_node_id == out_node_id)
            return i;
    }

    for (int i = 0; i < MAX_STEREO_SLOTS; i++) {
        if (!app->stereo_slots[i].occupied) {
            app->stereo_slots[i].occupied = true;
            app->stereo_slots[i].out_node_id = out_node_id;
            printf("[stereo] allocated slot %d for node %u\n",
                   i, out_node_id);
            return i;
        }
    }

    return -1;
}

static void free_stereo_slot(AppData *app, uint32_t out_node_id)
{
    for (int i = 0; i < MAX_STEREO_SLOTS; i++) {
        if (app->stereo_slots[i].occupied &&
            app->stereo_slots[i].out_node_id == out_node_id) {

            app->stereo_slots[i].occupied = false;
            app->stereo_slots[i].out_node_id = 0;

            printf("[stereo] freed slot %d for node %u\n",
                   i, out_node_id);
        }
    }
}

struct param_data {
    struct pw_proxy *proxy;
    char name[64];
    float value;
};

static int do_set_param(struct spa_loop *loop, bool async, uint32_t seq,
                        const void *data, size_t size, void *user_data)
{
    (void)loop; (void)async; (void)seq; (void)size; (void)user_data;

    const struct param_data *pd = data;

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame f, f_struct;

    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&b, &f_struct);
    spa_pod_builder_string(&b, pd->name);
    spa_pod_builder_float(&b, pd->value);
    spa_pod_builder_pop(&b, &f_struct);
    spa_pod_builder_pop(&b, &f);

    struct spa_pod *pod = spa_pod_builder_deref(&b, 0);
    pw_node_set_param((struct pw_node*)pd->proxy, SPA_PARAM_Props, 0, pod);

    return 0;
}

static void set_slot_gain(AppData *data, int slot, float gain)
{
    if (!data || !data->filter_proxy) return;
    if (slot < 0 || slot >= MAX_SOURCES) return;

    struct param_data pd_gain;
    pd_gain.proxy = data->filter_proxy;
    int base_channel = slot * 2;

    for (int i = 0; i < 2; i++) {
        int channel_id = base_channel + i + 1; /* 1..8 */

        snprintf(pd_gain.name, sizeof(pd_gain.name), "mixL:Gain %d", channel_id);
        pd_gain.value = gain;
        pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                       &pd_gain, sizeof(pd_gain), false, NULL);

        snprintf(pd_gain.name, sizeof(pd_gain.name), "mixR:Gain %d", channel_id);
        pd_gain.value = gain;
        pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                       &pd_gain, sizeof(pd_gain), false, NULL);
    }
}

static void send_sofa_control_force(AppData *data, int source_idx)
{
    if (!data) return;
    data->sources[source_idx].last_valid = false;
    data->sources[source_idx].last_sofa_usec = 0;
    send_sofa_control(data, source_idx);
}

void send_sofa_control(AppData *data, int source_idx)
{
    if (!data->sources[source_idx].active || !data->filter_proxy) return;

    const char *left_names[]  = {"spk1", "spk3", "spk5", "spk7"};
    const char *right_names[] = {"spk2", "spk4", "spk6", "spk8"};
    if (source_idx < 0 || source_idx >= 4) return;

    if (data->sources[source_idx].bypass) {
        /* When bypassing, skip parameter updates */
        return;
    }

    gint64 now = g_get_monotonic_time();
    if (data->sources[source_idx].last_sofa_usec != 0 &&
        now - data->sources[source_idx].last_sofa_usec < 40000) { /* 40ms throttle */
        return;
    }
    data->sources[source_idx].last_sofa_usec = now;

    float center = data->sources[source_idx].azimuth;
    float width = data->sources[source_idx].width;
    float elevation = data->sources[source_idx].elevation;
    float radius = data->sources[source_idx].radius;
    float gain = radius_to_gain(radius);

    float half = width * 0.5f;

    float left_az = center - half;
    float right_az = center + half;

    if (left_az < 0.0f) left_az += 360.0f;
    if (left_az >= 360.0f) left_az -= 360.0f;
    if (right_az < 0.0f) right_az += 360.0f;
    if (right_az >= 360.0f) right_az -= 360.0f;

    const char *names[] = { left_names[source_idx], right_names[source_idx] };
    const float azimuths[] = { left_az, right_az };
    float bypass = data->sources[source_idx].bypass ? 1.0f : 0.0f;
    int base_channel = source_idx * 2; /* 0-based */

    /* avoid redundant updates to reduce artifact noise */
    if (!params_changed(&data->sources[source_idx], center, elevation, radius, width, gain)) {
        return;
    }

    bool gain_changed = !data->sources[source_idx].last_valid ||
        fabsf(radius - data->sources[source_idx].last_radius) > 0.5f;

    for (int i = 0; i < 2; i++) {
        const char *spk_name = names[i];
        float azimuth = mirror_azimuth(azimuths[i]);

        printf("Setting SOFA controls for %s: azimuth=%.1f°, elevation=%.1f°, radius=%.1f\n",
               spk_name, azimuth, elevation, radius);

        struct param_data pd_az;
        pd_az.proxy = data->filter_proxy;
        snprintf(pd_az.name, sizeof(pd_az.name), "%.48s:Azimuth", spk_name);
        pd_az.value = azimuth;
        pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                       &pd_az, sizeof(pd_az), false, NULL);

        struct param_data pd_el;
        pd_el.proxy = data->filter_proxy;
        snprintf(pd_el.name, sizeof(pd_el.name), "%.46s:Elevation", spk_name);
        pd_el.value = elevation;
        pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                       &pd_el, sizeof(pd_el), false, NULL);

        struct param_data pd_rad;
        pd_rad.proxy = data->filter_proxy;
        snprintf(pd_rad.name, sizeof(pd_rad.name), "%.51s:Radius", spk_name);
        pd_rad.value = radius;
        pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                       &pd_rad, sizeof(pd_rad), false, NULL);

        struct param_data pd_bypass;
        pd_bypass.proxy = data->filter_proxy;
        snprintf(pd_bypass.name, sizeof(pd_bypass.name), "%.48s:Bypass", spk_name);
        pd_bypass.value = bypass;
        pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                       &pd_bypass, sizeof(pd_bypass), false, NULL);

        if (gain_changed) {
            struct param_data pd_gain;
            pd_gain.proxy = data->filter_proxy;
            int channel_id = base_channel + i + 1; /* 1..8 */
            snprintf(pd_gain.name, sizeof(pd_gain.name), "mixL:Gain %d", channel_id);
            pd_gain.value = gain;
            pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                           &pd_gain, sizeof(pd_gain), false, NULL);

            snprintf(pd_gain.name, sizeof(pd_gain.name), "mixR:Gain %d", channel_id);
            pd_gain.value = gain;
            pw_loop_invoke(pw_main_loop_get_loop(data->loop), do_set_param, 1,
                           &pd_gain, sizeof(pd_gain), false, NULL);
        }
    }

    remember_params(&data->sources[source_idx], center, elevation, radius, width, gain);
}

static void registry_event_global(void *data, uint32_t id, uint32_t permissions,
                                   const char *type, uint32_t version,
                                   const struct spa_dict *props)
{
    AppData *app = data;

    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *node_desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

        if (node_name && strcmp(node_name, "effect_input.multi_spatial") == 0) {
            printf("Found Multi-Source Spatializer node: %s (id: %u)\n", node_name, id);
            app->filter_node_id = id;

            app->filter_proxy = pw_registry_bind(app->registry,
                                                  id,
                                                  PW_TYPE_INTERFACE_Node,
                                                  PW_VERSION_NODE,
                                                  0);

            /* Start with all mixer gains muted to avoid stale buffers before links appear */
            for (int i = 0; i < MAX_SOURCES; i++) {
                set_slot_gain(app, i, 0.0f);
                app->sources[i].last_valid = false;
                app->sources[i].last_sofa_usec = 0;
            }

            const char *source_names[] = {"spk1/spk2", "spk3/spk4", "spk5/spk6", "spk7/spk8"};
            for (int i = 0; i < MAX_SOURCES; i++) {
                app->sources[i].id = id;
                app->sources[i].index = i;
                app->sources[i].active = true;
                strncpy(app->sources[i].name, source_names[i], sizeof(app->sources[i].name) - 1);
                app->sources[i].azimuth = (i == 3) ? 270.0f : 0.0f;
                app->sources[i].elevation = 0.0f;
                app->sources[i].radius = 50.0f;
                app->sources[i].is_playing = false;

                if (app->source_labels[i]) {
                    char label_text[64];
                    snprintf(label_text, sizeof(label_text), "Source %d (%s)", i + 1, source_names[i]);
                    gtk_label_set_text(GTK_LABEL(app->source_labels[i]), label_text);
                }

                if (app->elevation_sliders[i]) {
                    gtk_widget_set_sensitive(app->elevation_sliders[i], false);
                }
            }

            refresh_canvas(app);
            return;
        }

        /* cache node info for display */
        replace_node_info(app, id, node_info_new(props));

        if (app->default_sink_node_id == 0 && media_class && strcmp(media_class, "Audio/Sink") == 0) {
            app->default_sink_node_id = id;
        }
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char *node_id_s = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *dir_s     = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char *port_id_s = spa_dict_lookup(props, PW_KEY_PORT_ID);

        if (!node_id_s || !dir_s || !port_id_s) {
            return;
        }

        PortInfo *pi = g_new0(PortInfo, 1);
        pi->global_id = id;
        pi->node_id   = (uint32_t)atoi(node_id_s);
        pi->direction = (strcmp(dir_s, "in") == 0) ? 0 : 1;
        pi->port_id   = atoi(port_id_s);

        g_hash_table_replace(app->ports, u32key(id), pi);

        if (app->filter_node_id != 0 && pi->node_id == app->filter_node_id) {
            if (pi->direction == 0 && pi->port_id >= 0 && pi->port_id < 8) {
                app->filter_in_gid[pi->port_id] = pi->global_id;
                printf("[registry] filter input port discovered port.id=%d global_id=%u\n",
                    pi->port_id, pi->global_id);
            }
        }
        return;
    }

    if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char *out_port_s = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT);
        const char *in_port_s  = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT);

        if (!out_port_s || !in_port_s) {
            printf("[registry] LINK id=%u missing port props\n", id);
            return;
        }

        uint32_t out_port_gid = (uint32_t)atoi(out_port_s);
        uint32_t in_port_gid  = (uint32_t)atoi(in_port_s);

        PortInfo *out_pi = g_hash_table_lookup(app->ports, u32key(out_port_gid));
        PortInfo *in_pi  = g_hash_table_lookup(app->ports, u32key(in_port_gid));

        printf("[registry] LINK id=%u out_port_gid=%u in_port_gid=%u\n",
            id, out_port_gid, in_port_gid);

        if (!out_pi || !in_pi) {
            printf("[linkmgr] port metadata not ready for link id=%u (out_pi=%p in_pi=%p)\n",
                id, (void*)out_pi, (void*)in_pi);
            return;
        }

        /* Prevent feedback: block effect_output → effect_input links */
        NodeInfo *out_ni = g_hash_table_lookup(app->nodes, u32key(out_pi->node_id));
        NodeInfo *in_ni  = g_hash_table_lookup(app->nodes, u32key(in_pi->node_id));
        if (out_ni && in_ni &&
            out_ni->name && strstr(out_ni->name, "effect_output.multi_spatial") &&
            in_ni->name && strstr(in_ni->name, "effect_input.multi_spatial")) {
            printf("[linkmgr] rejecting feedback link id=%u (%s -> %s)\n",
                   id, out_ni->name, in_ni->name);
            destroy_link(app, id);
            return;
        }

        LinkInfo *li = g_new0(LinkInfo, 1);
        li->link_id = id;
        li->out_port_gid = out_port_gid;
        li->in_port_gid  = in_port_gid;
        li->out_node_id  = out_pi->node_id;
        li->filter_in_port_id = -1;

        if (out_pi->port_id >= 2) {
            printf("[linkmgr] rejecting non-stereo output: out port.id=%d (link id=%u)\n",
                out_pi->port_id, id);
            g_hash_table_replace(app->links, u32key(id), li);
            destroy_link(app, id);
            return;
        }

        if (app->filter_node_id != 0 && in_pi->node_id == app->filter_node_id && in_pi->direction == 0) {
            int in_port_id = in_pi->port_id;
            if (in_port_id >= 0 && in_port_id < 8) {

                li->filter_in_port_id = in_port_id;

                if (app->filter_node_id != 0 && in_pi->node_id == app->filter_node_id && in_pi->direction == 0) {

                    int slot = find_or_allocate_stereo_slot(app, out_pi->node_id);
                    if (slot < 0) {
                        printf("[linkmgr] no free stereo slots; destroying link id=%u\n", id);
                        destroy_link(app, id);
                        return;
                    }

                    app->sources[slot].source_node_id = out_pi->node_id;

                    int base_input = slot * 2;
                    int target_input = base_input + out_pi->port_id;

                    if (target_input < 0 || target_input >= 8) {
                        destroy_link(app, id);
                        return;
                    }

                    uint32_t target_in_gid = app->filter_in_gid[target_input];
                    if (!target_in_gid) {
                        destroy_link(app, id);
                        return;
                    }

                    if (app->sources[slot].bypass) {
                        printf("[linkmgr] bypass active; dropping link id=%u for slot %d\n", id, slot);
                        destroy_link(app, id);
                        create_sink_links(app, slot);
                        return;
                    }

                    if (in_pi->port_id != target_input) {
                        /* If the desired input is already occupied by the same slot, just drop this stray link to avoid flapping */
                        if (app->filter_in_occupied[target_input]) {
                            printf("[linkmgr] dropping stray stereo link id=%u (node %u port.id=%d) because target input %d already in use\n",
                                   id, out_pi->node_id, out_pi->port_id, target_input);
                            destroy_link(app, id);
                            return;
                        }

                        printf("[linkmgr] correcting stereo link: node %u port.id=%d → filter input %d\n",
                            out_pi->node_id, out_pi->port_id, target_input);

                        destroy_link(app, id);
                        create_link(app, out_port_gid, target_in_gid);
                        return;
                    }

                    li->filter_in_port_id = target_input;
                    app->filter_in_occupied[target_input] = true;

                    printf("[linkmgr] accepted stereo link id=%u slot=%d input=%d\n",
                        id, slot, target_input);

                    const char *app_name = g_hash_table_lookup(app->nodes, u32key(out_pi->node_id));
                    NodeInfo *ni = g_hash_table_lookup(app->nodes, u32key(out_pi->node_id));
                    set_source_label(app, slot, node_label(ni));

                    apply_connection_state(app, slot);

                    /* On fresh link, reset to a defined default pose and push params immediately */
                    app->sources[slot].azimuth = 0.0f;
                    app->sources[slot].elevation = 0.0f;
                    app->sources[slot].width = 25.0f;
                    app->sources[slot].radius = 55.0f;
                    app->sources[slot].initial_position_set = true;
                    send_sofa_control_force(app, slot);
                }
            }
        }

        g_hash_table_replace(app->links, u32key(id), li);
        return;
    }

}

static void registry_event_global_remove(void *data, uint32_t id)
{
    AppData *app = data;

    LinkInfo *li = g_hash_table_lookup(app->links, u32key(id));
    if (li) {
        if (li->filter_in_port_id >= 0 && li->filter_in_port_id < 8) {
            app->filter_in_occupied[li->filter_in_port_id] = false;
            printf("[linkmgr] link removed id=%u freed filter input port.id=%d\n",
                   id, li->filter_in_port_id);

            int slot = li->filter_in_port_id / 2;
            apply_connection_state(app, slot);
            if (!app->filter_in_occupied[li->filter_in_port_id ^ 1]) {
                free_stereo_slot(app, li->out_node_id);
            }
        }
        g_hash_table_remove(app->links, u32key(id));
    }

    if (g_hash_table_contains(app->ports, u32key(id))) {
        g_hash_table_remove(app->ports, u32key(id));
        return;
    }

    if (app->filter_node_id == id) {
        printf("Multi-Source Spatializer node removed (id: %u)\n", id);
        app->filter_node_id = 0;

        for (int i = 0; i < 8; i++) {
            app->filter_in_gid[i] = 0;
            app->filter_in_occupied[i] = false;
        }

        if (g_hash_table_contains(app->nodes, u32key(id))) {
            g_hash_table_remove(app->nodes, u32key(id));
        }

        for (int i = 0; i < MAX_SOURCES; i++) {
            app->sources[i].active = false;
            app->sources[i].is_playing = false;
            if (app->source_labels[i]) {
                gtk_label_set_text(GTK_LABEL(app->source_labels[i]), "No source");
            }
            if (app->elevation_sliders[i]) {
                gtk_widget_set_sensitive(app->elevation_sliders[i], false);
            }
            if (app->width_sliders[i]) {
                gtk_widget_set_sensitive(app->width_sliders[i], false);
            }
        }

        refresh_canvas(app);
    }

    if (g_hash_table_contains(app->nodes, u32key(id))) {
        NodeInfo *old = g_hash_table_lookup(app->nodes, u32key(id));
        if (old) node_info_free(old);
        g_hash_table_remove(app->nodes, u32key(id));
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_event_global,
    .global_remove = registry_event_global_remove,
};

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    fprintf(stderr, "PipeWire core error: %s\n", message);
}

static void on_core_done(void *data, uint32_t id, int seq)
{
    AppData *app = data;

    if (seq != (int)app->sync_seq || app->initial_sync_done)
        return;

    printf("[startup] registry sync complete\n");

    app->initial_sync_done = true;

    if (app->filter_node_id != 0)
        cleanup_existing_filter_links(app);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
    .done  = on_core_done,
};

bool init_pipewire(AppData *data)
{
    pw_init(NULL, NULL);

    data->loop = pw_main_loop_new(NULL);
    if (!data->loop) {
        fprintf(stderr, "Failed to create main loop\n");
        return false;
    }

    data->context = pw_context_new(pw_main_loop_get_loop(data->loop), NULL, 0);
    if (!data->context) {
        fprintf(stderr, "Failed to create context\n");
        return false;
    }

    data->core = pw_context_connect(data->context, NULL, 0);
    if (!data->core) {
        fprintf(stderr, "Failed to connect to PipeWire\n");
        return false;
    }

    pw_core_add_listener(data->core, &data->core_listener, &core_events, data);

    data->registry = pw_core_get_registry(data->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(data->registry, &data->registry_listener, &registry_events, data);

    data->sync_seq = pw_core_sync(data->core, 0, 1);

    printf("Connected to PipeWire\n");
    printf("Looking for 'effect_input.multi_spatial' filter-chain node...\n");
    return true;
}

gpointer pipewire_thread(gpointer user_data)
{
    AppData *data = user_data;
    pw_main_loop_run(data->loop);
    return NULL;
}

void shutdown_pipewire(AppData *data)
{
    if (data->filter_proxy) pw_proxy_destroy(data->filter_proxy);
    if (data->registry) pw_proxy_destroy((struct pw_proxy*)data->registry);
    if (data->core) pw_core_disconnect(data->core);
    if (data->context) pw_context_destroy(data->context);
    if (data->loop) pw_main_loop_destroy(data->loop);

    pw_deinit();
}
#include <stdbool.h>
