/* FreeRDP: A Remote Desktop Protocol Client
 * Optimized Color conversion operations.
 * vi:ts=4 sw=4:
 *
 * Copyright 2011 Stephen Erisman
 * Copyright 2011 Norbert Federa <norbert.federa@thincast.com>
 * Copyright 2011 Martin Fleisz <martin.fleisz@thincast.com>
 * (c) Copyright 2012 Hewlett-Packard Development Company, L.P.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0.
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <freerdp/config.h>

#include <freerdp/types.h>
#include <freerdp/primitives.h>
#include <winpr/sysinfo.h>

#include "prim_internal.h"
#include "prim_colors.h"

/*---------------------------------------------------------------------------*/
#if defined(NEON_INTRINSICS_ENABLED)
#include <arm_neon.h>

static primitives_t* generic = NULL;

static pstatus_t neon_yCbCrToRGB_16s8u_P3AC4R_X(const INT16* WINPR_RESTRICT pSrc[3], UINT32 srcStep,
                                                BYTE* WINPR_RESTRICT pDst, UINT32 dstStep,
                                                const prim_size_t* WINPR_RESTRICT roi, uint8_t rPos,
                                                uint8_t gPos, uint8_t bPos, uint8_t aPos)
{
	BYTE* pRGB = pDst;
	const INT16* pY = pSrc[0];
	const INT16* pCb = pSrc[1];
	const INT16* pCr = pSrc[2];
	const size_t srcPad = (srcStep - (roi->width * sizeof(INT16))) / sizeof(INT16);
	const size_t dstPad = (dstStep - (roi->width * 4)) / 4;
	const size_t pad = roi->width % 8;
	const int16x4_t c4096 = vdup_n_s16(4096);

	for (UINT32 y = 0; y < roi->height; y++)
	{
		for (UINT32 x = 0; x < roi->width - pad; x += 8)
		{
			const int16x8_t Y = vld1q_s16(pY);
			const int16x4_t Yh = vget_high_s16(Y);
			const int16x4_t Yl = vget_low_s16(Y);
			const int32x4_t YhAdd = vaddl_s16(Yh, c4096); /* Y + 4096 */
			const int32x4_t YlAdd = vaddl_s16(Yl, c4096); /* Y + 4096 */
			const int32x4_t YhW = vshlq_n_s32(YhAdd, 16);
			const int32x4_t YlW = vshlq_n_s32(YlAdd, 16);
			const int16x8_t Cr = vld1q_s16(pCr);
			const int16x4_t Crh = vget_high_s16(Cr);
			const int16x4_t Crl = vget_low_s16(Cr);
			const int16x8_t Cb = vld1q_s16(pCb);
			const int16x4_t Cbh = vget_high_s16(Cb);
			const int16x4_t Cbl = vget_low_s16(Cb);
			uint8x8x4_t bgrx;
			{
				/* R */
				const int32x4_t CrhR = vmulq_n_s32(vmovl_s16(Crh), 91916); /* 1.402525 * 2^16 */
				const int32x4_t CrlR = vmulq_n_s32(vmovl_s16(Crl), 91916); /* 1.402525 * 2^16 */
				const int32x4_t CrhRa = vaddq_s32(CrhR, YhW);
				const int32x4_t CrlRa = vaddq_s32(CrlR, YlW);
				const int16x4_t Rsh = vmovn_s32(vshrq_n_s32(CrhRa, 21));
				const int16x4_t Rsl = vmovn_s32(vshrq_n_s32(CrlRa, 21));
				const int16x8_t Rs = vcombine_s16(Rsl, Rsh);
				bgrx.val[rPos] = vqmovun_s16(Rs);
			}
			{
				/* G */
				const int32x4_t CbGh = vmull_n_s16(Cbh, 22527);            /* 0.343730 * 2^16 */
				const int32x4_t CbGl = vmull_n_s16(Cbl, 22527);            /* 0.343730 * 2^16 */
				const int32x4_t CrGh = vmulq_n_s32(vmovl_s16(Crh), 46819); /* 0.714401 * 2^16 */
				const int32x4_t CrGl = vmulq_n_s32(vmovl_s16(Crl), 46819); /* 0.714401 * 2^16 */
				const int32x4_t CbCrGh = vaddq_s32(CbGh, CrGh);
				const int32x4_t CbCrGl = vaddq_s32(CbGl, CrGl);
				const int32x4_t YCbCrGh = vsubq_s32(YhW, CbCrGh);
				const int32x4_t YCbCrGl = vsubq_s32(YlW, CbCrGl);
				const int16x4_t Gsh = vmovn_s32(vshrq_n_s32(YCbCrGh, 21));
				const int16x4_t Gsl = vmovn_s32(vshrq_n_s32(YCbCrGl, 21));
				const int16x8_t Gs = vcombine_s16(Gsl, Gsh);
				const uint8x8_t G = vqmovun_s16(Gs);
				bgrx.val[gPos] = G;
			}
			{
				/* B */
				const int32x4_t CbBh = vmulq_n_s32(vmovl_s16(Cbh), 115992); /* 1.769905 * 2^16 */
				const int32x4_t CbBl = vmulq_n_s32(vmovl_s16(Cbl), 115992); /* 1.769905 * 2^16 */
				const int32x4_t YCbBh = vaddq_s32(CbBh, YhW);
				const int32x4_t YCbBl = vaddq_s32(CbBl, YlW);
				const int16x4_t Bsh = vmovn_s32(vshrq_n_s32(YCbBh, 21));
				const int16x4_t Bsl = vmovn_s32(vshrq_n_s32(YCbBl, 21));
				const int16x8_t Bs = vcombine_s16(Bsl, Bsh);
				const uint8x8_t B = vqmovun_s16(Bs);
				bgrx.val[bPos] = B;
			}
			/* A */
			{
				bgrx.val[aPos] = vdup_n_u8(0xFF);
			}
			vst4_u8(pRGB, bgrx);
			pY += 8;
			pCb += 8;
			pCr += 8;
			pRGB += 32;
		}

		for (UINT32 x = 0; x < pad; x++)
		{
			const INT32 divisor = 16;
			const INT32 Y = ((*pY++) + 4096) << divisor;
			const INT32 Cb = (*pCb++);
			const INT32 Cr = (*pCr++);
			const INT32 CrR = Cr * (INT32)(1.402525f * (1 << divisor));
			const INT32 CrG = Cr * (INT32)(0.714401f * (1 << divisor));
			const INT32 CbG = Cb * (INT32)(0.343730f * (1 << divisor));
			const INT32 CbB = Cb * (INT32)(1.769905f * (1 << divisor));
			INT16 R = ((INT16)((CrR + Y) >> divisor) >> 5);
			INT16 G = ((INT16)((Y - CbG - CrG) >> divisor) >> 5);
			INT16 B = ((INT16)((CbB + Y) >> divisor) >> 5);
			BYTE bgrx[4];
			bgrx[bPos] = CLIP(B);
			bgrx[gPos] = CLIP(G);
			bgrx[rPos] = CLIP(R);
			bgrx[aPos] = 0xFF;
			*pRGB++ = bgrx[0];
			*pRGB++ = bgrx[1];
			*pRGB++ = bgrx[2];
			*pRGB++ = bgrx[3];
		}

		pY += srcPad;
		pCb += srcPad;
		pCr += srcPad;
		pRGB += dstPad;
	}

	return PRIMITIVES_SUCCESS;
}

static pstatus_t neon_yCbCrToRGB_16s8u_P3AC4R(const INT16* WINPR_RESTRICT pSrc[3], UINT32 srcStep,
                                              BYTE* WINPR_RESTRICT pDst, UINT32 dstStep,
                                              UINT32 DstFormat,
                                              const prim_size_t* WINPR_RESTRICT roi)
{
	switch (DstFormat)
	{
		case PIXEL_FORMAT_BGRA32:
		case PIXEL_FORMAT_BGRX32:
			return neon_yCbCrToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 2, 1, 0, 3);

		case PIXEL_FORMAT_RGBA32:
		case PIXEL_FORMAT_RGBX32:
			return neon_yCbCrToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 0, 1, 2, 3);

		case PIXEL_FORMAT_ARGB32:
		case PIXEL_FORMAT_XRGB32:
			return neon_yCbCrToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 1, 2, 3, 0);

		case PIXEL_FORMAT_ABGR32:
		case PIXEL_FORMAT_XBGR32:
			return neon_yCbCrToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 3, 2, 1, 0);

		default:
			return generic->yCbCrToRGB_16s8u_P3AC4R(pSrc, srcStep, pDst, dstStep, DstFormat, roi);
	}
}

static pstatus_t
neon_RGBToRGB_16s8u_P3AC4R_X(const INT16* WINPR_RESTRICT pSrc[3], /* 16-bit R,G, and B arrays */
                             UINT32 srcStep,            /* bytes between rows in source data */
                             BYTE* WINPR_RESTRICT pDst, /* 32-bit interleaved ARGB (ABGR?) data */
                             UINT32 dstStep,            /* bytes between rows in dest data */
                             const prim_size_t* WINPR_RESTRICT roi, /* region of interest */
                             uint8_t rPos, uint8_t gPos, uint8_t bPos, uint8_t aPos)
{
	UINT32 pad = roi->width % 8;

	for (UINT32 y = 0; y < roi->height; y++)
	{
		const INT16* pr = (const INT16*)(((BYTE*)pSrc[0]) + y * srcStep);
		const INT16* pg = (const INT16*)(((BYTE*)pSrc[1]) + y * srcStep);
		const INT16* pb = (const INT16*)(((BYTE*)pSrc[2]) + y * srcStep);
		BYTE* dst = pDst + y * dstStep;

		for (UINT32 x = 0; x < roi->width - pad; x += 8)
		{
			int16x8_t r = vld1q_s16(pr);
			int16x8_t g = vld1q_s16(pg);
			int16x8_t b = vld1q_s16(pb);
			uint8x8x4_t bgrx;
			bgrx.val[aPos] = vdup_n_u8(0xFF);
			bgrx.val[rPos] = vqmovun_s16(r);
			bgrx.val[gPos] = vqmovun_s16(g);
			bgrx.val[bPos] = vqmovun_s16(b);
			vst4_u8(dst, bgrx);
			pr += 8;
			pg += 8;
			pb += 8;
			dst += 32;
		}

		for (UINT32 x = 0; x < pad; x++)
		{
			BYTE bgrx[4];
			bgrx[bPos] = *pb++;
			bgrx[gPos] = *pg++;
			bgrx[rPos] = *pr++;
			bgrx[aPos] = 0xFF;
			*dst++ = bgrx[0];
			*dst++ = bgrx[1];
			*dst++ = bgrx[2];
			*dst++ = bgrx[3];
		}
	}

	return PRIMITIVES_SUCCESS;
}

static pstatus_t
neon_RGBToRGB_16s8u_P3AC4R(const INT16* WINPR_RESTRICT pSrc[3], /* 16-bit R,G, and B arrays */
                           UINT32 srcStep,            /* bytes between rows in source data */
                           BYTE* WINPR_RESTRICT pDst, /* 32-bit interleaved ARGB (ABGR?) data */
                           UINT32 dstStep,            /* bytes between rows in dest data */
                           UINT32 DstFormat,
                           const prim_size_t* WINPR_RESTRICT roi) /* region of interest */
{
	switch (DstFormat)
	{
		case PIXEL_FORMAT_BGRA32:
		case PIXEL_FORMAT_BGRX32:
			return neon_RGBToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 2, 1, 0, 3);

		case PIXEL_FORMAT_RGBA32:
		case PIXEL_FORMAT_RGBX32:
			return neon_RGBToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 0, 1, 2, 3);

		case PIXEL_FORMAT_ARGB32:
		case PIXEL_FORMAT_XRGB32:
			return neon_RGBToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 1, 2, 3, 0);

		case PIXEL_FORMAT_ABGR32:
		case PIXEL_FORMAT_XBGR32:
			return neon_RGBToRGB_16s8u_P3AC4R_X(pSrc, srcStep, pDst, dstStep, roi, 3, 2, 1, 0);

		default:
			return generic->RGBToRGB_16s8u_P3AC4R(pSrc, srcStep, pDst, dstStep, DstFormat, roi);
	}
}
#endif /* NEON_INTRINSICS_ENABLED */

/* ------------------------------------------------------------------------- */
void primitives_init_colors_neon_int(primitives_t* WINPR_RESTRICT prims)
{
#if defined(NEON_INTRINSICS_ENABLED)
	generic = primitives_get_generic();

	WLog_VRB(PRIM_TAG, "NEON optimizations");
	prims->RGBToRGB_16s8u_P3AC4R = neon_RGBToRGB_16s8u_P3AC4R;
	prims->yCbCrToRGB_16s8u_P3AC4R = neon_yCbCrToRGB_16s8u_P3AC4R;
#else
	WLog_VRB(PRIM_TAG, "undefined WITH_SIMD or neon intrinsics not available");
	WINPR_UNUSED(prims);
#endif
}
