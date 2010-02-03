#pragma once

#include "../chunk_s.h"

/*! @brief MAIN chunk. */
struct MainChunk_s : Chunk_s {
  struct MapTile_s {
    int32_t flags;
    void *async_object;
  };

  MapTile_s map_tile[64][64];

  MainChunk_s(int32_t memAbsOffset, void *in_buf) {
    int32_t buf_addr = reinterpret_cast<int32_t>(in_buf);
    void *dest_addr = reinterpret_cast<void*>(buf_addr + memAbsOffset);

    /* fill MPHD chunk with its real content */
    memcpy(this, dest_addr, sizeof(MphdChunk_s));
  }
};
