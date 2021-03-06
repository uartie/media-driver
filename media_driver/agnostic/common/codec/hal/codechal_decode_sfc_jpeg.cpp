/*
* Copyright (c) 2017, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file     codechal_decode_sfc_jpeg.cpp
//! \brief    Implements the decode interface extension for CSC and scaling via SFC for jpeg decoder.
//! \details  Downsampling in this case is supported by the SFC fixed function HW unit.
//!

#include "codechal_decode_sfc_jpeg.h"

MOS_STATUS CODECHAL_JPEG_SFC_STATE::CheckAndInitialize(
        PMOS_SURFACE            destSurface,
        CodecDecodeJpegPicParams*  picParams)
{
    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_HW_FUNCTION_ENTER;

    // Check if SFC output is supported
    if (MEDIA_IS_SKU(pHwInterface->GetSkuTable(), FtrSFCPipe)        &&
        destSurface->Format == Format_A8R8G8B8 &&   // Currently only support this SFC usage in JPEG
        (picParams->m_interleavedData ||   // SFC only support interleaved single scan (YUV400 is excluded for "interleaved" limitation)
            picParams->m_chromaType == jpegYUV400) &&
        picParams->m_totalScans == 1)
    {
        // Create the suedo SFC input surface (fill only the parameters will be used)
        sSfcInSurface.dwWidth = destSurface->dwWidth;
        sSfcInSurface.dwHeight = destSurface->dwHeight;
        sSfcInSurface.dwPitch = MOS_ALIGN_CEIL(destSurface->dwWidth, CODECHAL_SURFACE_PITCH_ALIGNMENT);
        sSfcInSurface.UPlaneOffset.iYOffset = destSurface->dwHeight;
        sSfcInSurface.TileType = destSurface->TileType;

        bSfcPipeOut = true;

        switch (picParams->m_chromaType)
        {
        case jpegYUV400:
            sSfcInSurface.Format = Format_400P;
            break;
        case jpegYUV420:
            sSfcInSurface.Format = Format_IMC3;
            sSfcInSurface.VPlaneOffset.iYOffset =
                MOS_ALIGN_CEIL(destSurface->dwHeight, MHW_VDBOX_MFX_UV_PLANE_ALIGNMENT_LEGACY) + (destSurface->dwHeight >> 1);
            break;
        case jpegYUV422H2Y:
        case jpegYUV422H4Y:
            sSfcInSurface.Format = Format_422H;
            sSfcInSurface.VPlaneOffset.iYOffset =
                MOS_ALIGN_CEIL(destSurface->dwHeight, MHW_VDBOX_MFX_UV_PLANE_ALIGNMENT_LEGACY) + (destSurface->dwHeight >> 1);
            break;
        case jpegYUV444:
        case jpegRGB:
        case jpegBGR:
            sSfcInSurface.Format = Format_444P;
            sSfcInSurface.VPlaneOffset.iYOffset =
                MOS_ALIGN_CEIL(destSurface->dwHeight, MHW_VDBOX_MFX_UV_PLANE_ALIGNMENT_LEGACY) + destSurface->dwHeight;
            break;
        default:
            bSfcPipeOut = false;
        }

        if (bSfcPipeOut)
        {
            CODECHAL_DECODE_PROCESSING_PARAMS   procParams;
            MOS_ZeroMemory(&procParams, sizeof(CODECHAL_DECODE_PROCESSING_PARAMS));
            procParams.pInputSurface = &sSfcInSurface;
            procParams.pOutputSurface = destSurface;
            procParams.rcInputSurfaceRegion.Width = sSfcInSurface.dwWidth;
            procParams.rcInputSurfaceRegion.Height = sSfcInSurface.dwHeight;
            procParams.rcOutputSurfaceRegion.Width = destSurface->dwWidth;
            procParams.rcOutputSurfaceRegion.Height = destSurface->dwHeight;

            if (IsSfcOutputSupported(&procParams, MhwSfcInterface::SFC_PIPE_MODE_VDBOX))
            {
                bJpegInUse = true;
                ucJpegChromaType = picParams->m_chromaType;

                CODECHAL_HW_CHK_STATUS_RETURN(Initialize(
                    &procParams,
                    MhwSfcInterface::SFC_PIPE_MODE_VDBOX));

                // Use SFC for Direct YUV->ARGB on SKL
                bSfcPipeOut = true;
            }
            else
            {
                bSfcPipeOut = false;
            }
        }
    }

    // Sanity check - supposed to have been done in Media SDK
    if (!bSfcPipeOut && destSurface->Format == Format_A8R8G8B8)
    {
        CODECHAL_HW_ASSERTMESSAGE("SFC YUV->RGB Unsupported.");
        return MOS_STATUS_UNKNOWN;
    }

    return eStatus;
}

MOS_STATUS CODECHAL_JPEG_SFC_STATE::UpdateInputInfo(
    PMHW_SFC_STATE_PARAMS   sfcStateParams)
{
    MOS_STATUS eStatus = MOS_STATUS_SUCCESS;

    CODECHAL_HW_FUNCTION_ENTER;

    sfcStateParams->sfcPipeMode                = MEDIASTATE_SFC_PIPE_VD_TO_SFC;
    sfcStateParams->dwAVSFilterMode            = MEDIASTATE_SFC_AVS_FILTER_5x5;

    uint16_t widthAlignUnit                             = CODECHAL_SFC_ALIGNMENT_16;
    uint16_t heightAlignUnit                            = CODECHAL_SFC_ALIGNMENT_16;

    switch (ucJpegChromaType)
    {
    case jpegYUV400:
        sfcStateParams->dwVDVEInputOrderingMode    = MEDIASTATE_SFC_INPUT_ORDERING_VD_8x8_JPEG;
        sfcStateParams->dwInputChromaSubSampling   = MEDIASTATE_SFC_CHROMA_SUBSAMPLING_400;
        widthAlignUnit                     = CODECHAL_SFC_ALIGNMENT_8;
        heightAlignUnit                    = CODECHAL_SFC_ALIGNMENT_8;
        break;
    case jpegYUV420:
        sfcStateParams->dwVDVEInputOrderingMode    = MEDIASTATE_SFC_INPUT_ORDERING_VD_16x16_JPEG;
        sfcStateParams->dwInputChromaSubSampling   = MEDIASTATE_SFC_CHROMA_SUBSAMPLING_420;
        break;
    case jpegYUV422H2Y:
        sfcStateParams->dwVDVEInputOrderingMode    = MEDIASTATE_SFC_INPUT_ORDERING_VD_8x8_JPEG;
        sfcStateParams->dwInputChromaSubSampling   = MEDIASTATE_SFC_CHROMA_SUBSAMPLING_422H;
        widthAlignUnit                     = CODECHAL_SFC_ALIGNMENT_8;
        heightAlignUnit                    = CODECHAL_SFC_ALIGNMENT_8;
        break;
    case jpegYUV422H4Y:
        sfcStateParams->dwVDVEInputOrderingMode    = MEDIASTATE_SFC_INPUT_ORDERING_VD_16x16_JPEG;
        sfcStateParams->dwInputChromaSubSampling   = MEDIASTATE_SFC_CHROMA_SUBSAMPLING_422H;
        break;
    case jpegYUV444:
    case jpegRGB:
    case jpegBGR:
        sfcStateParams->dwVDVEInputOrderingMode    = MEDIASTATE_SFC_INPUT_ORDERING_VD_8x8_JPEG;
        sfcStateParams->dwInputChromaSubSampling   = MEDIASTATE_SFC_CHROMA_SUBSAMPLING_444;
        widthAlignUnit                     = CODECHAL_SFC_ALIGNMENT_8;
        heightAlignUnit                    = CODECHAL_SFC_ALIGNMENT_8;
        break;
    default:
        CODECHAL_HW_ASSERTMESSAGE("Unsupported input format of SFC.");
        return MOS_STATUS_UNKNOWN;
    }

    sfcStateParams->dwInputFrameWidth          = MOS_ALIGN_CEIL(pInputSurface->dwWidth, widthAlignUnit);
    sfcStateParams->dwInputFrameHeight         = MOS_ALIGN_CEIL(pInputSurface->dwHeight, heightAlignUnit);

    return eStatus;
}
