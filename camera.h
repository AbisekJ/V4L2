#ifndef CAMERA_H
#define CAMERA_H

#include "common.h"

struct camera_device
{
        int fd;
};

int camera_open(struct camera_device *camera, const char *device_path);
void camera_close(struct camera_device *camera);

#endif
