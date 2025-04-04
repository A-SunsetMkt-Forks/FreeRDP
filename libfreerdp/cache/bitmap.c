/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Bitmap Cache V2
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <stdio.h>

#include <winpr/crt.h>
#include <winpr/assert.h>
#include <winpr/cast.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>
#include <winpr/stream.h>

#include <freerdp/log.h>
#include <freerdp/gdi/bitmap.h>

#include "../gdi/gdi.h"
#include "../core/graphics.h"

#include "bitmap.h"
#include "cache.h"

#define TAG FREERDP_TAG("cache.bitmap")

static rdpBitmap* bitmap_cache_get(rdpBitmapCache* bitmapCache, UINT32 id, UINT32 index);
static BOOL bitmap_cache_put(rdpBitmapCache* bitmapCache, UINT32 id, UINT32 index,
                             rdpBitmap* bitmap);

static BOOL update_gdi_memblt(rdpContext* context, MEMBLT_ORDER* memblt)
{
	rdpBitmap* bitmap = NULL;
	rdpCache* cache = NULL;

	cache = context->cache;

	if (memblt->cacheId == 0xFF)
		bitmap = offscreen_cache_get(cache->offscreen, memblt->cacheIndex);
	else
		bitmap = bitmap_cache_get(cache->bitmap, (BYTE)memblt->cacheId, memblt->cacheIndex);

	/* XP-SP2 servers sometimes ask for cached bitmaps they've never defined. */
	if (bitmap == NULL)
		return TRUE;

	memblt->bitmap = bitmap;
	return IFCALLRESULT(TRUE, cache->bitmap->MemBlt, context, memblt);
}

static BOOL update_gdi_mem3blt(rdpContext* context, MEM3BLT_ORDER* mem3blt)
{
	rdpBitmap* bitmap = NULL;
	rdpCache* cache = context->cache;
	rdpBrush* brush = &mem3blt->brush;
	BOOL ret = TRUE;

	if (mem3blt->cacheId == 0xFF)
		bitmap = offscreen_cache_get(cache->offscreen, mem3blt->cacheIndex);
	else
		bitmap = bitmap_cache_get(cache->bitmap, (BYTE)mem3blt->cacheId, mem3blt->cacheIndex);

	/* XP-SP2 servers sometimes ask for cached bitmaps they've never defined. */
	if (!bitmap)
		return TRUE;

	const BYTE style = WINPR_ASSERTING_INT_CAST(UINT8, brush->style);

	if (brush->style & CACHED_BRUSH)
	{
		brush->data = brush_cache_get(cache->brush, brush->index, &brush->bpp);

		if (!brush->data)
			return FALSE;

		brush->style = 0x03;
	}

	mem3blt->bitmap = bitmap;
	IFCALLRET(cache->bitmap->Mem3Blt, ret, context, mem3blt);
	brush->style = style;
	return ret;
}

static BOOL update_gdi_cache_bitmap(rdpContext* context, const CACHE_BITMAP_ORDER* cacheBitmap)
{
	rdpBitmap* bitmap = NULL;
	rdpBitmap* prevBitmap = NULL;
	rdpCache* cache = context->cache;
	bitmap = Bitmap_Alloc(context);

	if (!bitmap)
		return FALSE;

	if (!Bitmap_SetDimensions(bitmap, WINPR_ASSERTING_INT_CAST(UINT16, cacheBitmap->bitmapWidth),
	                          WINPR_ASSERTING_INT_CAST(UINT16, cacheBitmap->bitmapHeight)))
		goto fail;

	if (!bitmap->Decompress(context, bitmap, cacheBitmap->bitmapDataStream,
	                        cacheBitmap->bitmapWidth, cacheBitmap->bitmapHeight,
	                        cacheBitmap->bitmapBpp, cacheBitmap->bitmapLength,
	                        cacheBitmap->compressed, RDP_CODEC_ID_NONE))
		goto fail;

	if (!bitmap->New(context, bitmap))
		goto fail;

	prevBitmap = bitmap_cache_get(cache->bitmap, cacheBitmap->cacheId, cacheBitmap->cacheIndex);
	Bitmap_Free(context, prevBitmap);
	return bitmap_cache_put(cache->bitmap, cacheBitmap->cacheId, cacheBitmap->cacheIndex, bitmap);

fail:
	Bitmap_Free(context, bitmap);
	return FALSE;
}

static BOOL update_gdi_cache_bitmap_v2(rdpContext* context, CACHE_BITMAP_V2_ORDER* cacheBitmapV2)

{
	rdpBitmap* prevBitmap = NULL;
	rdpCache* cache = context->cache;
	rdpSettings* settings = context->settings;
	rdpBitmap* bitmap = Bitmap_Alloc(context);

	if (!bitmap)
		return FALSE;

	const UINT32 ColorDepth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
	bitmap->key64 = ((UINT64)cacheBitmapV2->key1 | (((UINT64)cacheBitmapV2->key2) << 32));

	if (!cacheBitmapV2->bitmapBpp)
		cacheBitmapV2->bitmapBpp = ColorDepth;

	if ((ColorDepth == 15) && (cacheBitmapV2->bitmapBpp == 16))
		cacheBitmapV2->bitmapBpp = ColorDepth;

	if (!Bitmap_SetDimensions(bitmap, WINPR_ASSERTING_INT_CAST(UINT16, cacheBitmapV2->bitmapWidth),
	                          WINPR_ASSERTING_INT_CAST(UINT16, cacheBitmapV2->bitmapHeight)))
		goto fail;

	if (!bitmap->Decompress(context, bitmap, cacheBitmapV2->bitmapDataStream,
	                        cacheBitmapV2->bitmapWidth, cacheBitmapV2->bitmapHeight,
	                        cacheBitmapV2->bitmapBpp, cacheBitmapV2->bitmapLength,
	                        cacheBitmapV2->compressed, RDP_CODEC_ID_NONE))
		goto fail;

	prevBitmap = bitmap_cache_get(cache->bitmap, cacheBitmapV2->cacheId, cacheBitmapV2->cacheIndex);

	if (!bitmap->New(context, bitmap))
		goto fail;

	Bitmap_Free(context, prevBitmap);
	return bitmap_cache_put(cache->bitmap, cacheBitmapV2->cacheId, cacheBitmapV2->cacheIndex,
	                        bitmap);

fail:
	Bitmap_Free(context, bitmap);
	return FALSE;
}

static BOOL update_gdi_cache_bitmap_v3(rdpContext* context, CACHE_BITMAP_V3_ORDER* cacheBitmapV3)
{
	rdpBitmap* bitmap = NULL;
	rdpBitmap* prevBitmap = NULL;
	BOOL compressed = TRUE;
	rdpCache* cache = context->cache;
	rdpSettings* settings = context->settings;
	BITMAP_DATA_EX* bitmapData = &cacheBitmapV3->bitmapData;
	bitmap = Bitmap_Alloc(context);

	if (!bitmap)
		return FALSE;

	const UINT32 ColorDepth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
	bitmap->key64 = ((UINT64)cacheBitmapV3->key1 | (((UINT64)cacheBitmapV3->key2) << 32));

	if (!cacheBitmapV3->bpp)
		cacheBitmapV3->bpp = ColorDepth;

	compressed = (bitmapData->codecID != RDP_CODEC_ID_NONE);

	if (!Bitmap_SetDimensions(bitmap, WINPR_ASSERTING_INT_CAST(UINT16, bitmapData->width),
	                          WINPR_ASSERTING_INT_CAST(UINT16, bitmapData->height)))
		goto fail;

	if (!bitmap->Decompress(context, bitmap, bitmapData->data, bitmapData->width,
	                        bitmapData->height, bitmapData->bpp, bitmapData->length, compressed,
	                        bitmapData->codecID))
		goto fail;

	if (!bitmap->New(context, bitmap))
		goto fail;

	prevBitmap = bitmap_cache_get(cache->bitmap, cacheBitmapV3->cacheId, cacheBitmapV3->cacheIndex);
	Bitmap_Free(context, prevBitmap);
	return bitmap_cache_put(cache->bitmap, cacheBitmapV3->cacheId, cacheBitmapV3->cacheIndex,
	                        bitmap);

fail:
	Bitmap_Free(context, bitmap);
	return FALSE;
}

rdpBitmap* bitmap_cache_get(rdpBitmapCache* bitmapCache, UINT32 id, UINT32 index)
{
	rdpBitmap* bitmap = NULL;

	if (id >= bitmapCache->maxCells)
	{
		WLog_ERR(TAG, "get invalid bitmap cell id: %" PRIu32 "", id);
		return NULL;
	}

	if (index == BITMAP_CACHE_WAITING_LIST_INDEX)
	{
		index = bitmapCache->cells[id].number;
	}
	else if (index > bitmapCache->cells[id].number)
	{
		WLog_ERR(TAG, "get invalid bitmap index %" PRIu32 " in cell id: %" PRIu32 "", index, id);
		return NULL;
	}

	bitmap = bitmapCache->cells[id].entries[index];
	return bitmap;
}

BOOL bitmap_cache_put(rdpBitmapCache* bitmapCache, UINT32 id, UINT32 index, rdpBitmap* bitmap)
{
	if (id > bitmapCache->maxCells)
	{
		WLog_ERR(TAG, "put invalid bitmap cell id: %" PRIu32 "", id);
		return FALSE;
	}

	if (index == BITMAP_CACHE_WAITING_LIST_INDEX)
	{
		index = bitmapCache->cells[id].number;
	}
	else if (index > bitmapCache->cells[id].number)
	{
		WLog_ERR(TAG, "put invalid bitmap index %" PRIu32 " in cell id: %" PRIu32 "", index, id);
		return FALSE;
	}

	bitmapCache->cells[id].entries[index] = bitmap;
	return TRUE;
}

void bitmap_cache_register_callbacks(rdpUpdate* update)
{
	rdpCache* cache = NULL;

	WINPR_ASSERT(update);
	WINPR_ASSERT(update->context);
	WINPR_ASSERT(update->context->cache);

	cache = update->context->cache;
	WINPR_ASSERT(cache);

	if (!freerdp_settings_get_bool(update->context->settings, FreeRDP_DeactivateClientDecoding))
	{
		cache->bitmap->MemBlt = update->primary->MemBlt;
		cache->bitmap->Mem3Blt = update->primary->Mem3Blt;
		update->primary->MemBlt = update_gdi_memblt;
		update->primary->Mem3Blt = update_gdi_mem3blt;
		update->secondary->CacheBitmap = update_gdi_cache_bitmap;
		update->secondary->CacheBitmapV2 = update_gdi_cache_bitmap_v2;
		update->secondary->CacheBitmapV3 = update_gdi_cache_bitmap_v3;
		update->BitmapUpdate = gdi_bitmap_update;
	}
}

static int bitmap_cache_save_persistent(rdpBitmapCache* bitmapCache)
{
	rdpContext* context = bitmapCache->context;
	rdpSettings* settings = context->settings;

	const UINT32 version = freerdp_settings_get_uint32(settings, FreeRDP_BitmapCacheVersion);

	if (version != 2)
		return 0; /* persistent bitmap cache already saved in egfx channel */

	if (!freerdp_settings_get_bool(settings, FreeRDP_BitmapCachePersistEnabled))
		return 0;

	const char* BitmapCachePersistFile =
	    freerdp_settings_get_string(settings, FreeRDP_BitmapCachePersistFile);
	if (!BitmapCachePersistFile)
		return 0;

	rdpPersistentCache* persistent = persistent_cache_new();

	if (!persistent)
		return -1;

	int status = persistent_cache_open(persistent, BitmapCachePersistFile, TRUE, version);

	if (status < 1)
		goto end;

	if (bitmapCache->cells)
	{
		for (UINT32 i = 0; i < bitmapCache->maxCells; i++)
		{
			BITMAP_V2_CELL* cell = &bitmapCache->cells[i];
			for (UINT32 j = 0; j < cell->number + 1 && cell->entries; j++)
			{
				PERSISTENT_CACHE_ENTRY cacheEntry = { 0 };
				rdpBitmap* bitmap = cell->entries[j];

				if (!bitmap || !bitmap->key64)
					continue;

				cacheEntry.key64 = bitmap->key64;

				cacheEntry.width = WINPR_ASSERTING_INT_CAST(UINT16, bitmap->width);
				cacheEntry.height = WINPR_ASSERTING_INT_CAST(UINT16, bitmap->height);
				const UINT64 size = 4ULL * bitmap->width * bitmap->height;
				if (size > UINT32_MAX)
					continue;
				cacheEntry.size = (UINT32)size;
				cacheEntry.flags = 0;
				cacheEntry.data = bitmap->data;

				if (persistent_cache_write_entry(persistent, &cacheEntry) < 1)
				{
					status = -1;
					goto end;
				}
			}
		}
	}

	status = 1;

end:
	persistent_cache_free(persistent);
	return status;
}

rdpBitmapCache* bitmap_cache_new(rdpContext* context)
{
	rdpSettings* settings = NULL;
	rdpBitmapCache* bitmapCache = NULL;

	WINPR_ASSERT(context);

	settings = context->settings;
	WINPR_ASSERT(settings);

	bitmapCache = (rdpBitmapCache*)calloc(1, sizeof(rdpBitmapCache));

	if (!bitmapCache)
		return NULL;

	const UINT32 BitmapCacheV2NumCells =
	    freerdp_settings_get_uint32(settings, FreeRDP_BitmapCacheV2NumCells);
	bitmapCache->context = context;
	bitmapCache->cells = (BITMAP_V2_CELL*)calloc(BitmapCacheV2NumCells, sizeof(BITMAP_V2_CELL));

	if (!bitmapCache->cells)
		goto fail;
	bitmapCache->maxCells = BitmapCacheV2NumCells;

	for (UINT32 i = 0; i < bitmapCache->maxCells; i++)
	{
		const BITMAP_CACHE_V2_CELL_INFO* info =
		    freerdp_settings_get_pointer_array(settings, FreeRDP_BitmapCacheV2CellInfo, i);
		BITMAP_V2_CELL* cell = &bitmapCache->cells[i];
		UINT32 nr = info->numEntries;
		/* allocate an extra entry for BITMAP_CACHE_WAITING_LIST_INDEX */
		cell->entries = (rdpBitmap**)calloc((nr + 1), sizeof(rdpBitmap*));

		if (!cell->entries)
			goto fail;
		cell->number = nr;
	}

	return bitmapCache;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	bitmap_cache_free(bitmapCache);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

void bitmap_cache_free(rdpBitmapCache* bitmapCache)
{
	if (!bitmapCache)
		return;

	bitmap_cache_save_persistent(bitmapCache);

	if (bitmapCache->cells)
	{
		for (UINT32 i = 0; i < bitmapCache->maxCells; i++)
		{
			UINT32 j = 0;
			BITMAP_V2_CELL* cell = &bitmapCache->cells[i];

			if (!cell->entries)
				continue;

			for (j = 0; j < cell->number + 1; j++)
			{
				rdpBitmap* bitmap = cell->entries[j];
				Bitmap_Free(bitmapCache->context, bitmap);
			}

			free((void*)cell->entries);
		}

		free(bitmapCache->cells);
	}

	persistent_cache_free(bitmapCache->persistent);

	free(bitmapCache);
}

static void free_bitmap_data(BITMAP_DATA* data, size_t count)
{
	if (!data)
		return;

	for (size_t x = 0; x < count; x++)
		free(data[x].bitmapDataStream);

	free(data);
}

static BITMAP_DATA* copy_bitmap_data(const BITMAP_DATA* data, size_t count)
{
	BITMAP_DATA* dst = (BITMAP_DATA*)calloc(count, sizeof(BITMAP_DATA));

	if (!dst)
		goto fail;

	for (size_t x = 0; x < count; x++)
	{
		dst[x] = data[x];

		if (data[x].bitmapLength > 0)
		{
			dst[x].bitmapDataStream = malloc(data[x].bitmapLength);

			if (!dst[x].bitmapDataStream)
				goto fail;

			memcpy(dst[x].bitmapDataStream, data[x].bitmapDataStream, data[x].bitmapLength);
		}
	}

	return dst;
fail:
	free_bitmap_data(dst, count);
	return NULL;
}

void free_bitmap_update(WINPR_ATTR_UNUSED rdpContext* context,
                        WINPR_ATTR_UNUSED BITMAP_UPDATE* pointer)
{
	if (!pointer)
		return;

	free_bitmap_data(pointer->rectangles, pointer->number);
	free(pointer);
}

BITMAP_UPDATE* copy_bitmap_update(rdpContext* context, const BITMAP_UPDATE* pointer)
{
	BITMAP_UPDATE* dst = calloc(1, sizeof(BITMAP_UPDATE));

	if (!dst || !pointer)
		goto fail;

	*dst = *pointer;
	dst->rectangles = copy_bitmap_data(pointer->rectangles, pointer->number);

	if (!dst->rectangles)
		goto fail;

	return dst;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	free_bitmap_update(context, dst);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

CACHE_BITMAP_ORDER* copy_cache_bitmap_order(rdpContext* context, const CACHE_BITMAP_ORDER* order)
{
	CACHE_BITMAP_ORDER* dst = calloc(1, sizeof(CACHE_BITMAP_ORDER));

	if (!dst || !order)
		goto fail;

	*dst = *order;

	if (order->bitmapLength > 0)
	{
		dst->bitmapDataStream = malloc(order->bitmapLength);

		if (!dst->bitmapDataStream)
			goto fail;

		memcpy(dst->bitmapDataStream, order->bitmapDataStream, order->bitmapLength);
	}

	return dst;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	free_cache_bitmap_order(context, dst);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

void free_cache_bitmap_order(WINPR_ATTR_UNUSED rdpContext* context, CACHE_BITMAP_ORDER* order)
{
	if (order)
		free(order->bitmapDataStream);

	free(order);
}

CACHE_BITMAP_V2_ORDER* copy_cache_bitmap_v2_order(rdpContext* context,
                                                  const CACHE_BITMAP_V2_ORDER* order)
{
	CACHE_BITMAP_V2_ORDER* dst = calloc(1, sizeof(CACHE_BITMAP_V2_ORDER));

	if (!dst || !order)
		goto fail;

	*dst = *order;

	if (order->bitmapLength > 0)
	{
		dst->bitmapDataStream = malloc(order->bitmapLength);

		if (!dst->bitmapDataStream)
			goto fail;

		memcpy(dst->bitmapDataStream, order->bitmapDataStream, order->bitmapLength);
	}

	return dst;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	free_cache_bitmap_v2_order(context, dst);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

void free_cache_bitmap_v2_order(WINPR_ATTR_UNUSED rdpContext* context,
                                WINPR_ATTR_UNUSED CACHE_BITMAP_V2_ORDER* order)
{
	if (order)
		free(order->bitmapDataStream);

	free(order);
}

CACHE_BITMAP_V3_ORDER* copy_cache_bitmap_v3_order(rdpContext* context,
                                                  const CACHE_BITMAP_V3_ORDER* order)
{
	CACHE_BITMAP_V3_ORDER* dst = calloc(1, sizeof(CACHE_BITMAP_V3_ORDER));

	if (!dst || !order)
		goto fail;

	*dst = *order;

	if (order->bitmapData.length > 0)
	{
		dst->bitmapData.data = malloc(order->bitmapData.length);

		if (!dst->bitmapData.data)
			goto fail;

		memcpy(dst->bitmapData.data, order->bitmapData.data, order->bitmapData.length);
	}

	return dst;
fail:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	free_cache_bitmap_v3_order(context, dst);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

void free_cache_bitmap_v3_order(WINPR_ATTR_UNUSED rdpContext* context, CACHE_BITMAP_V3_ORDER* order)
{
	if (order)
		free(order->bitmapData.data);

	free(order);
}
