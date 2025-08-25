#include "player.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gst/app/gstappsink.h>
#include <gst/video/videooverlay.h>

struct Player
{
    GstElement * pipeline;
    GstElement * sink;

    pthread_mutex_t sampleMutex;
    GstSample * sample;

    struct Task * sampleThread;
};

static GstPadProbeReturn sinkQuery(GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
    GstQuery * query = (GstQuery *)info->data;

    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) {
        return GST_PAD_PROBE_OK;
    }

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    return GST_PAD_PROBE_HANDLED;
}

static void sampleThread(struct Player * player)
{
    printf("sampleThread begin\n");

    for (;;) {
        usleep(5 * 1000);

        GstSample * sample = gst_app_sink_try_pull_sample(GST_APP_SINK(player->sink), 1);
        if (sample) {
            // GstCaps * caps = gst_sample_get_caps(sample);
            // gchar * capsString = gst_caps_to_string(caps);
            // printf("color caps: %s\n", capsString);
            // g_free(capsString);

            pthread_mutex_lock(&player->sampleMutex);
            if (player->sample) {
                gst_sample_unref(player->sample);
            }
            player->sample = sample;
            pthread_mutex_unlock(&player->sampleMutex);
        }
    }

    printf("sampleThread end\n");
}

struct Player * playerCreate()
{
    struct Player * player = calloc(1, sizeof(struct Player));
    pthread_mutex_init(&player->sampleMutex, NULL);

    char pipelineDesc[4096];
    sprintf(pipelineDesc,
            "filesrc location=../test.video.es ! h264parse ! v4l2slh264dec ! video/x-raw(memory:DMABuf) ! appsink name=samplesink");

    printf("pipelineDesc: %s\n", pipelineDesc);

    GError * error = NULL;
    player->pipeline = gst_parse_launch(pipelineDesc, &error);
    if (error) {
        fatal(error->message);
    } else {
        printf("Successfully created pipeline.\n");
        gst_element_set_state(player->pipeline, GST_STATE_PLAYING);
    }

    player->sink = gst_bin_get_by_name(GST_BIN(player->pipeline), "samplesink");
    GstPad * sinkPad = gst_element_get_static_pad(player->sink, "sink");
    gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, sinkQuery, NULL, NULL);
    gst_object_unref(sinkPad);

    player->sampleThread = taskCreate((TaskFunc)sampleThread, player);

    return player;
}

GstSample * playerAdoptSample(struct Player * player)
{
    GstSample * sample = NULL;
    pthread_mutex_lock(&player->sampleMutex);
    sample = player->sample;
    player->sample = NULL;
    pthread_mutex_unlock(&player->sampleMutex);
    return sample;
}

void playerDestroy(struct Player * player)
{
    // TODO: implement
}
