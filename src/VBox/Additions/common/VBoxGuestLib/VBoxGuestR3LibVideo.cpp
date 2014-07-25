/* $Id$ */
/** @file
 * VBoxGuestR3Lib - Ring-3 Support Library for VirtualBox guest additions, Video.
 */

/*
 * Copyright (C) 2007-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "VBGLR3Internal.h"

#include <VBox/log.h>
#include <VBox/HostServices/GuestPropertySvc.h>  /* For Save and RetrieveVideoMode */
#include <iprt/assert.h>
#ifndef VBOX_VBGLR3_XFREE86
# include <iprt/mem.h>
#endif
#include <iprt/string.h>

#include <stdio.h>

#ifdef VBOX_VBGLR3_XFREE86
/* Rather than try to resolve all the header file conflicts, I will just
   prototype what we need here. */
extern "C" void* xf86memcpy(void*,const void*,xf86size_t);
# undef memcpy
# define memcpy xf86memcpy
extern "C" void* xf86memset(const void*,int,xf86size_t);
# undef memset
# define memset xf86memset
#endif /* VBOX_VBGLR3_XFREE86 */

#define VIDEO_PROP_PREFIX "/VirtualBox/GuestAdd/Vbgl/Video/"

/**
 * Enable or disable video acceleration.
 *
 * @returns VBox status code.
 *
 * @param   fEnable       Pass zero to disable, any other value to enable.
 */
VBGLR3DECL(int) VbglR3VideoAccelEnable(bool fEnable)
{
    VMMDevVideoAccelEnable Req;
    vmmdevInitRequest(&Req.header, VMMDevReq_VideoAccelEnable);
    Req.u32Enable = fEnable;
    Req.cbRingBuffer = VBVA_RING_BUFFER_SIZE;
    Req.fu32Status = 0;
    return vbglR3GRPerform(&Req.header);
}


/**
 * Flush the video buffer.
 *
 * @returns VBox status code.
 */
VBGLR3DECL(int) VbglR3VideoAccelFlush(void)
{
    VMMDevVideoAccelFlush Req;
    vmmdevInitRequest(&Req.header, VMMDevReq_VideoAccelFlush);
    return vbglR3GRPerform(&Req.header);
}


/**
 * Send mouse pointer shape information to the host.
 *
 * @returns VBox status code.
 *
 * @param   fFlags      Mouse pointer flags.
 * @param   xHot        X coordinate of hot spot.
 * @param   yHot        Y coordinate of hot spot.
 * @param   cx          Pointer width.
 * @param   cy          Pointer height.
 * @param   pvImg       Pointer to the image data (can be NULL).
 * @param   cbImg       Size of the image data pointed to by pvImg.
 */
VBGLR3DECL(int) VbglR3SetPointerShape(uint32_t fFlags, uint32_t xHot, uint32_t yHot, uint32_t cx, uint32_t cy, const void *pvImg, size_t cbImg)
{
    VMMDevReqMousePointer *pReq;
    size_t cbReq = vmmdevGetMousePointerReqSize(cx, cy);
    AssertReturn(   !pvImg
                 || cbReq == RT_OFFSETOF(VMMDevReqMousePointer, pointerData) + cbImg,
                 VERR_INVALID_PARAMETER);
    int rc = vbglR3GRAlloc((VMMDevRequestHeader **)&pReq, cbReq,
                           VMMDevReq_SetPointerShape);
    if (RT_SUCCESS(rc))
    {
        pReq->fFlags = fFlags;
        pReq->xHot = xHot;
        pReq->yHot = yHot;
        pReq->width = cx;
        pReq->height = cy;
        if (pvImg)
            memcpy(pReq->pointerData, pvImg, cbImg);

        rc = vbglR3GRPerform(&pReq->header);
        if (RT_SUCCESS(rc))
            rc = pReq->header.rc;
        vbglR3GRFree(&pReq->header);
    }
    return rc;
}


/**
 * Send mouse pointer shape information to the host.
 * This version of the function accepts a request for clients that
 * already allocate and manipulate the request structure directly.
 *
 * @returns VBox status code.
 *
 * @param   pReq        Pointer to the VMMDevReqMousePointer structure.
 */
VBGLR3DECL(int) VbglR3SetPointerShapeReq(VMMDevReqMousePointer *pReq)
{
    int rc = vbglR3GRPerform(&pReq->header);
    if (RT_SUCCESS(rc))
        rc = pReq->header.rc;
    return rc;
}
/**
 * Query the last display change request sent from the host to the guest.
 *
 * @returns iprt status value
 * @param   pcx         Where to store the horizontal pixel resolution
 *                      requested (a value of zero means do not change).
 * @param   pcy         Where to store the vertical pixel resolution
 *                      requested (a value of zero means do not change).
 * @param   pcBits      Where to store the bits per pixel requested (a value
 *                      of zero means do not change).
 * @param   iDisplay    Where to store the display number the request was for
 *                      - 0 for the primary display, 1 for the first
 *                      secondary display, etc.
 * @param   fAck        whether or not to acknowledge the newest request sent by
 *                      the host.  If this is set, the function will return the
 *                      most recent host request, otherwise it will return the
 *                      last request to be acknowledged.
 *
 * @param   pcOriginX   New horizontal position of the secondary monitor.
 * @param   pcOriginY   New vertical position of the secondary monitor.
 * param    pfEnabled   Secondary monitor is enabled or not.
 *
 */
VBGLR3DECL(int) VbglR3GetDisplayChangeRequestEx(uint32_t *pcx, uint32_t *pcy, uint32_t *pcBits,
                                                uint32_t *piDisplay, uint32_t *pcOriginX, uint32_t *pcOriginY,
                                                bool *pfEnabled, bool fAck)
{
    VMMDevDisplayChangeRequestEx Req;
    int rc = VINF_SUCCESS;
    AssertPtrReturn(pcx, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcy, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcBits, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcOriginX, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcOriginY, VERR_INVALID_PARAMETER);
    AssertPtrReturn(piDisplay, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pfEnabled, VERR_INVALID_PARAMETER);
    RT_ZERO(Req);
    rc = vmmdevInitRequest(&Req.header, VMMDevReq_GetDisplayChangeRequestEx);
    if (RT_FAILURE(rc))
    {
        LogRelFlowFunc(("DisplayChangeRequest Extended not supported. Can't Init the Req.\n"));
        return rc;
    }

    if (fAck)
        Req.eventAck = VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST;
    rc = vbglR3GRPerform(&Req.header);
    if (RT_SUCCESS(rc))
        rc = Req.header.rc;
    if (RT_SUCCESS(rc))
    {
        *pcx = Req.xres;
        *pcy = Req.yres;
        *pcBits = Req.bpp;
        *piDisplay = Req.display;
        *pcOriginX = Req.cxOrigin;
        *pcOriginY = Req.cyOrigin;
        *pfEnabled = Req.fEnabled;
        LogRel(("VbglR3GetDisplayChangeRequestEx: pcx=%d pcy=%d display=%d orgX=%d orgY=%d and Enabled=%d\n",
                 *pcx, *pcy, *piDisplay, *pcOriginX, *pcOriginY, *pfEnabled));
    }
    return rc;
}


/**
 * Query the last display change request sent from the host to the guest.
 *
 * @returns iprt status value
 * @param   pcx         Where to store the horizontal pixel resolution
 * @param   pcy         Where to store the vertical pixel resolution
 *                      requested (a value of zero means do not change).
 * @param   pcBits      Where to store the bits per pixel requested (a value
 *                      of zero means do not change).
 * @param   iDisplay    Where to store the display number the request was for
 *                      - 0 for the primary display, 1 for the first
 *                      secondary display, etc.
 * @param   fAck        whether or not to acknowledge the newest request sent by
 *                      the host.  If this is set, the function will return the
 *                      most recent host request, otherwise it will return the
 *                      last request to be acknowledged.
 *
 */
VBGLR3DECL(int) VbglR3GetDisplayChangeRequest(uint32_t *pcx, uint32_t *pcy, uint32_t *pcBits, uint32_t *piDisplay, bool fAck)
{
    VMMDevDisplayChangeRequest2 Req;

    AssertPtrReturn(pcx, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcy, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcBits, VERR_INVALID_PARAMETER);
    AssertPtrReturn(piDisplay, VERR_INVALID_PARAMETER);
    RT_ZERO(Req);
    vmmdevInitRequest(&Req.header, VMMDevReq_GetDisplayChangeRequest2);
    if (fAck)
        Req.eventAck = VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST;
    int rc = vbglR3GRPerform(&Req.header);
    if (RT_SUCCESS(rc))
        rc = Req.header.rc;
    if (RT_SUCCESS(rc))
    {
        *pcx = Req.xres;
        *pcy = Req.yres;
        *pcBits = Req.bpp;
        *piDisplay = Req.display;
    }
    return rc;
}


/**
 * Query the host as to whether it likes a specific video mode.
 *
 * @returns the result of the query
 * @param   cx     the width of the mode being queried
 * @param   cy     the height of the mode being queried
 * @param   cBits  the bpp of the mode being queried
 */
VBGLR3DECL(bool) VbglR3HostLikesVideoMode(uint32_t cx, uint32_t cy, uint32_t cBits)
{
    bool fRc = true;  /* If for some reason we can't contact the host then
                       * we like everything. */
    int rc;
    VMMDevVideoModeSupportedRequest req;

    vmmdevInitRequest(&req.header, VMMDevReq_VideoModeSupported);
    req.width      = cx;
    req.height     = cy;
    req.bpp        = cBits;
    req.fSupported = true;
    rc = vbglR3GRPerform(&req.header);
    if (RT_SUCCESS(rc) && RT_SUCCESS(req.header.rc))
        fRc = req.fSupported;
    return fRc;
}

/**
 * Get the highest screen number for which there is a saved video mode or "0"
 * if there are no saved modes.
 *
 * @returns iprt status value
 * @param   pcScreen   where to store the virtual screen number
 */
VBGLR3DECL(int) VbglR3VideoModeGetHighestSavedScreen(unsigned *pcScreen)
{
#if defined(VBOX_WITH_GUEST_PROPS)
    using namespace guestProp;

    int rc, rc2 = VERR_UNRESOLVED_ERROR;
    uint32_t u32ClientId = 0;
    const char *pszPattern = VIDEO_PROP_PREFIX"*";
    PVBGLR3GUESTPROPENUM pHandle = NULL;
    const char *pszName;
    unsigned cHighestScreen = 0;

    AssertPtrReturn(pcScreen, VERR_INVALID_POINTER);
    rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_SUCCESS(rc))
        rc = VbglR3GuestPropEnum(u32ClientId, &pszPattern, 1, &pHandle,
                                 &pszName, NULL, NULL, NULL);
    if (u32ClientId != 0)
        rc2 = VbglR3GuestPropDisconnect(u32ClientId);
    if (RT_SUCCESS(rc))
        rc = rc2;
    while (pszName != NULL && RT_SUCCESS(rc))
    {
        uint32_t cScreen;

        rc = RTStrToUInt32Full(pszName + sizeof(VIDEO_PROP_PREFIX) - 1, 10,
                               &cScreen);
        if (RT_SUCCESS(rc))  /* There may be similar properties with text. */
            cHighestScreen = RT_MAX(cHighestScreen, cScreen);
        rc = VbglR3GuestPropEnumNext(pHandle, &pszName, NULL, NULL, NULL);
    }
    VbglR3GuestPropEnumFree(pHandle);
    if (RT_SUCCESS(rc))
        *pcScreen = cHighestScreen;
    return rc;
#else /* !VBOX_WITH_GUEST_PROPS */
    return VERR_NOT_IMPLEMENTED;
#endif /* !VBOX_WITH_GUEST_PROPS */
}

/**
 * Save video mode parameters to the guest property store.
 *
 * @returns iprt status value
 * @param   cScreen   virtual screen number
 * @param   cx        mode width
 * @param   cy        mode height
 * @param   cBits     bits per pixel for the mode
 * @param   x         virtual screen X offset
 * @param   y         virtual screen Y offset
 * @param   fEnabled  is this virtual screen enabled?
 */
VBGLR3DECL(int) VbglR3SaveVideoMode(unsigned cScreen, unsigned cx, unsigned cy,
                                    unsigned cBits, unsigned x, unsigned y,
                                    bool fEnabled)
{
#if defined(VBOX_WITH_GUEST_PROPS)
    using namespace guestProp;

    char szModeName[MAX_NAME_LEN];
    char szModeParms[MAX_VALUE_LEN];
    uint32_t u32ClientId = 0;
    unsigned cx2, cy2, cBits2, x2, y2, cHighestScreen, cHighestScreen2;
    bool fEnabled2;
    int rc, rc2 = VERR_UNRESOLVED_ERROR;

    rc = VbglR3VideoModeGetHighestSavedScreen(&cHighestScreen);
    RTStrPrintf(szModeName, sizeof(szModeName), VIDEO_PROP_PREFIX"%u", cScreen);
    RTStrPrintf(szModeParms, sizeof(szModeParms), "%ux%ux%u,%ux%u,%u", cx, cy,
                cBits, x, y, (unsigned) fEnabled);
    if (RT_SUCCESS(rc))
        rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_SUCCESS(rc))
        rc = VbglR3GuestPropWriteValue(u32ClientId, szModeName, szModeParms);
    if (u32ClientId != 0)
        rc2 = VbglR3GuestPropDisconnect(u32ClientId);
    if (RT_SUCCESS(rc))
        rc = rc2;
    /* Sanity check 1.  We do not try to make allowance for someone else
     * changing saved settings at the same time as us. */
    if (RT_SUCCESS(rc))
    {
        rc = VbglR3RetrieveVideoMode(cScreen, &cx2, &cy2, &cBits2, &x2, &y2,
                                     &fEnabled2);
        if (   RT_SUCCESS(rc)
            && (   cx != cx2 || cy != cy2 || cBits != cBits2
                || x != x2 || y != y2 || fEnabled != fEnabled2))
            rc = VERR_WRITE_ERROR;
    }
    /* Sanity check 2.  Same comment. */
    if (RT_SUCCESS(rc))
        rc = VbglR3VideoModeGetHighestSavedScreen(&cHighestScreen2);
    if (RT_SUCCESS(rc))
        if (cHighestScreen2 != RT_MAX(cHighestScreen, cScreen))
            rc = VERR_INTERNAL_ERROR;
    return rc;
#else /* !VBOX_WITH_GUEST_PROPS */
    return VERR_NOT_IMPLEMENTED;
#endif /* !VBOX_WITH_GUEST_PROPS */
}


/**
 * Retrieve video mode parameters from the guest property store.
 *
 * @returns iprt status value
 * @param   cScreen    the virtual screen number
 * @param   pcx        where to store the mode width
 * @param   pcy        where to store the mode height
 * @param   pcBits     where to store the bits per pixel for the mode
 * @param   px         where to store the virtual screen X offset
 * @param   py         where to store the virtual screen Y offset
 * @param   pfEnabled  where to store whether this virtual screen is enabled
 */
VBGLR3DECL(int) VbglR3RetrieveVideoMode(unsigned cScreen,
                                        unsigned *pcx, unsigned *pcy,
                                        unsigned *pcBits,
                                        unsigned *px, unsigned *py,
                                        bool *pfEnabled)
{
#if defined(VBOX_WITH_GUEST_PROPS)
    using namespace guestProp;

/*
 * First we retrieve the video mode which is saved as a string in the
 * guest property store.
 */
    /* The buffer for VbglR3GuestPropReadValue.  If this is too small then
     * something is wrong with the data stored in the property. */
    char szModeName[MAX_NAME_LEN];
    char szModeParms[1024];
    uint32_t u32ClientId = 0;
    int cMatches;
    unsigned cx, cy, cBits, x, y, fEnabled;
    int rc, rc2 = VERR_UNRESOLVED_ERROR;

    /** @todo add a VbglR3GuestPropReadValueF/FV that does the RTStrPrintf for you. */
    RTStrPrintf(szModeName, sizeof(szModeName), VIDEO_PROP_PREFIX"%u", cScreen);
    rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_SUCCESS(rc))
        rc = VbglR3GuestPropReadValue(u32ClientId, szModeName, szModeParms,
                                      sizeof(szModeParms), NULL);
    if (u32ClientId != 0)
        rc2 = VbglR3GuestPropDisconnect(u32ClientId);
    if (RT_SUCCESS(rc))
        rc = rc2;

/*
 * Now we convert the string returned to numeric values.
 */
    cMatches = sscanf(szModeParms, "%ux%ux%u,%ux%u,%u\n", &cx, &cy, &cBits, &x,
                      &y, &fEnabled);
    if (cMatches == 6)
        rc = VINF_SUCCESS;
    else if (cMatches < 0)
        rc = VERR_READ_ERROR;
    else
        rc = VERR_PARSE_ERROR;
/*
 * And clean up and return the values if we successfully obtained them.
 */
    if (RT_SUCCESS(rc))
    {
        if (pcx)
            *pcx = cx;
        if (pcy)
            *pcy = cy;
        if (pcBits)
            *pcBits = cBits;
        if (px)
            *px = x;
        if (py)
            *py = y;
        if (pfEnabled)
            *pfEnabled = RT_BOOL(fEnabled);
    }
    return rc;
#else /* !VBOX_WITH_GUEST_PROPS */
    return VERR_NOT_IMPLEMENTED;
#endif /* !VBOX_WITH_GUEST_PROPS */
}
