#include "camera.h"

int main(int argc, char *argv[])
{
        struct camera_device camera;
        struct v4l2_pix_format applied_format;
        struct camera_capture_config capture_config;
        struct camera_buffer_pool buffer_pool;
        char device_path[CAMERA_DEVICE_PATH_SIZE];

        camera.fd = -1;
        camera.capabilities = 0;
        memset(&applied_format, 0, sizeof(applied_format));
        capture_config.io_method = CAMERA_IO_NONE;
        capture_config.frame_count = 0;
        buffer_pool.buffers = NULL;
        buffer_pool.count = 0;
        buffer_pool.io_method = CAMERA_IO_NONE;

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

        if (camera_configure_format(&camera, &applied_format) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        if (camera_configure_frame_interval(&camera, applied_format.pixelformat, applied_format.width, applied_format.height) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        if (camera_configure_capture(&camera, &capture_config) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        if (camera_setup_buffers(&camera, &capture_config, applied_format.sizeimage, &buffer_pool) == -1)
        {
                camera_close(&camera);
                return EXIT_FAILURE;
        }

        camera_release_buffers(&buffer_pool);
        camera_close(&camera);
        return EXIT_SUCCESS;
}
