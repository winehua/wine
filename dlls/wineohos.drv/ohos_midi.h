#ifndef WINEOHOS_MIDI_H
#define WINEOHOS_MIDI_H

#include "ntstatus.h"

NTSTATUS ohos_midi_driver_get(void *args);
NTSTATUS ohos_midi_driver_init(void *args);
NTSTATUS ohos_midi_driver_release(void *args);
NTSTATUS ohos_midi_driver_out_message(void *args);
NTSTATUS ohos_midi_driver_in_message(void *args);
NTSTATUS ohos_midi_driver_notify_wait(void *args);

#endif
