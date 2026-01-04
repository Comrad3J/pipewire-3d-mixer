#ifndef PW_MIXER_UI_H
#define PW_MIXER_UI_H

#include "app.h"

void build_gui(AppData *data);
void update_source_position(AppData *data, int source_idx, float azimuth, float radius);
void refresh_canvas(AppData *data);

#endif /* PW_MIXER_UI_H */
