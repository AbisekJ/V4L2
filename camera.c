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
