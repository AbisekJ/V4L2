#ifndef CAMERA_H
#define CAMERA_H

#include "common.h"
#include <time.h>
#include <linux/videodev2.h>

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
int camera_configure_format(struct camera_device *camera, struct v4l2_pix_format *applied_format);
int camera_configure_frame_interval(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height);
void camera_close(struct camera_device *camera);

#endif
