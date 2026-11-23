#include "camera.h"

#include <fcntl.h>
#include <unistd.h>

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
