#ifndef CAMERA_H
#define CAMERA_H

#include "common.h"

struct camera_device
{
        int fd;
};

int camera_open(struct camera_device *camera, const char *device_path);
int camera_query_capabilities(struct camera_device *camera);
int camera_enumerate_formats(struct camera_device *camera);
void camera_close(struct camera_device *camera);

#endif
