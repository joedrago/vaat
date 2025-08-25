#ifndef VAAT_PLAYER_H
#define VAAT_PLAYER_H

#include <gst/gst.h>

struct Player * playerCreate();
void playerDestroy(struct Player * player);

// returns NULL if there isn't one to adopt
GstSample * playerAdoptSample(struct Player * player);

#endif
