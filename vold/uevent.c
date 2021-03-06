
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

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>

#include "vold.h"
#include "uevent.h"
#include "mmc.h"
#include "blkdev.h"
#include "volmgr.h"
#include "media.h"

#define DEBUG_UEVENT 0

#define UEVENT_PARAMS_MAX 32

enum uevent_action { action_add, action_remove, action_change };

struct uevent {
    char *path;
    enum uevent_action action;
    char *subsystem;
    char *param[UEVENT_PARAMS_MAX];
    unsigned int seqnum;
};

struct uevent_dispatch {
    char *subsystem;
    int (* dispatch) (struct uevent *);
};

static void dump_uevent(struct uevent *);
static int dispatch_uevent(struct uevent *event);
static void free_uevent(struct uevent *event);
static char *get_uevent_param(struct uevent *event, char *param_name);

static int handle_powersupply_event(struct uevent *event);
static int handle_switch_event(struct uevent *);
static int handle_battery_event(struct uevent *);
static int handle_mmc_event(struct uevent *);
static int handle_block_event(struct uevent *);
static int handle_bdi_event(struct uevent *);
static void _cb_blkdev_ok_to_destroy(blkdev_t *dev);

static struct uevent_dispatch dispatch_table[] = {
    { "switch", handle_switch_event }, 
    { "battery", handle_battery_event }, 
    { "mmc", handle_mmc_event },
    { "block", handle_block_event },
    { "bdi", handle_bdi_event },
    { "power_supply", handle_powersupply_event },
    { NULL, NULL }
};

int process_uevent_message(int socket)
{
    char buffer[64 * 1024]; // Thank god we're not in the kernel :)
    int count;
    char *s = buffer;
    char *end;
    struct uevent *event;
    int param_idx = 0;
    int i;
    int first = 1;
    int rc = 0;

    if ((count = recv(socket, buffer, sizeof(buffer), 0)) < 0) {
        LOGE("Error receiving uevent (%s)\n", strerror(errno));
        return -errno;
    }

    if (!(event = malloc(sizeof(struct uevent)))) {
        LOGE("Error allocating memory (%s)\n", strerror(errno));
        return -errno;
    }

    memset(event, 0, sizeof(struct uevent));

    end = s + count;
    while (s < end) {
        if (first) {
            char *p;
            for (p = s; *p != '@'; p++);
            p++;
            event->path = strdup(p);
            first = 0;
        } else {
            if (!strncmp(s, "ACTION=", strlen("ACTION="))) {
                char *a = s + strlen("ACTION=");
               
                if (!strcmp(a, "add"))
                    event->action = action_add;
                else if (!strcmp(a, "change"))
                    event->action = action_change;
                else if (!strcmp(a, "remove"))
                    event->action = action_remove;
            } else if (!strncmp(s, "SEQNUM=", strlen("SEQNUM=")))
                event->seqnum = atoi(s + strlen("SEQNUM="));
            else if (!strncmp(s, "SUBSYSTEM=", strlen("SUBSYSTEM=")))
                event->subsystem = strdup(s + strlen("SUBSYSTEM="));
            else
                event->param[param_idx++] = strdup(s);
        }
        s+= (strlen(s) + 1);
    }

    rc = dispatch_uevent(event);
    
    free_uevent(event);
    return rc;
}

int simulate_uevent(char *subsys, char *path, char *action, char **params)
{
    struct uevent *event;
    char tmp[255];
    int i, rc;

    if (!(event = malloc(sizeof(struct uevent)))) {
        LOGE("Error allocating memory (%s)\n", strerror(errno));
        return -errno;
    }

    memset(event, 0, sizeof(struct uevent));

    event->subsystem = strdup(subsys);

    if (!strcmp(action, "add"))
        event->action = action_add;
    else if (!strcmp(action, "change"))
        event->action = action_change;
    else if (!strcmp(action, "remove"))
        event->action = action_remove;
    else {
        LOGE("Invalid action '%s'\n", action);
        return -1;
    }

    event->path = strdup(path);

    for (i = 0; i < UEVENT_PARAMS_MAX; i++) {
        if (!params[i])
            break;
        event->param[i] = strdup(params[i]);
    }

    rc = dispatch_uevent(event);
    free_uevent(event);
    return rc;
}

static int dispatch_uevent(struct uevent *event)
{
    int i;

    for (i = 0; dispatch_table[i].subsystem != NULL; i++) {
        if (!strcmp(dispatch_table[i].subsystem, event->subsystem))
            return dispatch_table[i].dispatch(event);
    }

#if DEBUG_UEVENT
    LOG_VOL("No uevent handlers registered for '%s' subsystem\n", event->subsystem);
#endif
    return 0;
}

static void dump_uevent(struct uevent *event)
{
    int i;

    LOG_VOL("[UEVENT] Sq: %u S: %s A: %d P: %s\n",
              event->seqnum, event->subsystem, event->action, event->path);
    for (i = 0; i < UEVENT_PARAMS_MAX; i++) {
        if (!event->param[i])
            break;
        LOG_VOL("%s\n", event->param[i]);
    }
}

static void free_uevent(struct uevent *event)
{
    int i;
    free(event->path);
    free(event->subsystem);
    for (i = 0; i < UEVENT_PARAMS_MAX; i++) {
        if (!event->param[i])
            break;
        free(event->param[i]);
    }
    free(event);
}

static char *get_uevent_param(struct uevent *event, char *param_name)
{
    int i;

    for (i = 0; i < UEVENT_PARAMS_MAX; i++) {
        if (!event->param[i])
            break;
        if (!strncmp(event->param[i], param_name, strlen(param_name)))
            return &event->param[i][strlen(param_name) + 1];
    }

    LOGE("get_uevent_param(): No parameter '%s' found\n", param_name);
    return NULL;
}

/*
 * ---------------
 * Uevent Handlers
 * ---------------
 */

static int handle_powersupply_event(struct uevent *event)
{
    dump_uevent(event);
    return 0;
}

static int handle_switch_event(struct uevent *event)
{
    char *name = get_uevent_param(event, "SWITCH_NAME");
    char *state = get_uevent_param(event, "SWITCH_STATE");

    if (!strcmp(name, "usb_mass_storage")) {
        if (!strcmp(state, "online")) {
            ums_hostconnected_set(true);
        } else {
            ums_hostconnected_set(false);
        }
    } else
        LOG_VOL("handle_switch_event(): Ignoring switch '%s'\n", name);

    return 0;
}

static int handle_battery_event(struct uevent *event)
{
    dump_uevent(event);
    return 0;
}

static int handle_block_event(struct uevent *event)
{
    char mediapath[255];
    media_t *media;
    int n;
    int maj, min;
    blkdev_t *blkdev;

    /*
     * Look for backing media for this block device
     */
    if (!strcmp(get_uevent_param(event, "DEVTYPE"), "disk"))
        n = 2;
    else if (!strcmp(get_uevent_param(event, "DEVTYPE"), "partition"))
        n = 3;
    else {
        LOGE("Bad blockdev type '%s'\n", get_uevent_param(event, "DEVTYPE"));
        return -EINVAL;
    }

    truncate_sysfs_path(event->path, n, mediapath);

    if (!(media = media_lookup_by_path(mediapath, false))) {
#if DEBUG_UEVENT
        LOG_VOL("No backend media found @ device path '%s'\n", mediapath);
#endif
        return 0;
    }

    maj = atoi(get_uevent_param(event, "MAJOR"));
    min = atoi(get_uevent_param(event, "MINOR"));

    if (event->action == action_add) {
        blkdev_t *disk;
        boolean pending = false;

        /*
         * If there isn't a disk already its because *we*
         * are the disk
         */
        disk = blkdev_lookup_by_devno(maj, 0);

        /*
         * It is possible that there is already a blkdev
         * for this device (created by blkdev_create_pending_partition())
         */

        if ((blkdev = blkdev_lookup_by_devno(maj, min))) {
            blkdev_devpath_set(blkdev, event->path);
            pending = true;
        } else {
            if (!(blkdev = blkdev_create(disk,
                                         event->path,
                                         maj,
                                         min,
                                         media,
                                         get_uevent_param(event, "DEVTYPE")))) {
                LOGE("Unable to allocate new blkdev (%m)\n");
                return -1;
            }
        }

        /*
         * Add the blkdev to media
         */
        int rc;
        if ((rc = media_add_blkdev(media, blkdev)) < 0) {
            LOGE("Unable to add blkdev to card (%d)\n", rc);
            return rc;
        }

        LOG_VOL("New blkdev %d.%d on media %s, media path %s\n", blkdev->major, blkdev->minor, media->name, mediapath);

        if (pending) {
            /*
             * This blkdev already has its dev_fspath set so 
             * if all partitions are read, pass it off to
             * the volume manager
             */
            LOG_VOL("Pending disk '%d.%d' has %d pending partitions\n",
                    blkdev->disk->major, blkdev->disk->minor, 
                    blkdev_get_num_pending_partitions(blkdev->disk));

            if (blkdev_get_num_pending_partitions(blkdev->disk) == 0) {
                if ((rc = volmgr_consider_disk(blkdev->disk)) < 0) {
                    LOGE("Volmgr failed to handle pending device (%d)\n", rc);
                    return rc;
                }
            }
        }
    } else if (event->action == action_remove) {
        int rc;

        if (!(blkdev = blkdev_lookup_by_devno(maj, min))) {
#if DEBUG_UEVENT
            LOG_VOL("We aren't handling blkdev @ %s\n", event->path);
#endif
            return 0;
        }

        LOG_VOL("Destroying blkdev %d.%d @ %s on media %s\n", blkdev->major, blkdev->minor, blkdev->devpath, media->name);
        if ((rc = volmgr_notify_eject(blkdev, _cb_blkdev_ok_to_destroy)) < 0)
            LOGE("Error notifying volmgr of eject\n");
    } else {
#if DEBUG_UEVENT
        LOG_VOL("No handler implemented for action %d\n", event->action);
#endif
    }
    return 0;
}

static void _cb_blkdev_ok_to_destroy(blkdev_t *dev)
{
    media_t *media = media_lookup_by_dev(dev);
    if (media)
        media_remove_blkdev(media, dev);
    blkdev_destroy(dev);
}

static int handle_bdi_event(struct uevent *event)
{
    return 0;
}

static int handle_mmc_event(struct uevent *event)
{
    if (event->action == action_add) {
        media_t *media;
        char serial[80];
        char *type;

        /*
         * Pull card information from sysfs
         */
        type = get_uevent_param(event, "MMC_TYPE");
        if (strcmp(type, "SD") && strcmp(type, "MMC"))
            return 0;
        
        read_sysfs_var(serial, sizeof(serial), event->path, "serial");
        if (!(media = media_create(event->path,
                                   get_uevent_param(event, "MMC_NAME"),
                                   serial,
                                   media_mmc))) {
            LOGE("Unable to allocate new media (%m)\n");
            return -1;
        }
        LOG_VOL("New MMC card '%s' (serial %u) added @ %s\n", media->name,
                  media->serial, media->devpath);
    } else if (event->action == action_remove) {
        media_t *media;

        if (!(media = media_lookup_by_path(event->path, false))) {
            LOGE("Unable to lookup media '%s'\n", event->path);
            return -1;
        }

        LOG_VOL("MMC card '%s' (serial %u) @ %s removed\n", media->name, 
                  media->serial, media->devpath);
        /*
         * If this media is still mounted, then we have an unsafe removal
         */
        media_destroy(media);
    } else {
#if DEBUG_UEVENT
        LOG_VOL("No handler implemented for action %d\n", event->action);
#endif
    }

    return 0;
}
