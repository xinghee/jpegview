
#include "stdafx.h"
#include "TJPEGWrapper.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "MaxImageDef.h"

// Very large images (a 389 MP panorama, for instance) are decoded at full size
// today, which costs over a gigabyte of memory and several seconds - even though
// the window only ever shows a small part of them. libjpeg-turbo can emit a
// reduced-size image straight out of the IDCT (1/2 .. 1/8), far cheaper than
// decoding everything and resampling afterwards.
//
// The target is twice the virtual screen in each direction, so "fit to window"
// stays sharp and moderate zooming still has pixels to spend. Images that are
// already small enough are left completely untouched.
// Set JPEGVIEW_NO_DOWNSAMPLE=1 to force full-size decoding.
static void ApplyDecodeScaling(tjhandle hDecoder, int &width, int &height) {
	char env[8];
	env[0] = '\0';
	if (GetEnvironmentVariableA("JPEGVIEW_NO_DOWNSAMPLE", env, sizeof(env)) > 0 && env[0] == '1') {
		return;
	}

	int targetW = GetSystemMetrics(SM_CXVIRTUALSCREEN) * 2;
	int targetH = GetSystemMetrics(SM_CYVIRTUALSCREEN) * 2;
	if (targetW <= 0 || targetH <= 0) {
		targetW = GetSystemMetrics(SM_CXSCREEN) * 2;
		targetH = GetSystemMetrics(SM_CYSCREEN) * 2;
	}
	if (targetW <= 0 || targetH <= 0) return;
	if (width <= targetW && height <= targetH) return;

	int count = 0;
	tjscalingfactor *factors = tj3GetScalingFactors(&count);
	if (factors == NULL || count <= 0) return;

	// Pick the biggest reduction that still covers the target rectangle.
	// Areas are compared as double: MAX_IMAGE_PIXELS allows up to 65535x65535,
	// which overflows a 32-bit long on Windows.
	tjscalingfactor chosen = TJUNSCALED;
	double bestArea = (double)width * (double)height;
	bool found = false;
	for (int i = 0; i < count; i++) {
		int w = TJSCALED(width, factors[i]);
		int h = TJSCALED(height, factors[i]);
		if (w >= targetW && h >= targetH) {
			double area = (double)w * (double)h;
			if (!found || area < bestArea) {
				chosen = factors[i];
				bestArea = area;
				found = true;
			}
		}
	}
	if (!found) return;
	if (chosen.num >= chosen.denom) return;

	if (tj3SetScalingFactor(hDecoder, chosen) == 0) {
		width = TJSCALED(width, chosen);
		height = TJSCALED(height, chosen);
	}
}

void * TurboJpeg::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   TJSAMP &chromoSubsampling,
					   bool &outOfMemory,
					   const void *buffer,
					   int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 3;
	chromoSubsampling = TJSAMP_420;

	tjhandle hDecoder = tj3Init(TJINIT_DECOMPRESS);
	if (hDecoder == NULL) {
		return NULL;
	}

	unsigned char* pPixelData = NULL;
	int nResult = tj3DecompressHeader(hDecoder, (unsigned char*)buffer, sizebytes);
	if (nResult == 0) {
		width = tj3Get(hDecoder, TJPARAM_JPEGWIDTH);
		height = tj3Get(hDecoder, TJPARAM_JPEGHEIGHT);
		chromoSubsampling = (TJSAMP)tj3Get(hDecoder, TJPARAM_SUBSAMP);
		ApplyDecodeScaling(hDecoder, width, height);
		// Compared as double: abs() would truncate to int and can go negative
		// for images allowed by MAX_IMAGE_PIXELS (up to 65535x65535).
		if ((double)width * (double)height > (double)MAX_IMAGE_PIXELS) {
			outOfMemory = true;
		} else if (width <= MAX_IMAGE_DIMENSION && height <= MAX_IMAGE_DIMENSION && chromoSubsampling != TJSAMP_UNKNOWN) {
			pPixelData = new(std::nothrow) unsigned char[TJPAD(width * 3) * height];
			if (pPixelData != NULL) {
				nResult = tj3Decompress8(hDecoder, (unsigned char*)buffer, sizebytes, pPixelData, TJPAD(width * 3), TJPF_BGR);
				if (nResult != 0) {
					delete[] pPixelData;
					pPixelData = NULL;
				}
			} else {
				outOfMemory = true;
			}
		}
	}

	tj3Destroy(hDecoder);

	return pPixelData;
}

void * TurboJpeg::Compress(const void *source,
					  int width,
					  int height,
					  int &len,
					  bool &outOfMemory,
					  int quality)
{
	outOfMemory = false;
	len = 0;
	tjhandle hEncoder = tj3Init(TJINIT_COMPRESS);
	if (hEncoder == NULL) {
		return NULL;
	}

	unsigned char* pJPEGCompressed = NULL;
	size_t nCompressedLen = 0;
	tj3Set(hEncoder, TJPARAM_SUBSAMP, TJSAMP_420);
	tj3Set(hEncoder, TJPARAM_QUALITY, quality);
	int nResult = tj3Compress8(hEncoder, (unsigned char*)source, width, TJPAD(width * 3), height, TJPF_BGR,
		&pJPEGCompressed, &nCompressedLen);
	if (nResult != 0 || nCompressedLen > INT_MAX) {
		if (pJPEGCompressed == NULL) {
			outOfMemory = true;
		}
		Free(pJPEGCompressed);
		pJPEGCompressed = NULL;
	}

	len = nCompressedLen;

	tj3Destroy(hEncoder);

	return pJPEGCompressed;
}

void TurboJpeg::Free(void* buffer) {
	tj3Free(buffer);
}
