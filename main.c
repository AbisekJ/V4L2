#include "camera.h"

int main(int argc, char *argv[])
{
        struct camera_device camera;
        char device_path[CAMERA_DEVICE_PATH_SIZE];

        camera.fd = -1;
        camera.capabilities = 0;

        if (argc > 2)
        {
                fprintf(stderr, "Usage: %s [device_path]\n", argv[0]);
                return EXIT_FAILURE;
        }

        printf("========================================\n");
        printf("       %s\n", APP_NAME);
        printf("========================================\n");

        if (argc == 2)
        {
                if (argv[1][0] == '\0')
                {
                        fprintf(stderr, "%s Device path cannot be empty.\n", ERROR_PREFIX);
                        return EXIT_FAILURE;
                }

                if (strlen(argv[1]) >= sizeof(device_path))
                {
                        fprintf(stderr, "%s Device path is too long.\n", ERROR_PREFIX);
                        return EXIT_FAILURE;
                }

                strcpy(device_path, argv[1]);
        }
        else
        {
                if (camera_select_device(device_path, sizeof(device_path)) == -1)
                {
                        return EXIT_FAILURE;
                }
        }

        if (camera_open(&camera, device_path) == -1)
        {
                return EXIT_FAILURE;
        }

        if (camera_query_capabilities(&camera) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        printf("\n========== CAMERA SUPPORTED CONFIGURATIONS ==========\n");

        if (camera_enumerate_formats(&camera) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        if (camera_configure_format(&camera) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        camera_close(&camera);

        return EXIT_SUCCESS;
}
