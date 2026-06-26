#ifndef WINEOHOS_AUDIO_CLIENT_H
#define WINEOHOS_AUDIO_CLIENT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../../shared/audio/audio_ipc_protocol.h"

typedef struct OhosAudioClient
{
    int fd;
    uint32_t next_seq;
    pthread_mutex_t lock;
} OhosAudioClient;

typedef struct OhosAudioClientStream
{
    uint32_t stream_id;
    uint32_t preferred_period_frames;
    uint32_t ring_capacity_frames;
    uint32_t mix_frame_size;
    int ring_fd;
    size_t ring_mapping_size;
    WinehuaAudioRingBuffer *ring;
} OhosAudioClientStream;

int ohos_audio_client_connect(OhosAudioClient **out_client);
void ohos_audio_client_disconnect(OhosAudioClient *client);

int ohos_audio_client_open_stream(OhosAudioClient *client,
                                  const WinehuaAudioOpenStreamReq *req,
                                  OhosAudioClientStream *out_stream);
int ohos_audio_client_start(OhosAudioClient *client, uint32_t stream_id);
int ohos_audio_client_stop(OhosAudioClient *client, uint32_t stream_id);
int ohos_audio_client_reset(OhosAudioClient *client, uint32_t stream_id);
int ohos_audio_client_close_stream(OhosAudioClient *client, OhosAudioClientStream *stream);
int ohos_audio_client_get_status(OhosAudioClient *client,
                                 uint32_t stream_id,
                                 WinehuaAudioGetStatusResp *out_status);

size_t ohos_audio_client_write_frames(OhosAudioClientStream *stream, const void *data, size_t frames);
size_t ohos_audio_client_get_free_frames(const OhosAudioClientStream *stream);
size_t ohos_audio_client_get_queued_frames(const OhosAudioClientStream *stream);

#endif
