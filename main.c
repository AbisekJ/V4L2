#include "camera.h"

int main(int argc, char *argv[])
{
        struct camera_device camera;

        camera.fd = -1;

        if (argc != 2)
        {
                printf("Usage: %s <device_path>\n", argv[0]);
                return EXIT_FAILURE;
        }

        if (argv[1][0] == '\0')
        {
                fprintf(stderr, "%s Device path cannot be empty.\n", ERROR_PREFIX);
                return EXIT_FAILURE;
        }

        printf("========================================\n");
        printf("       %s\n", APP_NAME);
        printf("========================================\n");

        if (camera_open(&camera, argv[1]) == -1)
        {
                return EXIT_FAILURE;
        }

        if (camera_query_capabilities(&camera) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        if (camera_enumerate_formats(&camera) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        camera_close(&camera);

        return EXIT_SUCCESS;
}
