#ifndef PW_MIXER_PIPEWIRE_H
#define PW_MIXER_PIPEWIRE_H

#include <stdbool.h>
#include <glib.h>
#include "app.h"

bool init_pipewire(AppData *data);
gpointer pipewire_thread(gpointer user_data);
void shutdown_pipewire(AppData *data);
void send_sofa_control(AppData *data, int source_idx);
void relink_stereo_to_filter(AppData *data, int source_idx);
void unlink_all_filter_inputs(AppData *app);
void set_source_bypass(AppData *app, int source_idx, bool bypass);

#endif /* PW_MIXER_PIPEWIRE_H */
