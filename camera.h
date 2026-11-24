#ifndef CAMERA_H
#define CAMERA_H

#include "common.h"

#define CAMERA_DEVICE_PATH_SIZE 512
#define CAMERA_DEVICE_NAME_SIZE 256

struct camera_device
{
        int fd;
        uint32_t capabilities;
};

int camera_select_device(char *device_path, size_t path_size);
int camera_open(struct camera_device *camera, const char *device_path);
int camera_query_capabilities(struct camera_device *camera);
int camera_enumerate_formats(struct camera_device *camera);
void camera_close(struct camera_device *camera);

#endif
