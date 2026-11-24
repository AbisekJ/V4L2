#ifndef CAMERA_H
#define CAMERA_H

#include "common.h"
#include <time.h>
#include <stddef.h>
#include <linux/videodev2.h>

#define CAMERA_DEVICE_PATH_SIZE 512
#define CAMERA_DEVICE_NAME_SIZE 256

#define CAMERA_MIN_FRAME_COUNT 1U
#define CAMERA_MAX_FRAME_COUNT 10000U
#define CAMERA_IO_METHOD_COUNT 3U

enum camera_io_method
{
        CAMERA_IO_NONE = 0,
        CAMERA_IO_MMAP,
        CAMERA_IO_USERPTR,
        CAMERA_IO_DMABUF
};

struct camera_device
{
        int fd;
        uint32_t capabilities;
};

struct camera_capture_config
{
        enum camera_io_method io_method;
        uint32_t frame_count;
};

struct camera_buffer
{
        void *start;
        size_t length;
        int dmabuf_fd;
        uint32_t index;
        enum v4l2_memory memory;
};

struct camera_buffer_pool
{
        struct camera_buffer *buffers;
        uint32_t count;
        enum camera_io_method io_method;
};

int camera_select_device(char *device_path, size_t path_size);
int camera_open(struct camera_device *camera, const char *device_path);
int camera_query_capabilities(struct camera_device *camera);
int camera_enumerate_formats(struct camera_device *camera);
int camera_configure_format(struct camera_device *camera, struct v4l2_pix_format *applied_format);
int camera_configure_frame_interval(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height);
int camera_configure_capture(struct camera_device *camera, struct camera_capture_config *config);
int camera_setup_buffers(struct camera_device *camera, const struct camera_capture_config *config, uint32_t frame_size, struct camera_buffer_pool *pool);
void camera_release_buffers(struct camera_buffer_pool *pool);
int camera_queue_all_buffers(struct camera_device *camera, const struct camera_buffer_pool *pool);
int camera_stream_on(struct camera_device *camera);
int camera_stream_off(struct camera_device *camera);
void camera_close(struct camera_device *camera);

#endif
