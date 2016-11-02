/*
 * This file is part of MultiROM.
 *
 * MultiROM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiROM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiROM.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "inject.h"
#include "mrom_data.h"
#include "log.h"
#include "util.h"
#include "../version.h"

#include "bump.h"

int bump_bootimg(const char * sourceimg)
{
    char bump_dir[256];
    char path[256];
    int ret = 0;

    snprintf(bump_dir, sizeof(bump_dir), "%s/bump_dir", mrom_dir());

    INFO("Bumping boot.img...\n");

    // remove temp bump dir
    remove_dir(bump_dir);

    // mkdir temp bump dir
    mkdir(bump_dir, 0755);

    // copy img to temporary bump dir as "original-boot.img"
    snprintf(path, sizeof(path), "%s/original-boot.img", bump_dir);
    if (copy_file(sourceimg, path) < 0)
    {
        ERROR("Failed to copy %s to %s!\n", sourceimg, path);
        remove_dir(bump_dir);
        return -1;
    }

    // execute livebump script
    ret = mr_system("B=%s/busybox; \"$B\" sh %s/livebump.sh %s %s", mrom_dir(), mrom_dir(), mrom_dir(), bump_dir);
    if(ret != 0)
    {
        ERROR("Failed to bump boot.img!, ret=%d\n", ret);
        remove_dir(bump_dir);
        return -1;
    }
    // copy bumped image back to sourceimg
    snprintf(path, sizeof(path), "%s/image_bumped.img", bump_dir);
    if (copy_file(path, sourceimg) < 0)
    {
        ERROR("Failed to copy %s to %s!\n", path, sourceimg);
        remove_dir(bump_dir);
        return -1;
    }

    // remove temp bump dir
    remove_dir(bump_dir);

    return 0;
}

int wipe_boot(const char * img_path)
{
    int ret = 0;

    INFO("Cleaning boot image at %s...\n", img_path);

    // Clean boot path
    ret = mr_system("B=%s/busybox; \"$B\" dd if=\"/dev/zero\" of=\"%s\" bs=4096 count=4096; \"$B\" strings %s | \"$B\" head", mrom_dir(), img_path, img_path);
    if(ret != 0)
    {
        ERROR("Failed to clean boot at %s!, ret=%d\n", img_path, ret);
        return -1;
    }

    return 0;
}
