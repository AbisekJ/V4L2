#include "camera.h"

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static int camera_ioctl(int fd, unsigned long request, void *argument);

static int camera_validate_device(const char *device_path, struct v4l2_capability *cap, uint32_t *effective_caps);

static int camera_enumerate_frame_sizes(struct camera_device *camera, uint32_t pixel_format);

static int camera_enumerate_frame_intervals(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height);


static int camera_ioctl(int fd, unsigned long request, void *argument)
{
        int result;

        do
        {
                result = ioctl(fd, request, argument);
        }
        while (result == -1 && errno == EINTR);

        return result;
}


static int camera_validate_device(const char *device_path, struct v4l2_capability *cap, uint32_t *effective_caps)
{
        struct stat device_stat;
        int fd;
        int saved_errno;

        if (device_path == NULL || cap == NULL || effective_caps == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        fd = open(device_path, O_RDWR);

        if (fd == -1)
        {
                return -1;
        }

        if (fstat(fd, &device_stat) == -1)
        {
                saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return -1;
        }

        if (!S_ISCHR(device_stat.st_mode))
        {
                close(fd);
                errno = ENODEV;
                return -1;
        }

        memset(cap, 0, sizeof(*cap));

        if (camera_ioctl(fd, VIDIOC_QUERYCAP, cap) == -1)
        {
                saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return -1;
        }

        if (cap->capabilities & V4L2_CAP_DEVICE_CAPS)
        {
                *effective_caps = cap->device_caps;
        }
        else
        {
                *effective_caps = cap->capabilities;
        }

        if (!(*effective_caps & V4L2_CAP_VIDEO_CAPTURE))
        {
                close(fd);
                errno = ENODEV;
                return -1;
        }

        return fd;
}


int camera_select_device(char *device_path, size_t path_size)
{
        glob_t matches;
        struct v4l2_capability cap;
        uint32_t effective_caps;
        size_t *valid_indices;
        size_t valid_count = 0;
        size_t index;
        size_t selection;
        char input[64];
        char *endptr;
        long choice;
        int fd;
        int result;

        if (device_path == NULL || path_size == 0)
        {
                fprintf(stderr, "%s Invalid device selection parameters.\n", ERROR_PREFIX);
                return -1;
        }

        printf("%s Searching for available camera devices...\n", STATUS_PREFIX);

        memset(&matches, 0, sizeof(matches));

        result = glob("/dev/video*", 0, NULL, &matches);

        if (result == GLOB_NOMATCH)
        {
                fprintf(stderr, "%s No video device nodes found.\n", ERROR_PREFIX);
                globfree(&matches);
                return -1;
        }

        if (result != 0)
        {
                fprintf(stderr, "%s Failed to discover video devices.\n", ERROR_PREFIX);
                globfree(&matches);
                return -1;
        }

        valid_indices = calloc(matches.gl_pathc, sizeof(*valid_indices));

        if (valid_indices == NULL)
        {
                perror("ERROR: Failed to allocate device list");
                globfree(&matches);
                return -1;
        }

        for (index = 0; index < matches.gl_pathc; index++)
        {
                fd = camera_validate_device(matches.gl_pathv[index], &cap, &effective_caps);

                if (fd == -1)
                {
                        continue;
                }

                if (close(fd) == -1)
                {
                        perror("ERROR: Failed to close temporary device");
                        free(valid_indices);
                        globfree(&matches);
                        return -1;
                }

                valid_indices[valid_count] = index;
                valid_count++;
        }

        if (valid_count == 0)
        {
                fprintf(stderr, "%s No valid V4L2 capture devices found.\n", ERROR_PREFIX);
                free(valid_indices);
                globfree(&matches);
                return -1;
        }

        printf("\n========== AVAILABLE CAMERA DEVICES ==========\n");

        for (index = 0; index < valid_count; index++)
        {
                fd = camera_validate_device(matches.gl_pathv[valid_indices[index]], &cap, &effective_caps);

                if (fd == -1)
                {
                        fprintf(stderr, "%s Device became unavailable during discovery.\n", ERROR_PREFIX);
                        free(valid_indices);
                        globfree(&matches);
                        return -1;
                }

                printf("\n[%zu] %s\n", index + 1, cap.card);
                printf("    Device : %s\n", matches.gl_pathv[valid_indices[index]]);
                printf("    Driver : %s\n", cap.driver);
                printf("    Bus    : %s\n", cap.bus_info);

                if (close(fd) == -1)
                {
                        perror("ERROR: Failed to close temporary device");
                        free(valid_indices);
                        globfree(&matches);
                        return -1;
                }
        }

        while (1)
        {
                printf("\nSelect a camera device (1-%zu): ", valid_count);
                fflush(stdout);

                if (fgets(input, sizeof(input), stdin) == NULL)
                {
                        fprintf(stderr, "\n%s Failed to read selection.\n", ERROR_PREFIX);
                        free(valid_indices);
                        globfree(&matches);
                        return -1;
                }

                if (strchr(input, '\n') == NULL && !feof(stdin))
                {
                        int character;

                        while ((character = getchar()) != '\n' && character != EOF)
                        {
                        }

                        fprintf(stderr, "%s Input is too long. Try again.\n", ERROR_PREFIX);
                        continue;
                }

                errno = 0;
                choice = strtol(input, &endptr, 10);

                while (isspace((unsigned char)*endptr))
                {
                        endptr++;
                }

                if (errno != 0 || endptr == input || *endptr != '\0' || choice < 1 || (unsigned long)choice > valid_count)
                {
                        fprintf(stderr, "%s Invalid selection. Try again.\n", ERROR_PREFIX);
                        continue;
                }

                selection = (size_t)choice - 1;
                break;
        }

        if (strlen(matches.gl_pathv[valid_indices[selection]]) >= path_size)
        {
                fprintf(stderr, "%s Device path is too long.\n", ERROR_PREFIX);
                free(valid_indices);
                globfree(&matches);
                return -1;
        }

        strcpy(device_path, matches.gl_pathv[valid_indices[selection]]);

        printf("\n%s Selected device: %s\n", STATUS_PREFIX, device_path);

        free(valid_indices);
        globfree(&matches);

        return 0;
}


int camera_open(struct camera_device *camera, const char *device_path)
{
        struct v4l2_capability cap;
        uint32_t effective_caps;
        int fd;

        if (camera == NULL || device_path == NULL || device_path[0] == '\0')
        {
                fprintf(stderr, "%s Invalid camera parameters.\n", ERROR_PREFIX);
                return -1;
        }

        if (camera->fd >= 0)
        {
                fprintf(stderr, "%s Camera is already open.\n", ERROR_PREFIX);
                return -1;
        }

        fd = camera_validate_device(device_path, &cap, &effective_caps);

        if (fd == -1)
        {
                perror("ERROR: Failed to validate camera device");
                return -1;
        }

        camera->fd = fd;
        camera->capabilities = effective_caps;

        printf("%s Valid V4L2 capture device opened: %s\n", STATUS_PREFIX, device_path);
        printf("%s File descriptor: %d\n", STATUS_PREFIX, camera->fd);

        return 0;
}


int camera_query_capabilities(struct camera_device *camera)
{
        struct v4l2_capability cap;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        memset(&cap, 0, sizeof(cap));

        if (camera_ioctl(camera->fd, VIDIOC_QUERYCAP, &cap) == -1)
        {
                perror("ERROR: VIDIOC_QUERYCAP failed");
                return -1;
        }

        printf("\n========== CAMERA INFORMATION ==========\n");

        printf("Driver        : %s\n", cap.driver);
        printf("Card          : %s\n", cap.card);
        printf("Bus Info      : %s\n", cap.bus_info);

        printf("Driver Version: %u.%u.%u\n",
               (cap.version >> 16) & 0xFF,
               (cap.version >> 8) & 0xFF,
               cap.version & 0xFF);

        printf("\n========== DEVICE CAPABILITIES ==========\n");

        if (camera->capabilities & V4L2_CAP_VIDEO_CAPTURE)
        {
                printf("Video Capture: Supported\n");
        }

        if (camera->capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        {
                printf("Video Capture Multiplanar: Supported (not implemented)\n");
        }

        if (camera->capabilities & V4L2_CAP_STREAMING)
        {
                printf("Streaming: Supported\n");
        }

        if (camera->capabilities & V4L2_CAP_READWRITE)
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

                if (camera_ioctl(camera->fd, VIDIOC_ENUM_FMT, &fmt) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FMT failed");
                        return -1;
                }

                printf("\n----------------------------------------------\n");
                printf("Index            : %u\n", fmt.index);
                printf("Description      : %s\n", fmt.description);

                printf("Pixel Format     : %c%c%c%c\n",
                       (unsigned char)(fmt.pixelformat & 0xFF),
                       (unsigned char)((fmt.pixelformat >> 8) & 0xFF),
                       (unsigned char)((fmt.pixelformat >> 16) & 0xFF),
                       (unsigned char)((fmt.pixelformat >> 24) & 0xFF));

                if (camera_enumerate_frame_sizes(camera, fmt.pixelformat) == -1)
                {
                        return -1;
                }

                index++;
        }

        if (index == 0)
        {
                fprintf(stderr, "%s No capture formats found.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n----------------------------------------------\n");
        printf("%s Found %u pixel formats.\n", STATUS_PREFIX, index);

        return 0;
}


static int camera_enumerate_frame_sizes(struct camera_device *camera, uint32_t pixel_format)
{
        struct v4l2_frmsizeenum size;
        unsigned int index = 0;
        bool found = false;

        printf("\n========== SUPPORTED FRAME SIZES ==========\n");

        while (1)
        {
                memset(&size, 0, sizeof(size));

                size.index = index;
                size.pixel_format = pixel_format;

                if (camera_ioctl(camera->fd, VIDIOC_ENUM_FRAMESIZES, &size) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FRAMESIZES failed");
                        return -1;
                }

                found = true;

                if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                {
                        printf("\nResolution: %u x %u\n", size.discrete.width, size.discrete.height);

                        if (camera_enumerate_frame_intervals(camera, pixel_format, size.discrete.width, size.discrete.height) == -1)
                        {
                                return -1;
                        }
                }
                else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE)
                {
                        printf("\nStepwise resolution range:\n");
                        printf("Minimum: %u x %u\n", size.stepwise.min_width, size.stepwise.min_height);
                        printf("Maximum: %u x %u\n", size.stepwise.max_width, size.stepwise.max_height);
                        printf("Step: %u x %u\n", size.stepwise.step_width, size.stepwise.step_height);

                        break;
                }
                else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
                {
                        printf("\nContinuous resolution range:\n");
                        printf("Minimum: %u x %u\n", size.stepwise.min_width, size.stepwise.min_height);
                        printf("Maximum: %u x %u\n", size.stepwise.max_width, size.stepwise.max_height);

                        break;
                }
                else
                {
                        printf("%s Unknown frame-size type.\n", STATUS_PREFIX);
                        break;
                }

                index++;
        }

        if (!found)
        {
                printf("%s No frame sizes reported for this format.\n", STATUS_PREFIX);
        }

        return 0;
}


static int camera_enumerate_frame_intervals(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height)
{
        struct v4l2_frmivalenum interval;
        unsigned int index = 0;
        bool found = false;
        double fps;

        printf("  Supported frame rates:\n");

        while (1)
        {
                memset(&interval, 0, sizeof(interval));

                interval.index = index;
                interval.pixel_format = pixel_format;
                interval.width = width;
                interval.height = height;

                if (camera_ioctl(camera->fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FRAMEINTERVALS failed");
                        return -1;
                }

                found = true;

                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
                {
                        if (interval.discrete.numerator == 0 || interval.discrete.denominator == 0)
                        {
                                fprintf(stderr, "%s Invalid frame interval.\n", ERROR_PREFIX);
                                return -1;
                        }

                        fps = (double)interval.discrete.denominator / interval.discrete.numerator;

                        printf("    Index %u: %.2f FPS\n", index, fps);
                }
                else if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE || interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS)
                {
                        if (interval.stepwise.min.numerator == 0 ||
                            interval.stepwise.min.denominator == 0 ||
                            interval.stepwise.max.numerator == 0 ||
                            interval.stepwise.max.denominator == 0)
                        {
                                fprintf(stderr, "%s Invalid frame interval range.\n", ERROR_PREFIX);
                                return -1;
                        }

                        double min_fps = (double)interval.stepwise.max.denominator / interval.stepwise.max.numerator;
                        double max_fps = (double)interval.stepwise.min.denominator / interval.stepwise.min.numerator;

                        printf("    Minimum FPS: %.2f\n", min_fps);
                        printf("    Maximum FPS: %.2f\n", max_fps);

                        if (interval.type == V4L2_FRMIVAL_TYPE_STEPWISE)
                        {
                                printf("    Frame interval step: %u/%u seconds\n",
                                       interval.stepwise.step.numerator,
                                       interval.stepwise.step.denominator);
                        }

                        break;
                }
                else
                {
                        printf("    %s Unknown frame-interval type.\n", STATUS_PREFIX);
                        break;
                }

                index++;
        }

        if (!found)
        {
                printf("    %s No frame intervals reported.\n", STATUS_PREFIX);
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
        camera->capabilities = 0;
}
