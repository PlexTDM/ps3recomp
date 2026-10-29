/*
 * ps3recomp - RSX Vertex Format Mapping
 *
 * Maps RSX vertex attribute types to host API formats.
 * RSX uses NV-style vertex attribute encoding from the NV4097 register.
 */

#ifndef PS3RECOMP_RSX_VERTEX_FORMATS_H
#define PS3RECOMP_RSX_VERTEX_FORMATS_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RSX vertex attribute type values (from NV4097_SET_VERTEX_DATA_ARRAY_FORMAT) */
#define RSX_VERTEX_TYPE_NONE           0  /* disabled */
#define RSX_VERTEX_TYPE_S1             1  /* signed normalized 16-bit */
#define RSX_VERTEX_TYPE_F              2  /* float32 */
#define RSX_VERTEX_TYPE_SF             3  /* half-float (float16) */
#define RSX_VERTEX_TYPE_UB             4  /* unsigned byte */
#define RSX_VERTEX_TYPE_S32K           5  /* signed 16-bit */
#define RSX_VERTEX_TYPE_CMP            6  /* compressed (11-11-10 packed) */
#define RSX_VERTEX_TYPE_UB256          7  /* unsigned byte (0-255, NOT normalized) */

/* Get the byte size of one component for an RSX vertex type */
static inline u32 rsx_vertex_type_size(u32 type)
{
    switch (type) {
    case RSX_VERTEX_TYPE_S1:     return 2;
    case RSX_VERTEX_TYPE_F:      return 4;
    case RSX_VERTEX_TYPE_SF:     return 2;
    case RSX_VERTEX_TYPE_UB:     return 1;
    case RSX_VERTEX_TYPE_S32K:   return 2;
    case RSX_VERTEX_TYPE_CMP:    return 4; /* packed */
    case RSX_VERTEX_TYPE_UB256:  return 1;
    default:                     return 0;
    }
}

/* Get the total byte size of a vertex attribute (type * components) */
static inline u32 rsx_vertex_attrib_size(u32 type, u32 num_components)
{
    if (type == RSX_VERTEX_TYPE_CMP) return 4; /* always 4 bytes packed */
    return rsx_vertex_type_size(type) * num_components;
}

/* Resolve one attribute's element under
 * NV4097_SET_FREQUENCY_DIVIDER_OPERATION. Frequency 0/1 is ordinary vertex
 * fetch, a set operation bit is modulo (instanced template), and a clear bit
 * is division (one per-instance datum shared by `frequency` vertices). */
static inline u32 rsx_vertex_element_index(u32 vertex_id, u32 base_index,
                                           u32 frequency, u32 modulo)
{
    if (frequency <= 1)
        return (vertex_id + base_index) & 0x000FFFFFu;
    if (modulo)
        return vertex_id % frequency;
    return vertex_id / frequency;
}

#ifdef __cplusplus
}
#endif
#endif
