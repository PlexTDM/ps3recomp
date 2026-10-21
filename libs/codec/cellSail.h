/*
 * ps3recomp - cellSail HLE: the system's movie player (libsail)
 *
 * A title gives the player a URI and two adapters of its own: it pulls the
 * decoded pictures from the graphics adapter and the decoded sound from the
 * sound adapter every frame, and hears about the player's progress through
 * one event callback. See cellSail.c.
 *
 * Every pointer here is a guest address: the NID bridge passes r3..r10
 * through as they are.
 */

#ifndef PS3RECOMP_CELL_SAIL_H
#define PS3RECOMP_CELL_SAIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (the SDK's) */
#define CELL_SAIL_ERROR_INVALID_ARG         0x80610701
#define CELL_SAIL_ERROR_INVALID_STATE       0x80610702
#define CELL_SAIL_ERROR_UNSUPPORTED_STREAM  0x80610703
#define CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE  0x80610704
#define CELL_SAIL_ERROR_EMPTY               0x80610705
#define CELL_SAIL_ERROR_FULLED              0x80610706
#define CELL_SAIL_ERROR_USING               0x80610707
#define CELL_SAIL_ERROR_NOT_AVAILABLE       0x80610708
#define CELL_SAIL_ERROR_CANCEL              0x80610709
#define CELL_SAIL_ERROR_MEMORY              0x806107F0
#define CELL_SAIL_ERROR_INVALID_FD          0x806107F1
#define CELL_SAIL_ERROR_FATAL               0x806107FF

/* Event majors: the event is passed as one u64, major << 32 | minor */
#define CELL_SAIL_EVENT_ERROR_OCCURED           1
#define CELL_SAIL_EVENT_PLAYER_CALL_COMPLETED   2   /* minor: the call; arg0: its result */
#define CELL_SAIL_EVENT_PLAYER_STATE_CHANGED    3   /* arg0: the new state */
#define CELL_SAIL_EVENT_STREAM_OPENED           4
#define CELL_SAIL_EVENT_STREAM_CLOSED           5
#define CELL_SAIL_EVENT_SESSION_STARTED         6
#define CELL_SAIL_EVENT_PAUSE_STATE_CHANGED     7
#define CELL_SAIL_EVENT_SOURCE_EOS              8

/* Player calls (the minor of PLAYER_CALL_COMPLETED) */
#define CELL_SAIL_PLAYER_CALL_BOOT              1
#define CELL_SAIL_PLAYER_CALL_OPEN_STREAM       2
#define CELL_SAIL_PLAYER_CALL_CLOSE_STREAM      3
#define CELL_SAIL_PLAYER_CALL_START             10
#define CELL_SAIL_PLAYER_CALL_STOP              11

/* Player states */
#define CELL_SAIL_PLAYER_STATE_INITIALIZED      0
#define CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION  1
#define CELL_SAIL_PLAYER_STATE_CLOSED           2
#define CELL_SAIL_PLAYER_STATE_OPEN_TRANSITION  3
#define CELL_SAIL_PLAYER_STATE_OPENED           4
#define CELL_SAIL_PLAYER_STATE_START_TRANSITION 5
#define CELL_SAIL_PLAYER_STATE_RUNNING          6
#define CELL_SAIL_PLAYER_STATE_STOP_TRANSITION  7
#define CELL_SAIL_PLAYER_STATE_CLOSE_TRANSITION 8
#define CELL_SAIL_PLAYER_STATE_LOST             9

/* CellSailVideoFormat.coding / CellSailAudioFormat.coding */
#define CELL_SAIL_VIDEO_CODING_ARGB_INTERLEAVED 0
#define CELL_SAIL_VIDEO_CODING_RGBA_INTERLEAVED 1
#define CELL_SAIL_VIDEO_CODING_UYVY422          2
#define CELL_SAIL_VIDEO_CODING_YUV420_PLANAR    3
#define CELL_SAIL_AUDIO_CODING_LPCM_FLOAT32     1

#define CELL_SAIL_PLAYER_MAX    4

/* Memory allocator and player */
s32 cellSailMemAllocatorInitialize(u32 pSelf, u32 pCallbacks);
s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pCallbackArg, u32 pAttribute, u32 pResource);
s32 cellSailPlayerFinalize(u32 pSelf);
s32 cellSailPlayerSetSoundAdapter(u32 pSelf, u32 index, u32 pAdapter);
s32 cellSailPlayerSetGraphicsAdapter(u32 pSelf, u32 index, u32 pAdapter);
s32 cellSailPlayerBoot(u32 pSelf, u64 userParam);
s32 cellSailPlayerCreateDescriptor(u32 pSelf, s32 streamType, u32 pMediaInfo,
                                   u32 pUri, u32 ppDesc);
s32 cellSailPlayerDestroyDescriptor(u32 pSelf, u32 pDesc);
s32 cellSailPlayerAddDescriptor(u32 pSelf, u32 pDesc);
s32 cellSailPlayerRemoveDescriptor(u32 pSelf, u32 ppDesc);
s32 cellSailPlayerOpenStream(u32 pSelf);
s32 cellSailPlayerCloseStream(u32 pSelf);
s32 cellSailPlayerStart(u32 pSelf, u32 pCommand, u64 userParam);
s32 cellSailPlayerStop(u32 pSelf, u64 userParam);
s32 cellSailPlayerSetPaused(u32 pSelf, s32 paused);
s32 cellSailPlayerIsPaused(u32 pSelf);
s32 cellSailPlayerCancel(u32 pSelf);
s32 cellSailPlayerSetRepeatMode(u32 pSelf, s32 repeatMode, u32 pCommand);

/* Graphics adapter: the title's frame buffers, and the picture to show */
s32 cellSailGraphicsAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg);
s32 cellSailGraphicsAdapterFinalize(u32 pSelf);
s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat);
s32 cellSailGraphicsAdapterGetFrame(u32 pSelf, u32 pInfo);
s32 cellSailGraphicsAdapterGetFrame2(u32 pSelf, u32 pInfo, u32 pPrevInfo,
                                     u32 pFlipTime, u64 flags);
s32 cellSailGraphicsAdapterGetFormat(u32 pSelf, u32 pFormat);
s32 cellSailGraphicsAdapterUpdateAvSync(u32 pSelf, u64 pts);
s32 cellSailGraphicsAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 pTime);

/* Sound adapter: the decoded sound, pulled by the title's own mixer */
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg);
s32 cellSailSoundAdapterFinalize(u32 pSelf);
s32 cellSailSoundAdapterSetPreferredFormat(u32 pSelf, u32 pFormat);
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo);
s32 cellSailSoundAdapterGetFormat(u32 pSelf, u32 pFormat);
s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u64 pts);
s32 cellSailSoundAdapterPtsToTimePosition(u32 pSelf, u64 pts, u32 pTime);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SAIL_H */
