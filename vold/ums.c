
/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <errno.h>

#include "vold.h"
#include "ums.h"

static boolean host_connected = false;
static boolean ums_enabled = false;

int ums_bootstrap(void)
{
    return 0;
}

void ums_enabled_set(boolean enabled)
{
    ums_enabled = enabled;
    send_msg(enabled ? VOLD_EVT_UMS_ENABLED : VOLD_EVT_UMS_DISABLED);
}

boolean ums_enabled_get()
{
    return ums_enabled;
}

void ums_hostconnected_set(boolean connected)
{
    LOG_VOL("ums_hostconnected_set(%d):\n", connected);
    host_connected = connected;

    if (!connected)
        ums_enabled_set(false);
    send_msg(connected ? VOLD_EVT_UMS_CONNECTED : VOLD_EVT_UMS_DISCONNECTED);
}

int ums_enable(char *dev_fspath, char *lun_syspath)
{
    LOG_VOL("ums_enable(%s, %s):\n", dev_fspath, lun_syspath);

    int fd;
    char filename[255];

    sprintf(filename, "/sys/%s/file", lun_syspath);
    if ((fd = open(filename, O_WRONLY)) < 0) {
        LOGE("Unable to open '%s' (%s)\n", filename, strerror(errno));
        return -errno;
    }

    if (write(fd, dev_fspath, strlen(dev_fspath)) < 0) {
        LOGE("Unable to write to ums lunfile (%s)\n", strerror(errno));
        close(fd);
        return -errno;
    }
    
    close(fd);
    return 0;
}

int ums_disable(char *lun_syspath)
{
    LOG_VOL("ums_disable(%s):\n", lun_syspath);

    int fd;
    char filename[255];

    sprintf(filename, "/sys/%s/file", lun_syspath);
    if ((fd = open(filename, O_WRONLY)) < 0) {
        LOGE("Unable to open '%s' (%s)\n", filename, strerror(errno));
        return -errno;
    }

    char ch = 0;

    if (write(fd, &ch, 1) < 0) {
        LOGE("Unable to write to ums lunfile (%s)\n", strerror(errno));
        close(fd);
        return -errno;
    }
    
    close(fd);
    return 0;
}

boolean ums_hostconnected_get(void)
{
    return host_connected;
}

int ums_send_status(void)
{
    int rc;

    LOG_VOL("ums_send_status():\n");

    rc = send_msg(ums_enabled_get() ? VOLD_EVT_UMS_ENABLED :
                                      VOLD_EVT_UMS_DISABLED);
    if (rc < 0)
        return rc;

    rc = send_msg(ums_hostconnected_get() ? VOLD_EVT_UMS_CONNECTED :
                                            VOLD_EVT_UMS_DISCONNECTED);

    return rc;
}
