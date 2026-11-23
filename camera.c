#include "camera.h"

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

int camera_open(struct camera_device *camera, const char *device_path)
{
        if (camera == NULL || device_path == NULL || device_path[0] == '\0')
        {
                fprintf(stderr, "%s Invalid camera parameters.\n", ERROR_PREFIX);
                return -1;
        }

        camera->fd = -1;

        camera->fd = open(device_path, O_RDWR);

        if (camera->fd == -1)
        {
                perror("ERROR: Failed to open camera device");
                return -1;
        }

        printf("%s Camera device opened: %s\n", STATUS_PREFIX, device_path);
        printf("%s File descriptor: %d\n", STATUS_PREFIX, camera->fd);

        return 0;
}

int camera_query_capabilities(struct camera_device *camera)
{
        struct v4l2_capability cap;
        uint32_t effective_caps;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        memset(&cap, 0, sizeof(cap));

        if (ioctl(camera->fd, VIDIOC_QUERYCAP, &cap) == -1)
        {
                perror("ERROR: VIDIOC_QUERYCAP failed");
                return -1;
        }

        if (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
        {
                effective_caps = cap.device_caps;
        }
        else
        {
                effective_caps = cap.capabilities;
        }

        printf("\n========== CAMERA INFORMATION ==========\n");

        printf("Driver       : %s\n", cap.driver);
        printf("Card         : %s\n", cap.card);
        printf("Bus Info     : %s\n", cap.bus_info);

        printf("Driver Version: %u.%u.%u\n", (cap.version >> 16) & 0xFF, (cap.version >> 8) & 0xFF, cap.version & 0xFF);

        printf("\n========== DEVICE CAPABILITIES ==========\n");

        if (effective_caps & V4L2_CAP_VIDEO_CAPTURE)
        {
                printf("Video Capture: Supported\n");
        }

        if (effective_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        {
                printf("Video Capture Multiplanar: Supported\n");
        }

        if (effective_caps & V4L2_CAP_STREAMING)
        {
                printf("Streaming: Supported\n");
        }

        if (effective_caps & V4L2_CAP_READWRITE)
        {
                printf("Read/Write I/O: Supported\n");
        }

        printf("%s Capability discovery completed.\n", STATUS_PREFIX);

        return 0;
}

int camera_enumerate_formats(struct camera_device *camera)
{
        struct v4l2_fmtdesc fmt;
        unsigned int index = 0;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== AVAILABLE PIXEL FORMATS ==========\n");

        while (1)
        {
                memset(&fmt, 0, sizeof(fmt));

                fmt.index = index;
                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                if (ioctl(camera->fd, VIDIOC_ENUM_FMT, &fmt) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FMT failed");
                        return -1;
                }
		printf("----------------------------------------------\n");
                printf("Index            : %u\n", fmt.index);
                printf("Description      : %s\n", fmt.description);

                printf("Pixel Format     : %c%c%c%c\n",
                       fmt.pixelformat & 0xFF,
                       (fmt.pixelformat >> 8) & 0xFF,
                       (fmt.pixelformat >> 16) & 0xFF,
                       (fmt.pixelformat >> 24) & 0xFF);

                index++;
        }

        if (index == 0)
        {
                fprintf(stderr, "%s No capture formats found.\n", ERROR_PREFIX);
                return -1;
        }
	printf("----------------------------------------------\n");
        printf("%s Found %u pixel formats.\n", STATUS_PREFIX, index);

        return 0;
}

int camera_enumerate_frame_sizes(struct camera_device *camera, uint32_t pixel_format)
{
        struct v4l2_frmsizeenum size;
        unsigned int index = 0;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== SUPPORTED FRAME SIZES ==========\n");

        while (1)
        {
                memset(&size, 0, sizeof(size));

                size.index = index;
                size.pixel_format = pixel_format;

                if (ioctl(camera->fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FRAMESIZES failed");
                        return -1;
                }

                if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                {
                        printf("Index %u: %u x %u\n", index, size.discrete.width, size.discrete.height);
                }
                else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE)
                {
                        printf("Stepwise resolution range:\n");
                        printf("Minimum: %u x %u\n", size.stepwise.min_width, size.stepwise.min_height);

                        printf("Maximum: %u x %u\n", size.stepwise.max_width, size.stepwise.max_height);

                        printf("Step: %u x %u\n", size.stepwise.step_width, size.stepwise.step_height);

                        break;
                }
                else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
                {
                        printf("Continuous resolution range:\n");
                        printf("Minimum: %u x %u\n", size.stepwise.min_width, size.stepwise.min_height);

                        printf("Maximum: %u x %u\n", size.stepwise.max_width, size.stepwise.max_height);

                        break;
                }

                index++;
        }

        if (index == 0 && size.type != V4L2_FRMSIZE_TYPE_STEPWISE && size.type != V4L2_FRMSIZE_TYPE_CONTINUOUS)
        {
                printf("%s No frame sizes reported for this format.\n", STATUS_PREFIX);
        }

        return 0;
}

int camera_enumerate_frame_intervals(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height)
{
        struct v4l2_frmivalenum interval;
        unsigned int index = 0;
        double fps;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== SUPPORTED FRAME RATES ==========\n");
        printf("Resolution: %u x %u\n", width, height);

        while (1)
        {
                memset(&interval, 0, sizeof(interval));

                interval.index = index;
                interval.pixel_format = pixel_format;
                interval.width = width;
                interval.height = height;

                if (ioctl(camera->fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FRAMEINTERVALS failed");
                        return -1;
                }

                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
                {
                        if (interval.discrete.numerator == 0)
                        {
                                fprintf(stderr, "%s Invalid frame interval.\n", ERROR_PREFIX);
                                return -1;
                        }

                        fps = (double)interval.discrete.denominator / interval.discrete.numerator;

                        printf("Index %u: %.2f FPS\n", index, fps);
                }
                else if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE || interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS)
                {
                        if (interval.stepwise.min.numerator == 0 || interval.stepwise.max.numerator == 0)
                        {
                                fprintf(stderr, "%s Invalid frame interval range.\n", ERROR_PREFIX);
                                return -1;
                        }

                        double min_fps = (double)interval.stepwise.max.denominator / interval.stepwise.max.numerator;

                        double max_fps = (double)interval.stepwise.min.denominator / interval.stepwise.min.numerator;

                        printf("Minimum FPS: %.2f\n", min_fps);
                        printf("Maximum FPS: %.2f\n", max_fps);

                        if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE)
                        {
                                printf("Frame interval step: %u/%u seconds\n", interval.stepwise.step.numerator, interval.stepwise.step.denominator);
                        }

                        break;
                }

                index++;
        }

        if (index == 0)
        {
                printf("%s No frame intervals reported for this combination.\n", STATUS_PREFIX);
        }
        else
        {
                printf("%s Found %u discrete frame intervals.\n", STATUS_PREFIX, index);
        }

        return 0;
}

void camera_close(struct camera_device *camera)
{
        if (camera == NULL || camera->fd < 0)
        {
                return;
        }

        if (close(camera->fd) == -1)
        {
                perror("ERROR: Failed to close camera device");
        }
        else
        {
                printf("%s Camera device closed successfully.\n", STATUS_PREFIX);
        }

        camera->fd = -1;
}
