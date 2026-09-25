/*
 * WineHua Gamepad Protocol (WHGP) v1 — shared with host Controller Hub.
 * Must stay in sync with entry/src/main/cpp/controller/gamepad_ipc_protocol.h
 */
#ifndef WINEHUA_GAMEPAD_PROTOCOL_H
#define WINEHUA_GAMEPAD_PROTOCOL_H

#include <stdint.h>

#define WHGP_MAGIC 0x50474857u /* 'WHGP' LE */
#define WHGP_VERSION 1
#define WHGP_MSG_STATE 1
#define WHGP_MSG_RESET 2

#pragma pack(push, 1)
struct whgp_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t msg_type;
    uint32_t slot;
    uint32_t payload_size;
};

struct whgp_state_v1
{
    uint32_t buttons; /* bit0=A ... bit9=R3 */
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
    uint16_t lt; /* 0..32767 */
    uint16_t rt;
    int8_t hat_x; /* -1/0/+1 */
    int8_t hat_y;
    uint8_t reserved[2];
};
#pragma pack(pop)

#endif /* WINEHUA_GAMEPAD_PROTOCOL_H */
