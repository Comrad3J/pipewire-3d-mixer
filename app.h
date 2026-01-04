#ifndef PW_MIXER_APP_H
#define PW_MIXER_APP_H

#include <gtk/gtk.h>
#include <pipewire/pipewire.h>
#include <stdbool.h>

#define MAX_SOURCES 4
#define CANVAS_SIZE 400
#define CENTER_X (CANVAS_SIZE / 2)
#define CENTER_Y (CANVAS_SIZE / 2)
#define MAX_RADIUS 180
#define MIN_RADIUS_PCT 8.0f
#define MAX_STEREO_SLOTS 4   /* 4 stereo sources → 8 inputs */

typedef struct {
    bool occupied;
    uint32_t out_node_id;   /* node.id of the source (VLC, Spotify, etc.) */
} StereoSlot;

typedef struct {
    char *name;
    char *desc;
    char *media_class;
    char *icon_name;
} NodeInfo;

typedef struct {
    uint32_t global_id;      /* registry global id of the Port object */
    uint32_t node_id;        /* owning node global id (PW_KEY_NODE_ID) */
    int      direction;      /* 0=input, 1=output (from PW_KEY_PORT_DIRECTION) */
    int      port_id;        /* port.id within node (PW_KEY_PORT_ID) */
} PortInfo;

typedef struct {
    uint32_t link_id;        /* registry global id of the Link object */
    uint32_t out_port_gid;   /* global id of output Port object */
    uint32_t in_port_gid;    /* global id of input Port object */
    uint32_t out_node_id;    /* node id owning the output port */
    int      filter_in_port_id; /* 0..7 if input is filter input, else -1 */
} LinkInfo;

typedef struct {
    uint32_t id;
    char name[256];
    char app_label[128];
    uint32_t source_node_id; /* PipeWire node id of the stereo source */
    int index;         /* spk1=0, spk2=1, spk3=2, spk4=3 */
    float azimuth;     /* 0-360 degrees */
    float elevation;   /* -90 to 90 degrees */
    float radius;      /* 0-100 */
    float width;       /* stereo width in degrees */
    bool bypass;       /* true → bypass HRTF for this source */
    bool active;
    bool is_playing;
    bool initial_position_set;
    gint64 last_sofa_usec;
    float last_gain;
    float last_azimuth;
    float last_elevation;
    float last_radius;
    float last_width;
    bool last_valid;
} AudioSource;

typedef struct {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_proxy *filter_proxy;

    GtkWidget *window;
    GtkWidget *canvas;
    GtkWidget *source_boxes[MAX_SOURCES];
    GtkWidget *elevation_sliders[MAX_SOURCES];
    GtkWidget *width_sliders[MAX_SOURCES];
    GtkWidget *bypass_checkboxes[MAX_SOURCES];
    GtkWidget *playing_labels[MAX_SOURCES];
    GtkWidget *source_labels[MAX_SOURCES];

    AudioSource sources[MAX_SOURCES];
    int active_source;
    uint32_t filter_node_id;

    struct spa_hook registry_listener;
    struct spa_hook core_listener;

    GHashTable *ports; /* key: global port id (uint32), value: PortInfo* */
    GHashTable *links; /* key: global link id (uint32), value: LinkInfo* */
    GHashTable *nodes; /* key: global node id (uint32), value: NodeInfo* */

    /* Filter input port mapping: filter input "port.id" -> global port object id */
    uint32_t filter_in_gid[8];     /* index=port.id (0..7), value=global port id or 0 */
    bool     filter_in_occupied[8];

    bool initial_sync_done;
    uint32_t sync_seq;

    StereoSlot stereo_slots[MAX_STEREO_SLOTS];

    uint32_t default_sink_node_id;
} AppData;

void init_app_data(AppData *data);

#endif /* PW_MIXER_APP_H */
