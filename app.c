#include <string.h>
#include <glib.h>
#include "app.h"

void init_app_data(AppData *data)
{
    memset(data, 0, sizeof(*data));

    data->ports = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    data->links = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    data->nodes = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (int i = 0; i < 8; i++) {
        data->filter_in_gid[i] = 0;
        data->filter_in_occupied[i] = false;
    }

    for (int i = 0; i < MAX_STEREO_SLOTS; i++) {
        data->stereo_slots[i].occupied = false;
        data->stereo_slots[i].out_node_id = 0;
    }

    data->active_source = -1;

    for (int i = 0; i < MAX_SOURCES; i++) {
        data->sources[i].azimuth = 0.0f;
        data->sources[i].elevation = 0.0f;
        data->sources[i].radius = 50.0f;
        data->sources[i].width = 20.0f;
        data->sources[i].bypass = false;
        data->sources[i].app_label[0] = '\0';
        data->sources[i].active = false;
        data->sources[i].is_playing = false;
        data->sources[i].initial_position_set = false;
        data->sources[i].last_sofa_usec = 0;
        data->sources[i].last_gain = 0.0f;
        data->sources[i].last_azimuth = 0.0f;
        data->sources[i].last_elevation = 0.0f;
        data->sources[i].last_radius = 0.0f;
        data->sources[i].last_width = 0.0f;
        data->sources[i].last_valid = false;
    }
}
