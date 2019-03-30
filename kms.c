/*
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drmMode.h>
#include <xf86drm.h>

#include "kms.h"
#include "utils.h"

struct Config {
    uint32_t connectorID;
    uint32_t crtcID;
    int crtcIndex;
    uint32_t planeID;
    drmModeModeInfo mode;
    uint16_t width;
    uint16_t height;
};

struct PropertyIDs {

    struct {
        uint32_t mode_id;
        uint32_t active;
    } crtc;

    struct {
        uint32_t src_x;
        uint32_t src_y;
        uint32_t src_w;
        uint32_t src_h;
        uint32_t crtc_x;
        uint32_t crtc_y;
        uint32_t crtc_w;
        uint32_t crtc_h;
        uint32_t fb_id;
        uint32_t crtc_id;
    } plane;

    struct {
        uint32_t crtc_id;
    } connector;
};

struct PropertyIDAddresses {
    const char *name;
    uint32_t *ptr;
};

static int is_nvdc;

/*
 * Pick the first connected connector we find with usable modes and
 * CRTC.
 */
static void PickConnector(int drmFd,
                          drmModeResPtr pModeRes,
                          struct Config *pConfig)
{
    int i, j;

    Log("%s: mode has %u connectors, %u crtcs\n",
        __func__,
        pModeRes->count_connectors,
        pModeRes->count_crtcs);

    for (i = 0; i < pModeRes->count_connectors; i++) {

        drmModeConnectorPtr pConnector =
            drmModeGetConnector(drmFd, pModeRes->connectors[i]);

        Log("%s: connector %d of %d\n", __func__, i, pModeRes->count_connectors);

        if (pConnector == NULL) {
            Fatal("Unable to query DRM-KMS information for "
                  "connector index %d\n", i);
        }

        Log("   -- %d connection=%u, modes=%d, encoders=%d\n",
               i, pConnector->connection, pConnector->count_modes, pConnector->count_encoders);

        if ((pConnector->connection == DRM_MODE_CONNECTED) &&
            (pConnector->count_modes > 0) &&
            (pConnector->count_encoders > 0)) {

            drmModeEncoderPtr pEncoder =
                drmModeGetEncoder(drmFd, pConnector->encoders[0]);

            if (pEncoder == NULL) {
                Fatal("Unable to query DRM-KMS information for"
                      "encoder 0x%08x\n", pConnector->encoders[0]);
            }

            pConfig->connectorID = pModeRes->connectors[i];
            pConfig->mode = pConnector->modes[0];

            Log("    - encoder[0] possible_crtcs=0x%x\n", pEncoder->possible_crtcs);

            for (j = 0; j < pModeRes->count_crtcs; j++) {

                if ((pEncoder->possible_crtcs & (1 << j)) == 0) {
                    continue;
                }

                pConfig->crtcID = pModeRes->crtcs[j];
                pConfig->crtcIndex = j;
                break;
            }

            if (pConfig->crtcID == 0) {
                Fatal("Unable to select a suitable CRTC.\n");
            }

            drmModeFreeEncoder(pEncoder);
        }

        drmModeFreeConnector(pConnector);
    }

    if (pConfig->connectorID == 0) {
        Fatal("Could not find a suitable connector.\n");
    }

    if (pConfig->crtcID == 0) {
        Fatal("Could not find a suitable CRTC.\n");
    }
}


/*
 * Search for the specified property on the given object, and return
 * its value.
 */
static uint64_t GetPropertyValue(
    int drmFd,
    uint32_t objectID,
    uint32_t objectType,
    const char *propName)
{
    uint32_t i;
    int found = 0;
    uint64_t value = 0;
    drmModeObjectPropertiesPtr pModeObjectProperties =
        drmModeObjectGetProperties(drmFd, objectID, objectType);

    Log("%s: objectID=%u, objectType=0x%x has %d properties\n",
        __func__, objectID, objectType, pModeObjectProperties->count_props);

    for (i = 0; i < pModeObjectProperties->count_props; i++) {

        int j;
        drmModePropertyPtr pProperty =
            drmModeGetProperty(drmFd, pModeObjectProperties->props[i]);

        if (pProperty == NULL) {
            Fatal("Unable to query property.\n");
        }

        Log("      property %d: id=%u, flags=0x%x, name=%s, value count: %d enum count: %d blob count: %d\n",
            i, pProperty->prop_id, pProperty->flags, pProperty->name, pProperty->count_values,
               pProperty->count_enums, pProperty->count_blobs);
        for (j = 0; j < pProperty->count_values; j++)
            Log("        value[%d]=0x%lx\n", j, pProperty->values[j]);
        for (j = 0; j < pProperty->count_enums; j++)
            Log("        enum[%d]=(value: 0x%llx, name:%s)\n", j,
                pProperty->enums[j].value, pProperty->enums[j].name);

        if (strcmp(propName, pProperty->name) == 0) {
            value = pModeObjectProperties->prop_values[i];
            found = 1;
            Log("        -- found %s, value=0x%lx\n", propName, value);
        }

        drmModeFreeProperty(pProperty);

    }

    drmModeFreeObjectProperties(pModeObjectProperties);

    if (!found) {
        Fatal("Unable to find value for property \'%s\'.\n", propName);
    }

    return value;
}


/*
 * Pick a primary plane that can be used by the CRTC in the Config.
 */
static void PickPlane(int drmFd, struct Config *pConfig)
{
    drmModePlaneResPtr pPlaneRes = drmModeGetPlaneResources(drmFd);
    uint32_t i;

    if (pPlaneRes == NULL) {
        Fatal("Unable to query DRM-KMS plane resources\n");
    }

    Log("%s: found %d planes\n", __func__, pPlaneRes->count_planes);

    for (i = 0; i < pPlaneRes->count_planes; i++) {
        drmModePlanePtr pPlane = drmModeGetPlane(drmFd, pPlaneRes->planes[i]);
        uint32_t crtcs;
        uint64_t type;

        if (pPlane == NULL) {
            Fatal("Unable to query DRM-KMS plane %d\n", i);
        }

        Log("    -- plane %d: crtcs=0x%x\n", i, pPlane->possible_crtcs);
        Log("                 format count: %d\n", pPlane->count_formats);
        Log("                 crtc_id: %u\n", pPlane->crtc_id);
        Log("                 fb_id: %u\n", pPlane->fb_id);
        Log("                 plane_id: %u (%u)\n", pPlane->plane_id, pPlaneRes->planes[i]);

        crtcs = pPlane->possible_crtcs;

        drmModeFreePlane(pPlane);

        if ((crtcs & (1 << pConfig->crtcIndex)) == 0) {
            continue;
        }

        type = GetPropertyValue(drmFd, pPlaneRes->planes[i],
                                DRM_MODE_OBJECT_PLANE, "type");
        Log("                 type: %lu\n", type);

        if (type == DRM_PLANE_TYPE_PRIMARY) {
            pConfig->planeID = pPlaneRes->planes[i];
            Log("%s: choosing plane ID %u\n", __func__, pConfig->planeID);
        }
    }

    drmModeFreePlaneResources(pPlaneRes);

    if (pConfig->planeID == 0) {
        Fatal("Could not find a suitable plane.\n");
    }
}


/*
 * Pick a connector, CRTC, and plane to use for the modeset.
 */
static void PickConfig(int drmFd, struct Config *pConfig)
{
    drmModeResPtr pModeRes;
    int ret, i;

    ret = drmSetClientCap(drmFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    if (ret != 0) {
        Fatal("DRM_CLIENT_CAP_UNIVERSAL_PLANES not available.\n");
    }

    ret = drmSetClientCap(drmFd, DRM_CLIENT_CAP_ATOMIC, 1);

    if (ret != 0) {
        Fatal("DRM_CLIENT_CAP_ATOMIC not available.\n");
    }

    pModeRes = drmModeGetResources(drmFd);

    if (pModeRes == NULL) {
        Fatal("Unable to query DRM-KMS resources.\n");
    }

    Log("%s: FB count: %d, CRTC count: %d, Connector count: %d, Encoder count: %d\n", __func__,
           pModeRes->count_fbs, pModeRes->count_crtcs, pModeRes->count_connectors,
           pModeRes->count_encoders);
    for (i = 0; i < pModeRes->count_fbs; i++)
        Log("    fb[%d]: %u\n", i, pModeRes->fbs[i]);
    for (i = 0; i < pModeRes->count_crtcs; i++)
        Log("    crtc[%d]: %u\n", i, pModeRes->crtcs[i]);
    for (i = 0; i < pModeRes->count_connectors; i++)
        Log("    connector[%d]: %u\n", i, pModeRes->connectors[i]);
    for (i = 0; i < pModeRes->count_encoders; i++)
        Log("    encoder[%d]: %u\n", i, pModeRes->encoders[i]);
    PickConnector(drmFd, pModeRes, pConfig);

    if (!is_nvdc)
        PickPlane(drmFd, pConfig);

    drmModeFreeResources(pModeRes);

    pConfig->width = pConfig->mode.hdisplay;
    pConfig->height = pConfig->mode.vdisplay;
}


/*
 * Create an ID for the mode in the specified config.
 */
static uint32_t CreateModeID(int drmFd, const struct Config *pConfig)
{
    uint32_t modeID = 0;
    int ret = drmModeCreatePropertyBlob(drmFd,
                                        &pConfig->mode, sizeof(pConfig->mode),
                                        &modeID);
    if (ret != 0) {
        Fatal("Failed to create mode property.\n");
    }

    return modeID;
}


/*
 * Create a blank DRM fb object.
 */
static uint32_t CreateFb(int drmFd, const struct Config *pConfig)
{
    struct drm_mode_create_dumb createRequest = { 0 };
    struct drm_mode_map_dumb mapRequest = { 0 };
    uint8_t *map;
    uint32_t fb = 0;
    int ret;

    createRequest.width = pConfig->width;
    createRequest.height = pConfig->height;
    createRequest.bpp = 32;

    ret = drmIoctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &createRequest);
    if (ret < 0) {
        Fatal("Unable to create dumb buffer.\n");
    }

    ret = drmModeAddFB(drmFd, pConfig->width, pConfig->height, 24, 32,
                       createRequest.pitch, createRequest.handle, &fb);
    if (ret) {
        Fatal("Unable to add fb.\n");
    }

    mapRequest.handle = createRequest.handle;

    ret = drmIoctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapRequest);
    if (ret) {
        Fatal("Unable to map dumb buffer.\n");
    }

    if (is_nvdc)
        map = (uint8_t *) mapRequest.offset;
    else {
        map = mmap(0, createRequest.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   drmFd, mapRequest.offset);
        if (map == MAP_FAILED) {
            Fatal("Failed to mmap(2) fb.\n");
        }
    }

    memset(map, 0, createRequest.size);

    return fb;
}


/*
 * Query the properties for the specified object, and populate the IDs
 * in the given table.
 */
static void AssignPropertyIDsOneType(int drmFd,
                                     uint32_t objectID,
                                     uint32_t objectType,
                                     struct PropertyIDAddresses *table,
                                     size_t tableLen)
{
    uint32_t i;
    drmModeObjectPropertiesPtr pModeObjectProperties =
        drmModeObjectGetProperties(drmFd, objectID, objectType);

    if (pModeObjectProperties == NULL) {
        Fatal("Unable to query mode object properties.\n");
    }

    Log("%s: got %d properties for ID %d, type=0x%x\n",
        __func__, pModeObjectProperties->count_props, objectID, objectType);

    for (i = 0; i < pModeObjectProperties->count_props; i++) {

        uint32_t j;
        drmModePropertyPtr pProperty =
            drmModeGetProperty(drmFd, pModeObjectProperties->props[i]);

        if (pProperty == NULL) {
            Fatal("Unable to query property.\n");
        }

        for (j = 0; j < tableLen; j++) {
            if (strcmp(table[j].name, pProperty->name) == 0) {
                *(table[j].ptr) = pProperty->prop_id;
                Log("    - stored prop_id %d at %d for %s\n",
                    pProperty->prop_id, j, pProperty->name);
                break;
            }
        }

        drmModeFreeProperty(pProperty);
    }

    drmModeFreeObjectProperties(pModeObjectProperties);

    for (i = 0; i < tableLen; i++) {
        if (*(table[i].ptr) == 0) {
            Fatal("Unable to find property ID for \'%s\'.\n", table[i].name);
        }
    }
}


/*
 * Find the property IDs for the CRTC, plane, and connector in the
 * Config.
 */
static void AssignPropertyIDs(int drmFd,
                              const struct Config *pConfig,
                              struct PropertyIDs *pPropertyIDs)
{
    struct PropertyIDAddresses crtcTable[] = {
        { "MODE_ID", &pPropertyIDs->crtc.mode_id      },
        { "ACTIVE",  &pPropertyIDs->crtc.active       },
    };

    struct PropertyIDAddresses planeTable[] = {
        { "SRC_X",   &pPropertyIDs->plane.src_x       },
        { "SRC_Y",   &pPropertyIDs->plane.src_y       },
        { "SRC_W",   &pPropertyIDs->plane.src_w       },
        { "SRC_H",   &pPropertyIDs->plane.src_h       },
        { "CRTC_X",  &pPropertyIDs->plane.crtc_x      },
        { "CRTC_Y",  &pPropertyIDs->plane.crtc_y      },
        { "CRTC_W",  &pPropertyIDs->plane.crtc_w      },
        { "CRTC_H",  &pPropertyIDs->plane.crtc_h      },
        { "FB_ID",   &pPropertyIDs->plane.fb_id       },
        { "CRTC_ID", &pPropertyIDs->plane.crtc_id     },
    };

    struct PropertyIDAddresses connectorTable[] = {
        { "CRTC_ID", &pPropertyIDs->connector.crtc_id },
    };

    AssignPropertyIDsOneType(drmFd, pConfig->crtcID,
                             DRM_MODE_OBJECT_CRTC,
                             crtcTable, ARRAY_LEN(crtcTable));
    AssignPropertyIDsOneType(drmFd, pConfig->planeID,
                             DRM_MODE_OBJECT_PLANE,
                             planeTable, ARRAY_LEN(planeTable));
    AssignPropertyIDsOneType(drmFd, pConfig->connectorID,
                             DRM_MODE_OBJECT_CONNECTOR,
                             connectorTable, ARRAY_LEN(connectorTable));
}


/*
 * A KMS atomic request is made by "adding properties" to a
 * drmModeAtomicReqPtr object.
 *
 * Find the property IDs that we need to describe the request, then
 * add the properties to the request.
 */
static void AssignAtomicRequest(int drmFd,
                                drmModeAtomicReqPtr pAtomic,
                                const struct Config *pConfig,
                                uint32_t modeID, uint32_t fb)
{
    struct PropertyIDs propertyIDs = { 0 };

    AssignPropertyIDs(drmFd, pConfig, &propertyIDs);


    /* Specify the mode to use on the CRTC, and make the CRTC active. */

    drmModeAtomicAddProperty(pAtomic, pConfig->crtcID,
                             propertyIDs.crtc.mode_id, modeID);
    drmModeAtomicAddProperty(pAtomic, pConfig->crtcID,
                             propertyIDs.crtc.active, 1);

    /* Tell the connector to receive pixels from the CRTC. */

    drmModeAtomicAddProperty(pAtomic, pConfig->connectorID,
                             propertyIDs.connector.crtc_id, pConfig->crtcID);

    /*
     * Specify the region of source surface to display (i.e., the
     * "ViewPortIn").  Note these values are in 16.16 format, so shift
     * up by 16.
     */

    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.src_x, 0);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.src_y, 0);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.src_w, pConfig->width << 16);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.src_h, pConfig->height << 16);

    /*
     * Specify the region within the mode where the image should be
     * displayed (i.e., the "ViewPortOut").
     */

    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.crtc_x, 0);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.crtc_y, 0);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.crtc_w, pConfig->width);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.crtc_h, pConfig->height);

    /*
     * Specify the surface to display in the plane, and connect the
     * plane to the CRTC.
     *
     * XXX for EGLStreams purposes, it would be nice to have the
     * option of not specifying a surface at this point, as well as to
     * be able to have the KMS atomic modeset consume a frame from an
     * EGLStream.
     */

    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.fb_id, fb);
    drmModeAtomicAddProperty(pAtomic, pConfig->planeID,
                             propertyIDs.plane.crtc_id, pConfig->crtcID);
}


/*
 * Use the atomic DRM KMS API to set a mode on a CRTC.
 *
 * On success, return the non-zero ID of a DRM plane to which to
 * present, and its dimensions.  On failure, exit with a fatal error
 * message.
 */
void SetMode(int drmFd, uint32_t *pCrtcID, uint32_t *pPlaneID, int *pWidth, int *pHeight)
{
    struct Config config = { 0 };
    drmModeAtomicReqPtr pAtomic;
    uint32_t modeID, fb;
    int ret;
    const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;

    PickConfig(drmFd, &config);

    fb = CreateFb(drmFd, &config);
    if (is_nvdc) {
        ret = drmModeSetCrtc(drmFd, config.crtcID, fb, 0, 0,
                             &config.connectorID, 1, &config.mode);
        *pCrtcID = config.crtcID;
        *pPlaneID = 0;
    } else {
        modeID = CreateModeID(drmFd, &config);
        pAtomic = drmModeAtomicAlloc();

        AssignAtomicRequest(drmFd, pAtomic, &config, modeID, fb);

        ret = drmModeAtomicCommit(drmFd, pAtomic, flags, NULL /* user_data */);

        drmModeAtomicFree(pAtomic);
        *pCrtcID = 0;
        *pPlaneID = config.planeID;
    }

    if (ret != 0) {
        Fatal("Failed to set mode.\n");
    }

    *pWidth = config.width;
    *pHeight = config.height;
}

int DrmOpen(const char *deviceName)
{
    int fd = drmOpen(deviceName, "");
    if (fd >= 0)
        is_nvdc = 1;
    return fd;
}
