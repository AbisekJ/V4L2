#include "camera.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <libudev.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <poll.h>

struct camera_candidate
{
        char path[CAMERA_DEVICE_PATH_SIZE];
        char name[CAMERA_DEVICE_NAME_SIZE];
};

static int camera_ioctl(int fd, unsigned long request, void *argument);
static void camera_print_fourcc(uint32_t fourcc);
static int camera_validate_device(const char *device_path, uint32_t *capabilities);
static int camera_enumerate_frame_sizes(struct camera_device *camera, uint32_t pixel_format);
static int camera_enumerate_frame_intervals(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height);
static int camera_read_number(const char *prompt, uint32_t *value);
static int camera_validate_resolution(uint32_t width, uint32_t height, const struct v4l2_frmsizeenum *range);
static int camera_prompt_custom_resolution(const struct v4l2_frmsizeenum *range, uint32_t *width, uint32_t *height);
static double camera_fract_to_fps(struct v4l2_fract interval);
static int camera_get_current_interval(struct camera_device *camera, struct v4l2_fract *current_interval, bool *supports_timeperframe);
static int camera_set_interval(struct camera_device *camera, struct v4l2_fract target_interval);
static int camera_select_target_fps_from_range(const struct v4l2_frmivalenum *range, struct v4l2_fract *chosen_interval);
static const char *camera_io_method_name(enum camera_io_method method);
static int camera_select_io_method(struct camera_device *camera, enum camera_io_method *selected_method);
static int camera_select_frame_count(uint32_t *frame_count);
static enum v4l2_memory camera_io_to_v4l2_memory(enum camera_io_method method);
static size_t camera_align_up_size(size_t size, size_t alignment);
static int camera_request_buffers(struct camera_device *camera, enum v4l2_memory memory, uint32_t *allocated_count);
static int camera_init_mmap_buffers(struct camera_device *camera, struct camera_buffer_pool *pool);
static int camera_init_userptr_buffers(struct camera_buffer_pool *pool, size_t frame_size);
static int camera_init_dmabuf_buffers(struct camera_buffer_pool *pool, size_t frame_size);
static int camera_set_stream_state(struct camera_device *camera, unsigned long request, const char *action_name);
static int camera_wait_for_frame(struct camera_device *camera);
static int camera_requeue_single_buffer(struct camera_device *camera, const struct camera_buffer *buffer, const struct v4l2_buffer *dequeued_buf);
static int camera_write_frame_file(uint32_t frame_index, const void *data, size_t byte_count);
static int camera_save_captured_frame(const struct camera_buffer *buffer, const struct v4l2_buffer *dequeued_buf, uint32_t frame_index);

// Retry ioctl when interrupted by a signal.
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

// Print a V4L2 FourCC code as four ASCII characters.
static void camera_print_fourcc(uint32_t fourcc)
{
        printf("%c%c%c%c",
               (unsigned char)(fourcc & BYTE_MASK),
               (unsigned char)((fourcc >> SHIFT_8_BITS) & BYTE_MASK),
               (unsigned char)((fourcc >> SHIFT_16_BITS) & BYTE_MASK),
               (unsigned char)((fourcc >> SHIFT_24_BITS) & BYTE_MASK));
}

// Open device node, verify it is a char capture device, and return fd.
static int camera_validate_device(const char *device_path, uint32_t *capabilities)
{
        struct stat device_info;
        struct v4l2_capability capability;
        uint32_t effective_caps;
        int fd;
        int saved_errno;

        if (device_path == NULL || device_path[0] == '\0' || capabilities == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        fd = open(device_path, O_RDWR);
        if (fd == -1)
        {
                return -1;
        }

        if (fstat(fd, &device_info) == -1)
        {
                saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return -1;
        }

        if (!S_ISCHR(device_info.st_mode))
        {
                close(fd);
                errno = ENODEV;
                return -1;
        }

        memset(&capability, 0, sizeof(capability));
        if (camera_ioctl(fd, VIDIOC_QUERYCAP, &capability) == -1)
        {
                saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return -1;
        }

        if (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
        {
                effective_caps = capability.device_caps;
        }
        else
        {
                effective_caps = capability.capabilities;
        }

        if (!(effective_caps & V4L2_CAP_VIDEO_CAPTURE))
        {
                close(fd);
                errno = ENOTSUP;
                return -1;
        }

        *capabilities = effective_caps;
        return fd;
}

// Discover V4L2 capture devices via libudev and let the user pick one.
int camera_select_device(char *device_path, size_t path_size)
{
        struct udev *udev = NULL;
        struct udev_enumerate *enumerate = NULL;
        struct udev_list_entry *devices;
        struct udev_list_entry *entry;
        struct udev_device *device = NULL;
        struct camera_candidate *candidates = NULL;
        const char *syspath;
        const char *devnode;
        const char *device_name;
        size_t candidate_count = 0;
        size_t path_length;
        size_t name_length;
        size_t index;
        char input[INPUT_BUFFER_SIZE];
        char *end_pointer;
        long selection;
        uint32_t capabilities;
        int fd;
        int character;
        int result = -1;

        if (device_path == NULL || path_size == 0)
        {
                fprintf(stderr, "%s Invalid device path buffer.\n", ERROR_PREFIX);
                return -1;
        }

        udev = udev_new();
        if (udev == NULL)
        {
                fprintf(stderr, "%s Failed to initialize libudev.\n", ERROR_PREFIX);
                return -1;
        }

        enumerate = udev_enumerate_new(udev);
        if (enumerate == NULL)
        {
                fprintf(stderr, "%s Failed to create udev enumerator.\n", ERROR_PREFIX);
                goto cleanup;
        }

        if (udev_enumerate_add_match_subsystem(enumerate, "video4linux") < 0)
        {
                fprintf(stderr, "%s Failed to set video4linux subsystem filter.\n", ERROR_PREFIX);
                goto cleanup;
        }

        if (udev_enumerate_scan_devices(enumerate) < 0)
        {
                fprintf(stderr, "%s Failed to enumerate video4linux devices.\n", ERROR_PREFIX);
                goto cleanup;
        }

        devices = udev_enumerate_get_list_entry(enumerate);
        udev_list_entry_foreach(entry, devices)
        {
                syspath = udev_list_entry_get_name(entry);
                if (syspath == NULL)
                {
                        continue;
                }

                device = udev_device_new_from_syspath(udev, syspath);
                if (device == NULL)
                {
                        continue;
                }

                devnode = udev_device_get_devnode(device);
                if (devnode == NULL)
                {
                        udev_device_unref(device);
                        device = NULL;
                        continue;
                }

                path_length = strlen(devnode);
                if (path_length >= CAMERA_DEVICE_PATH_SIZE)
                {
                        udev_device_unref(device);
                        device = NULL;
                        continue;
                }

                fd = camera_validate_device(devnode, &capabilities);
                if (fd == -1)
                {
                        udev_device_unref(device);
                        device = NULL;
                        continue;
                }

                if (close(fd) == -1)
                {
                        perror("ERROR: Failed to close temporary device descriptor");
                }

                struct camera_candidate *new_candidates = realloc(candidates, (candidate_count + 1) * sizeof(*candidates));
                if (new_candidates == NULL)
                {
                        perror("ERROR: Failed to allocate candidate list");
                        udev_device_unref(device);
                        device = NULL;
                        goto cleanup;
                }

                candidates = new_candidates;
                strcpy(candidates[candidate_count].path, devnode);

                device_name = udev_device_get_sysattr_value(device, "name");
                if (device_name == NULL || device_name[0] == '\0')
                {
                        device_name = "Unknown V4L2 device";
                }

                name_length = strlen(device_name);
                if (name_length >= CAMERA_DEVICE_NAME_SIZE)
                {
                        name_length = CAMERA_DEVICE_NAME_SIZE - 1;
                }

                memcpy(candidates[candidate_count].name, device_name, name_length);
                candidates[candidate_count].name[name_length] = '\0';

                candidate_count++;
                udev_device_unref(device);
                device = NULL;
        }

        if (candidate_count == 0)
        {
                fprintf(stderr, "%s No usable V4L2 capture devices found.\n", ERROR_PREFIX);
                goto cleanup;
        }

        printf("\n========== AVAILABLE CAPTURE DEVICES ==========\n");
        for (index = 0; index < candidate_count; index++)
        {
                printf("\n[%zu] %s\n", index + 1, candidates[index].name);
                printf("    Device node: %s\n", candidates[index].path);
        }

        printf("\nSelect a camera [1-%zu]: ", candidate_count);
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
        {
                perror("ERROR: Failed to read device selection");
                goto cleanup;
        }

        if (strchr(input, '\n') == NULL && !feof(stdin))
        {
                while ((character = getchar()) != '\n' && character != EOF)
                {
                }

                fprintf(stderr, "%s Selection input is too long.\n", ERROR_PREFIX);
                goto cleanup;
        }

        errno = 0;
        selection = strtol(input, &end_pointer, DECIMAL_BASE);

        while (isspace((unsigned char)*end_pointer))
        {
                end_pointer++;
        }

        if (errno == ERANGE || end_pointer == input || *end_pointer != '\0' || selection < 1 || (size_t)selection > candidate_count)
        {
                fprintf(stderr, "%s Invalid device selection.\n", ERROR_PREFIX);
                goto cleanup;
        }

        path_length = strlen(candidates[selection - 1].path);
        if (path_length >= path_size)
        {
                fprintf(stderr, "%s Selected device path is too long.\n", ERROR_PREFIX);
                goto cleanup;
        }

        strcpy(device_path, candidates[selection - 1].path);
        printf("\n%s Selected device: %s\n", STATUS_PREFIX, device_path);
        result = 0;

cleanup:
        if (device != NULL)
        {
                udev_device_unref(device);
        }

        free(candidates);

        if (enumerate != NULL)
        {
                udev_enumerate_unref(enumerate);
        }

        if (udev != NULL)
        {
                udev_unref(udev);
        }

        return result;
}

// Open and store validated camera descriptor.
int camera_open(struct camera_device *camera, const char *device_path)
{
        uint32_t capabilities;
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

        fd = camera_validate_device(device_path, &capabilities);
        if (fd == -1)
        {
                perror("ERROR: Failed to open or validate camera device");
                return -1;
        }

        camera->fd = fd;
        camera->capabilities = capabilities;

        printf("%s Camera device opened: %s\n", STATUS_PREFIX, device_path);
        printf("%s File descriptor: %d\n", STATUS_PREFIX, camera->fd);
        return 0;
}

// Query and display V4L2 driver info and device capabilities.
int camera_query_capabilities(struct camera_device *camera)
{
        struct v4l2_capability capability;
        uint32_t effective_caps;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        memset(&capability, 0, sizeof(capability));
        if (camera_ioctl(camera->fd, VIDIOC_QUERYCAP, &capability) == -1)
        {
                perror("ERROR: VIDIOC_QUERYCAP failed");
                return -1;
        }

        if (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
        {
                effective_caps = capability.device_caps;
        }
        else
        {
                effective_caps = capability.capabilities;
        }

        camera->capabilities = effective_caps;

        printf("\n========== CAMERA INFORMATION ==========\n");
        printf("Driver        : %s\n", capability.driver);
        printf("Card          : %s\n", capability.card);
        printf("Bus Info      : %s\n", capability.bus_info);
        printf("Driver Version: %u.%u.%u\n", (capability.version >> SHIFT_16_BITS) & BYTE_MASK, (capability.version >> SHIFT_8_BITS) & BYTE_MASK, capability.version & BYTE_MASK);

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

// Enumerate supported pixel formats along with frame sizes and intervals.
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
                printf("Pixel Format     : ");
                camera_print_fourcc(fmt.pixelformat);
                printf("\n");

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

// Enumerate discrete or stepwise/continuous frame sizes for a pixel format.
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

// Enumerate supported frame intervals (FPS) for a pixel format and resolution.
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
                        if (interval.stepwise.min.numerator == 0 || interval.stepwise.min.denominator == 0 || interval.stepwise.max.numerator == 0 || interval.stepwise.max.denominator == 0)
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
                                printf("    Frame interval step: %u/%u seconds\n", interval.stepwise.step.numerator, interval.stepwise.step.denominator);
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

// Read and validate a uint32_t value from standard input.
static int camera_read_number(const char *prompt, uint32_t *value)
{
        char input[INPUT_BUFFER_SIZE];
        char *end_pointer;
        unsigned long number;
        int character;

        if (value == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        printf("%s", prompt);
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
        {
                perror("ERROR: Failed to read numeric input");
                return -1;
        }

        if (strchr(input, '\n') == NULL && !feof(stdin))
        {
                while ((character = getchar()) != '\n' && character != EOF)
                {
                }

                fprintf(stderr, "%s Input is too long.\n", ERROR_PREFIX);
                return -1;
        }

        errno = 0;
        number = strtoul(input, &end_pointer, DECIMAL_BASE);

        while (isspace((unsigned char)*end_pointer))
        {
                end_pointer++;
        }

        if (errno == ERANGE || end_pointer == input || *end_pointer != '\0' || number > UINT32_MAX)
        {
                fprintf(stderr, "%s Invalid numeric input.\n", ERROR_PREFIX);
                return -1;
        }

        *value = (uint32_t)number;
        return 0;
}

// Validate width and height against a reported stepwise or continuous range.
static int camera_validate_resolution(uint32_t width, uint32_t height, const struct v4l2_frmsizeenum *range)
{
        if (range == NULL || width == 0 || height == 0)
        {
                return -1;
        }

        if (width < range->stepwise.min_width || width > range->stepwise.max_width || height < range->stepwise.min_height || height > range->stepwise.max_height)
        {
                return -1;
        }

        if (range->type == V4L2_FRMSIZE_TYPE_STEPWISE)
        {
                if (range->stepwise.step_width == 0 || range->stepwise.step_height == 0)
                {
                        return -1;
                }

                if ((width - range->stepwise.min_width) % range->stepwise.step_width != 0 || (height - range->stepwise.min_height) % range->stepwise.step_height != 0)
                {
                        return -1;
                }
        }

        return 0;
}

// Prompt for and validate a custom width and height against a supported range.
static int camera_prompt_custom_resolution(const struct v4l2_frmsizeenum *range, uint32_t *width, uint32_t *height)
{
        printf("\nEnter the desired resolution.\n");
        if (camera_read_number("Width: ", width) == -1 || camera_read_number("Height: ", height) == -1)
        {
                return -1;
        }

        if (camera_validate_resolution(*width, *height, range) == -1)
        {
                fprintf(stderr, "%s Resolution is outside the supported range or step.\n", ERROR_PREFIX);
                return -1;
        }

        return 0;
}

// Prompt user to choose pixel format and resolution, then apply via VIDIOC_S_FMT.
int camera_configure_format(struct camera_device *camera, struct v4l2_pix_format *applied_format)
{
        struct v4l2_fmtdesc format;
        struct v4l2_frmsizeenum frame_size;
        struct v4l2_frmsizeenum frame_size_range;
        struct v4l2_format selected_format;
        struct v4l2_frmsizeenum *discrete_sizes = NULL;
        struct v4l2_frmsizeenum *new_sizes;
        uint32_t format_selection;
        uint32_t resolution_selection;
        uint32_t width;
        uint32_t height;
        unsigned int format_index = 0;
        size_t discrete_count = 0;
        size_t index;
        bool range_found = false;
        int result = -1;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== PIXEL FORMAT SELECTION ==========\n");

        while (1)
        {
                memset(&format, 0, sizeof(format));
                format.index = format_index;
                format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                if (camera_ioctl(camera->fd, VIDIOC_ENUM_FMT, &format) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FMT failed");
                        goto cleanup;
                }

                printf("[%u] %s (", format_index, format.description);
                camera_print_fourcc(format.pixelformat);
                printf(")\n");

                format_index++;
        }

        if (format_index == 0)
        {
                fprintf(stderr, "%s No pixel formats available.\n", ERROR_PREFIX);
                goto cleanup;
        }

        if (camera_read_number("\nSelect pixel format: ", &format_selection) == -1)
        {
                goto cleanup;
        }

        if (format_selection >= format_index)
        {
                fprintf(stderr, "%s Invalid pixel format selection.\n", ERROR_PREFIX);
                goto cleanup;
        }

        memset(&format, 0, sizeof(format));
        format.index = format_selection;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (camera_ioctl(camera->fd, VIDIOC_ENUM_FMT, &format) == -1)
        {
                perror("ERROR: Failed to retrieve selected pixel format");
                goto cleanup;
        }

        printf("\n%s Selected pixel format: %s\n", STATUS_PREFIX, format.description);

        printf("\n========== SUPPORTED RESOLUTIONS ==========\n");

        index = 0;
        while (1)
        {
                memset(&frame_size, 0, sizeof(frame_size));
                frame_size.index = (unsigned int)index;
                frame_size.pixel_format = format.pixelformat;

                if (camera_ioctl(camera->fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) == -1)
                {
                        if (errno == EINVAL)
                        {
                                break;
                        }

                        perror("ERROR: VIDIOC_ENUM_FRAMESIZES failed");
                        goto cleanup;
                }

                if (frame_size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
                {
                        new_sizes = realloc(discrete_sizes, (discrete_count + 1) * sizeof(*discrete_sizes));
                        if (new_sizes == NULL)
                        {
                                perror("ERROR: Failed to allocate discrete resolutions");
                                goto cleanup;
                        }

                        discrete_sizes = new_sizes;
                        discrete_sizes[discrete_count] = frame_size;

                        printf("[%zu] %u x %u\n", discrete_count + 1, frame_size.discrete.width, frame_size.discrete.height);
                        if (camera_enumerate_frame_intervals(camera, format.pixelformat, frame_size.discrete.width, frame_size.discrete.height) == -1)
                        {
                                goto cleanup;
                        }

                        discrete_count++;
                }
                else if (frame_size.type == V4L2_FRMSIZE_TYPE_STEPWISE || frame_size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
                {
                        frame_size_range = frame_size;
                        range_found = true;

                        printf("\nCustom resolution range:\n");
                        printf("Minimum: %u x %u\n", frame_size.stepwise.min_width, frame_size.stepwise.min_height);
                        printf("Maximum: %u x %u\n", frame_size.stepwise.max_width, frame_size.stepwise.max_height);

                        if (frame_size.type == V4L2_FRMSIZE_TYPE_STEPWISE)
                        {
                                printf("Step: %u x %u\n", frame_size.stepwise.step_width, frame_size.stepwise.step_height);
                        }

                        break;
                }

                index++;
        }

        if (discrete_count == 0 && !range_found)
        {
                fprintf(stderr, "%s No usable resolutions reported.\n", ERROR_PREFIX);
                goto cleanup;
        }

        printf("\n========== RESOLUTION SELECTION ==========\n");

        if (discrete_count > 0)
        {
                printf("Select a resolution:");
                if (range_found)
                {
                        printf("\n[0] Enter a custom resolution");
                }
                printf("\n");

                if (camera_read_number("Resolution selection: ", &resolution_selection) == -1)
                {
                        goto cleanup;
                }

                if (resolution_selection == 0 && range_found)
                {
                        if (camera_prompt_custom_resolution(&frame_size_range, &width, &height) == -1)
                        {
                                goto cleanup;
                        }
                }
                else
                {
                        if (resolution_selection == 0 || resolution_selection > discrete_count)
                        {
                                fprintf(stderr, "%s Invalid resolution selection.\n", ERROR_PREFIX);
                                goto cleanup;
                        }

                        width = discrete_sizes[resolution_selection - 1].discrete.width;
                        height = discrete_sizes[resolution_selection - 1].discrete.height;
                }
        }
        else
        {
                if (camera_prompt_custom_resolution(&frame_size_range, &width, &height) == -1)
                {
                        goto cleanup;
                }
        }

        printf("\n%s Requested resolution: %u x %u\n", STATUS_PREFIX, width, height);

        memset(&selected_format, 0, sizeof(selected_format));
        selected_format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        selected_format.fmt.pix.width = width;
        selected_format.fmt.pix.height = height;
        selected_format.fmt.pix.pixelformat = format.pixelformat;
        selected_format.fmt.pix.field = V4L2_FIELD_ANY;

        if (camera_ioctl(camera->fd, VIDIOC_S_FMT, &selected_format) == -1)
        {
                perror("ERROR: VIDIOC_S_FMT failed");
                goto cleanup;
        }

        if (applied_format != NULL)
        {
                *applied_format = selected_format.fmt.pix;
        }

        printf("\n========== APPLIED CAMERA FORMAT ==========\n");
        printf("Pixel Format : ");
        camera_print_fourcc(selected_format.fmt.pix.pixelformat);
        printf("\n");
        printf("Width        : %u\n", selected_format.fmt.pix.width);
        printf("Height       : %u\n", selected_format.fmt.pix.height);
        printf("Bytes/Line   : %u\n", selected_format.fmt.pix.bytesperline);
        printf("Image Size   : %u bytes\n", selected_format.fmt.pix.sizeimage);
        printf("Field        : %u\n", selected_format.fmt.pix.field);
        printf("Colorspace   : %u\n", selected_format.fmt.pix.colorspace);

        printf("\n%s Camera format configured successfully.\n", STATUS_PREFIX);
        result = 0;

cleanup:
        free(discrete_sizes);
        return result;
}

// Convert a frame interval fraction (seconds/frame) into frames per second.
static double camera_fract_to_fps(struct v4l2_fract interval)
{
        if (interval.numerator == 0 || interval.denominator == 0)
        {
                return ZERO_DOUBLE;
        }

        return (double)interval.denominator / (double)interval.numerator;
}

// Query current capture parameters and frame interval via VIDIOC_G_PARM.
static int camera_get_current_interval(struct camera_device *camera, struct v4l2_fract *current_interval, bool *supports_timeperframe)
{
        struct v4l2_streamparm parm;

        if (camera == NULL || camera->fd < 0 || current_interval == NULL || supports_timeperframe == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (camera_ioctl(camera->fd, VIDIOC_G_PARM, &parm) == -1)
        {
                perror("ERROR: VIDIOC_G_PARM failed");
                return -1;
        }

        *supports_timeperframe = ((parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) != 0);
        *current_interval = parm.parm.capture.timeperframe;
        return 0;
}

// Apply a target frame interval via VIDIOC_S_PARM and print driver result.
static int camera_set_interval(struct camera_device *camera, struct v4l2_fract target_interval)
{
        struct v4l2_streamparm parm;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe = target_interval;

        if (camera_ioctl(camera->fd, VIDIOC_S_PARM, &parm) == -1)
        {
                perror("ERROR: VIDIOC_S_PARM failed");
                return -1;
        }

        printf("\n========== APPLIED FRAME INTERVAL ==========\n");
        printf("Interval  : %u/%u sec\n", parm.parm.capture.timeperframe.numerator, parm.parm.capture.timeperframe.denominator);
        printf("Frame Rate: %.2f FPS\n", camera_fract_to_fps(parm.parm.capture.timeperframe));
        printf("%s Frame interval configured successfully.\n", STATUS_PREFIX);
        return 0;
}

// Prompt for target FPS within a continuous or stepwise range and convert to v4l2_fract.
static int camera_select_target_fps_from_range(const struct v4l2_frmivalenum *range, struct v4l2_fract *chosen_interval)
{
        uint32_t target_fps = 0;
        double min_fps;
        double max_fps;
        double requested_interval_sec;
        double min_interval_sec;
        double max_interval_sec;

        if (range == NULL || chosen_interval == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        min_fps = camera_fract_to_fps(range->stepwise.max);
        max_fps = camera_fract_to_fps(range->stepwise.min);

        printf("\nAllowed FPS range: %.2f FPS (min) to %.2f FPS (max)\n", min_fps, max_fps);
        if (range->type == V4L2_FRMIVAL_TYPE_STEPWISE)
        {
                printf("Interval step: %u/%u sec\n", range->stepwise.step.numerator, range->stepwise.step.denominator);
        }

        if (camera_read_number("Enter target FPS: ", &target_fps) == -1)
        {
                return -1;
        }

        if (target_fps == 0 || (double)target_fps < min_fps || (double)target_fps > max_fps)
        {
                fprintf(stderr, "%s Target FPS is outside the supported range.\n", ERROR_PREFIX);
                return -1;
        }

        requested_interval_sec = UNIT_SECONDS / (double)target_fps;
        min_interval_sec = (double)range->stepwise.min.numerator / (double)range->stepwise.min.denominator;
        max_interval_sec = (double)range->stepwise.max.numerator / (double)range->stepwise.max.denominator;

        if (range->type == V4L2_FRMIVAL_TYPE_STEPWISE)
        {
                double step_sec = (double)range->stepwise.step.numerator / (double)range->stepwise.step.denominator;
                double diff_from_min = requested_interval_sec - min_interval_sec;

                if (step_sec <= ZERO_DOUBLE || requested_interval_sec < min_interval_sec || requested_interval_sec > max_interval_sec)
                {
                        fprintf(stderr, "%s Target interval is outside stepwise bounds.\n", ERROR_PREFIX);
                        return -1;
                }

                // Verify target interval aligns with the driver's stepwise increment.
                double steps = diff_from_min / step_sec;
                double nearest_step = (double)(uint32_t)(steps + ROUNDING_OFFSET);
                if (steps - nearest_step > STEP_TOLERANCE || nearest_step - steps > STEP_TOLERANCE)
                {
                        fprintf(stderr, "%s Target FPS does not align with supported frame interval step.\n", ERROR_PREFIX);
                        return -1;
                }
        }

        chosen_interval->numerator = 1U;
        chosen_interval->denominator = target_fps;

        // Scale numerator and denominator when target FPS needs fractional precision.
        if ((double)chosen_interval->denominator != (double)target_fps)
        {
                chosen_interval->numerator = FPS_SCALE_FACTOR;
                chosen_interval->denominator = target_fps * FPS_SCALE_FACTOR;
        }

        return 0;
}

// Enumerate and let user select a frame interval for the applied format and resolution.
int camera_configure_frame_interval(struct camera_device *camera, uint32_t pixel_format, uint32_t width, uint32_t height)
{
        struct v4l2_frmivalenum interval;
        struct v4l2_frmivalenum *discrete_intervals = NULL;
        struct v4l2_frmivalenum *new_intervals;
        struct v4l2_frmivalenum range_info;
        struct v4l2_fract current_interval;
        struct v4l2_fract chosen_interval;
        bool supports_timeperframe = false;
        bool range_found = false;
        bool apply_interval = false;
        unsigned int enum_index = 0;
        size_t discrete_count = 0;
        uint32_t choice = 0;
        int result = -1;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device.\n", ERROR_PREFIX);
                return -1;
        }

        if (camera_get_current_interval(camera, &current_interval, &supports_timeperframe) == -1)
        {
                return -1;
        }

        printf("\n========== FRAME INTERVAL SELECTION ==========\n");
        if (current_interval.numerator > 0 && current_interval.denominator > 0)
        {
                printf("Current frame rate: %.2f FPS (%u/%u sec)\n", camera_fract_to_fps(current_interval), current_interval.numerator, current_interval.denominator);
        }

        memset(&range_info, 0, sizeof(range_info));
        memset(&chosen_interval, 0, sizeof(chosen_interval));

        while (1)
        {
                memset(&interval, 0, sizeof(interval));
                interval.index = enum_index;
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
                        goto cleanup;
                }

                if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
                {
                        if (interval.discrete.numerator == 0 || interval.discrete.denominator == 0)
                        {
                                fprintf(stderr, "%s Driver reported invalid discrete interval.\n", ERROR_PREFIX);
                                goto cleanup;
                        }

                        new_intervals = realloc(discrete_intervals, (discrete_count + 1) * sizeof(*discrete_intervals));
                        if (new_intervals == NULL)
                        {
                                perror("ERROR: Failed to allocate discrete frame intervals");
                                goto cleanup;
                        }

                        discrete_intervals = new_intervals;
                        discrete_intervals[discrete_count] = interval;
                        discrete_count++;
                }
                else if (interval.type == V4L2_FRMIVAL_TYPE_CONTINUOUS || interval.type == V4L2_FRMIVAL_TYPE_STEPWISE)
                {
                        if (interval.stepwise.min.numerator == 0 || interval.stepwise.min.denominator == 0 || interval.stepwise.max.numerator == 0 || interval.stepwise.max.denominator == 0)
                        {
                                fprintf(stderr, "%s Driver reported invalid frame interval range.\n", ERROR_PREFIX);
                                goto cleanup;
                        }

                        range_info = interval;
                        range_found = true;
                        break;
                }
                else
                {
                        printf("%s Unsupported frame interval type reported.\n", STATUS_PREFIX);
                        break;
                }

                enum_index++;
        }

        if (discrete_count == 0 && !range_found)
        {
                printf("%s No frame intervals enumerated; keeping current driver frame rate.\n", STATUS_PREFIX);
                result = 0;
                goto cleanup;
        }

        if (!supports_timeperframe)
        {
                printf("%s Driver does not advertise V4L2_CAP_TIMEPERFRAME; keeping current frame rate.\n", STATUS_PREFIX);
                result = 0;
                goto cleanup;
        }

        if (discrete_count > 0)
        {
                size_t i;

                printf("\nAvailable discrete frame rates:\n");
                printf("  [0] Keep current driver frame rate\n");
                for (i = 0; i < discrete_count; i++)
                {
                        double fps = camera_fract_to_fps(discrete_intervals[i].discrete);
                        printf("  [%zu] %.2f FPS (%u/%u sec)\n", i + 1, fps, discrete_intervals[i].discrete.numerator, discrete_intervals[i].discrete.denominator);
                }
                if (range_found)
                {
                        printf("  [%zu] Enter custom target FPS from range\n", discrete_count + 1);
                }

                if (camera_read_number("Select frame interval option: ", &choice) == -1)
                {
                        goto cleanup;
                }

                if (choice == 0)
                {
                        printf("%s Keeping current driver frame rate.\n", STATUS_PREFIX);
                }
                else if ((size_t)choice <= discrete_count)
                {
                        chosen_interval = discrete_intervals[choice - 1].discrete;
                        apply_interval = true;
                }
                else if (range_found && (size_t)choice == discrete_count + 1)
                {
                        if (camera_select_target_fps_from_range(&range_info, &chosen_interval) == -1)
                        {
                                goto cleanup;
                        }
                        apply_interval = true;
                }
                else
                {
                        fprintf(stderr, "%s Invalid frame interval selection.\n", ERROR_PREFIX);
                        goto cleanup;
                }
        }
        else
        {
                printf("\n  [0] Keep current driver frame rate\n");
                printf("  [1] Enter custom target FPS from range\n");

                if (camera_read_number("Select frame interval option: ", &choice) == -1)
                {
                        goto cleanup;
                }

                if (choice == 0)
                {
                        printf("%s Keeping current driver frame rate.\n", STATUS_PREFIX);
                }
                else if (choice == 1)
                {
                        if (camera_select_target_fps_from_range(&range_info, &chosen_interval) == -1)
                        {
                                goto cleanup;
                        }
                        apply_interval = true;
                }
                else
                {
                        fprintf(stderr, "%s Invalid frame interval selection.\n", ERROR_PREFIX);
                        goto cleanup;
                }
        }

        if (apply_interval)
        {
                if (camera_set_interval(camera, chosen_interval) == -1)
                {
                        goto cleanup;
                }
        }

        result = 0;

cleanup:
        free(discrete_intervals);
        return result;
}

// Return human-readable label for a streaming I/O method.
static const char *camera_io_method_name(enum camera_io_method method)
{
        if (method == CAMERA_IO_MMAP)
        {
                return "Memory Mapped I/O (MMAP)";
        }

        if (method == CAMERA_IO_USERPTR)
        {
                return "User Pointer I/O (USERPTR)";
        }

        if (method == CAMERA_IO_DMABUF)
        {
                return "DMA Buffer I/O (DMABUF)";
        }

        return "Unknown I/O method";
}

// Populate supported streaming I/O methods and prompt user to select one.
static int camera_select_io_method(struct camera_device *camera, enum camera_io_method *selected_method)
{
        enum camera_io_method supported_methods[CAMERA_IO_METHOD_COUNT];
        size_t supported_count = 0;
        size_t index;
        uint32_t choice = 0;

        if (camera == NULL || camera->fd < 0 || selected_method == NULL)
        {
                fprintf(stderr, "%s Invalid arguments for I/O method selection.\n", ERROR_PREFIX);
                return -1;
        }

        if (!(camera->capabilities & V4L2_CAP_STREAMING))
        {
                fprintf(stderr, "%s Device does not support V4L2_CAP_STREAMING.\n", ERROR_PREFIX);
                return -1;
        }

        supported_methods[supported_count] = CAMERA_IO_MMAP;
        supported_count++;
        supported_methods[supported_count] = CAMERA_IO_USERPTR;
        supported_count++;
        supported_methods[supported_count] = CAMERA_IO_DMABUF;
        supported_count++;

        printf("\n========== I/O METHOD SELECTION ==========\n");
        for (index = 0; index < supported_count; index++)
        {
                printf("  [%zu] %s\n", index + 1, camera_io_method_name(supported_methods[index]));
        }

        if (camera_read_number("Select I/O method: ", &choice) == -1)
        {
                return -1;
        }

        if (choice < CAMERA_MIN_FRAME_COUNT || (size_t)choice > supported_count)
        {
                fprintf(stderr, "%s Invalid I/O method selection.\n", ERROR_PREFIX);
                return -1;
        }

        *selected_method = supported_methods[choice - 1];
        printf("%s Selected I/O method: %s\n", STATUS_PREFIX, camera_io_method_name(*selected_method));
        return 0;
}

// Prompt user for frame count and validate against allowed bounds.
static int camera_select_frame_count(uint32_t *frame_count)
{
        uint32_t count = 0;

        if (frame_count == NULL)
        {
                fprintf(stderr, "%s Invalid frame count destination pointer.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== FRAME COUNT SELECTION ==========\n");
        printf("Allowed frame count range: %u to %u\n", CAMERA_MIN_FRAME_COUNT, CAMERA_MAX_FRAME_COUNT);

        if (camera_read_number("Enter number of frames to capture: ", &count) == -1)
        {
                return -1;
        }

        if (count < CAMERA_MIN_FRAME_COUNT || count > CAMERA_MAX_FRAME_COUNT)
        {
                fprintf(stderr, "%s Frame count must be between %u and %u.\n", ERROR_PREFIX, CAMERA_MIN_FRAME_COUNT, CAMERA_MAX_FRAME_COUNT);
                return -1;
        }

        *frame_count = count;
        printf("%s Selected frame count: %u\n", STATUS_PREFIX, *frame_count);
        return 0;
}

// Configure streaming I/O method and frame count after format and interval setup.
int camera_configure_capture(struct camera_device *camera, struct camera_capture_config *config)
{
        enum camera_io_method method = CAMERA_IO_NONE;
        uint32_t frame_count = 0;

        if (camera == NULL || camera->fd < 0 || config == NULL)
        {
                fprintf(stderr, "%s Invalid capture configuration parameters.\n", ERROR_PREFIX);
                return -1;
        }

        if (camera_select_io_method(camera, &method) == -1)
        {
                return -1;
        }

        if (camera_select_frame_count(&frame_count) == -1)
        {
                return -1;
        }

        config->io_method = method;
        config->frame_count = frame_count;

        printf("\n%s Capture settings configured (Method: %s, Frames: %u).\n", STATUS_PREFIX, camera_io_method_name(config->io_method), config->frame_count);
        return 0;
}

// Map internal I/O method enum to V4L2 memory type.
static enum v4l2_memory camera_io_to_v4l2_memory(enum camera_io_method method)
{
        if (method == CAMERA_IO_MMAP)
        {
                return V4L2_MEMORY_MMAP;
        }

        if (method == CAMERA_IO_USERPTR)
        {
                return V4L2_MEMORY_USERPTR;
        }

        if (method == CAMERA_IO_DMABUF)
        {
                return V4L2_MEMORY_DMABUF;
        }

        return (enum v4l2_memory)0;
}

// Round a byte size up to a required alignment boundary.
static size_t camera_align_up_size(size_t size, size_t alignment)
{
        size_t remainder;

        if (alignment == 0)
        {
                return size;
        }

        remainder = size % alignment;
        if (remainder == 0)
        {
                return size;
        }

        return size + (alignment - remainder);
}

// Request V4L2 streaming buffers via VIDIOC_REQBUFS.
static int camera_request_buffers(struct camera_device *camera, enum v4l2_memory memory, uint32_t *allocated_count)
{
        struct v4l2_requestbuffers req;

        if (camera == NULL || camera->fd < 0 || allocated_count == NULL)
        {
                errno = EINVAL;
                return -1;
        }

        memset(&req, 0, sizeof(req));
        req.count = CAMERA_DEFAULT_BUFFER_COUNT;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = memory;

        if (camera_ioctl(camera->fd, VIDIOC_REQBUFS, &req) == -1)
        {
                perror("ERROR: VIDIOC_REQBUFS failed");
                return -1;
        }

        if (req.count < CAMERA_MIN_BUFFER_COUNT)
        {
                fprintf(stderr, "%s Insufficient buffers allocated by driver (%u < %u).\n", ERROR_PREFIX, req.count, CAMERA_MIN_BUFFER_COUNT);
                return -1;
        }

        *allocated_count = req.count;
        printf("%s Driver allocated %u V4L2 buffers.\n", STATUS_PREFIX, *allocated_count);
        return 0;
}

// Query and mmap buffers into user space for MMAP streaming.
static int camera_init_mmap_buffers(struct camera_device *camera, struct camera_buffer_pool *pool)
{
        struct v4l2_buffer buf;
        uint32_t i;

        for (i = 0; i < pool->count; i++)
        {
                memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;

                if (camera_ioctl(camera->fd, VIDIOC_QUERYBUF, &buf) == -1)
                {
                        perror("ERROR: VIDIOC_QUERYBUF failed");
                        return -1;
                }

                pool->buffers[i].length = buf.length;
                pool->buffers[i].memory = V4L2_MEMORY_MMAP;
                pool->buffers[i].dmabuf_fd = -1;
                pool->buffers[i].index = i;
                pool->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd, buf.m.offset);

                if (pool->buffers[i].start == MAP_FAILED)
                {
                        pool->buffers[i].start = NULL;
                        perror("ERROR: mmap failed for V4L2 buffer");
                        return -1;
                }
        }

        printf("%s Mapped %u MMAP buffers successfully.\n", STATUS_PREFIX, pool->count);
        return 0;
}

// Allocate page-aligned user-space buffers for USERPTR streaming.
static int camera_init_userptr_buffers(struct camera_buffer_pool *pool, size_t frame_size)
{
        size_t aligned_size = camera_align_up_size(frame_size, CAMERA_BUFFER_ALIGNMENT);
        uint32_t i;

        for (i = 0; i < pool->count; i++)
        {
                pool->buffers[i].length = aligned_size;
                pool->buffers[i].memory = V4L2_MEMORY_USERPTR;
                pool->buffers[i].dmabuf_fd = -1;
                pool->buffers[i].index = i;
                pool->buffers[i].start = aligned_alloc(CAMERA_BUFFER_ALIGNMENT, aligned_size);

                if (pool->buffers[i].start == NULL)
                {
                        perror("ERROR: aligned_alloc failed for USERPTR buffer");
                        return -1;
                }
        }

        printf("%s Allocated %u USERPTR buffers (%zu bytes each).\n", STATUS_PREFIX, pool->count, aligned_size);
        return 0;
}

// Allocate DMA-BUF file descriptors from system heap for DMABUF streaming.
static int camera_init_dmabuf_buffers(struct camera_buffer_pool *pool, size_t frame_size)
{
        struct dma_heap_allocation_data heap_data;
        size_t aligned_size = camera_align_up_size(frame_size, CAMERA_BUFFER_ALIGNMENT);
        int heap_fd;
        uint32_t i;

        heap_fd = open(CAMERA_DMA_HEAP_PATH, O_RDONLY);
        if (heap_fd == -1)
        {
                perror("ERROR: Failed to open DMA heap device");
                return -1;
        }

        for (i = 0; i < pool->count; i++)
        {
                memset(&heap_data, 0, sizeof(heap_data));
                heap_data.len = aligned_size;
                heap_data.fd_flags = O_RDWR;

                if (camera_ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap_data) == -1)
                {
                        perror("ERROR: DMA_HEAP_IOCTL_ALLOC failed");
                        close(heap_fd);
                        return -1;
                }

                pool->buffers[i].length = aligned_size;
                pool->buffers[i].memory = V4L2_MEMORY_DMABUF;
                pool->buffers[i].dmabuf_fd = (int)heap_data.fd;
                pool->buffers[i].start = NULL;
                pool->buffers[i].index = i;
        }

        if (close(heap_fd) == -1)
        {
                perror("ERROR: Failed to close DMA heap descriptor");
                return -1;
        }

        printf("%s Allocated %u DMABUF buffers (%zu bytes each).\n", STATUS_PREFIX, pool->count, aligned_size);
        return 0;
}

// Allocate and initialize V4L2 buffers for the chosen I/O method.
int camera_setup_buffers(struct camera_device *camera, const struct camera_capture_config *config, uint32_t frame_size, struct camera_buffer_pool *pool)
{
        enum v4l2_memory memory;
        uint32_t count = 0;
        uint32_t i;

        if (camera == NULL || camera->fd < 0 || config == NULL || pool == NULL || frame_size == 0)
        {
                fprintf(stderr, "%s Invalid arguments for buffer setup.\n", ERROR_PREFIX);
                return -1;
        }

        pool->buffers = NULL;
        pool->count = 0;
        pool->io_method = config->io_method;

        memory = camera_io_to_v4l2_memory(config->io_method);
        if (memory == (enum v4l2_memory)0)
        {
                fprintf(stderr, "%s Unsupported I/O method for buffer setup.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== BUFFER SETUP ==========\n");

        if (camera_request_buffers(camera, memory, &count) == -1)
        {
                return -1;
        }

        pool->buffers = calloc(count, sizeof(*pool->buffers));
        if (pool->buffers == NULL)
        {
                perror("ERROR: Failed to allocate buffer tracking array");
                return -1;
        }

        pool->count = count;
        for (i = 0; i < pool->count; i++)
        {
                pool->buffers[i].dmabuf_fd = -1;
                pool->buffers[i].memory = memory;
                pool->buffers[i].index = i;
        }

        if (config->io_method == CAMERA_IO_MMAP)
        {
                if (camera_init_mmap_buffers(camera, pool) == -1)
                {
                        camera_release_buffers(pool);
                        return -1;
                }
        }
        else if (config->io_method == CAMERA_IO_USERPTR)
        {
                if (camera_init_userptr_buffers(pool, (size_t)frame_size) == -1)
                {
                        camera_release_buffers(pool);
                        return -1;
                }
        }
        else if (config->io_method == CAMERA_IO_DMABUF)
        {
                if (camera_init_dmabuf_buffers(pool, (size_t)frame_size) == -1)
                {
                        camera_release_buffers(pool);
                        return -1;
                }
        }

        printf("%s Buffer setup completed successfully.\n", STATUS_PREFIX);
        return 0;
}

// Release all mapped, allocated, or DMA-BUF buffers and free pool storage.
void camera_release_buffers(struct camera_buffer_pool *pool)
{
        uint32_t i;

        if (pool == NULL || pool->buffers == NULL)
        {
                return;
        }

        for (i = 0; i < pool->count; i++)
        {
                if (pool->buffers[i].memory == V4L2_MEMORY_MMAP && pool->buffers[i].start != NULL && pool->buffers[i].length > 0)
                {
                        if (munmap(pool->buffers[i].start, pool->buffers[i].length) == -1)
                        {
                                perror("ERROR: munmap failed");
                        }
                        pool->buffers[i].start = NULL;
                }
                else if (pool->buffers[i].memory == V4L2_MEMORY_USERPTR && pool->buffers[i].start != NULL)
                {
                        free(pool->buffers[i].start);
                        pool->buffers[i].start = NULL;
                }
                else if (pool->buffers[i].memory == V4L2_MEMORY_DMABUF && pool->buffers[i].dmabuf_fd >= 0)
                {
                        if (close(pool->buffers[i].dmabuf_fd) == -1)
                        {
                                perror("ERROR: Failed to close DMA buffer fd");
                        }
                        pool->buffers[i].dmabuf_fd = -1;
                }
        }

        free(pool->buffers);
        pool->buffers = NULL;
        pool->count = 0;
        pool->io_method = CAMERA_IO_NONE;
        printf("%s Buffers released.\n", STATUS_PREFIX);
}

// Start or stop V4L2 video capture streaming via VIDIOC_STREAMON or VIDIOC_STREAMOFF.
static int camera_set_stream_state(struct camera_device *camera, unsigned long request, const char *action_name)
{
        enum v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (camera == NULL || camera->fd < 0 || action_name == NULL)
        {
                fprintf(stderr, "%s Invalid arguments for %s.\n", ERROR_PREFIX, action_name != NULL ? action_name : "stream control");
                return -1;
        }

        if (camera_ioctl(camera->fd, request, &buf_type) == -1)
        {
                perror(action_name);
                return -1;
        }

        return 0;
}

// Queue every buffer in the buffer pool to the V4L2 capture driver via VIDIOC_QBUF.
int camera_queue_all_buffers(struct camera_device *camera, const struct camera_buffer_pool *pool)
{
        struct v4l2_buffer buf;
        uint32_t i;

        if (camera == NULL || camera->fd < 0 || pool == NULL || pool->buffers == NULL || pool->count == 0)
        {
                fprintf(stderr, "%s Invalid camera or buffer pool for queueing.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== BUFFER QUEUEING ==========\n");

        for (i = 0; i < pool->count; i++)
        {
                memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = pool->buffers[i].memory;
                buf.index = pool->buffers[i].index;

                if (pool->buffers[i].memory == V4L2_MEMORY_USERPTR)
                {
                        if (pool->buffers[i].start == NULL || pool->buffers[i].length == 0)
                        {
                                fprintf(stderr, "%s Invalid USERPTR buffer at index %u.\n", ERROR_PREFIX, i);
                                return -1;
                        }

                        buf.m.userptr = (unsigned long)(uintptr_t)pool->buffers[i].start;
                        buf.length = (__u32)pool->buffers[i].length;
                }
                else if (pool->buffers[i].memory == V4L2_MEMORY_DMABUF)
                {
                        if (pool->buffers[i].dmabuf_fd < 0 || pool->buffers[i].length == 0)
                        {
                                fprintf(stderr, "%s Invalid DMABUF buffer at index %u.\n", ERROR_PREFIX, i);
                                return -1;
                        }

                        buf.m.fd = pool->buffers[i].dmabuf_fd;
                        buf.length = (__u32)pool->buffers[i].length;
                }

                if (camera_ioctl(camera->fd, VIDIOC_QBUF, &buf) == -1)
                {
                        perror("ERROR: VIDIOC_QBUF failed");
                        return -1;
                }
        }

        printf("%s Queued %u buffers successfully.\n", STATUS_PREFIX, pool->count);
        return 0;
}

// Start V4L2 capture stream via VIDIOC_STREAMON.
int camera_stream_on(struct camera_device *camera)
{
        printf("\n========== STREAM CONTROL ==========\n");

        if (camera_set_stream_state(camera, VIDIOC_STREAMON, "ERROR: VIDIOC_STREAMON failed") == -1)
        {
                return -1;
        }

        printf("%s Video capture stream started (VIDIOC_STREAMON).\n", STATUS_PREFIX);
        return 0;
}

// Stop V4L2 capture stream via VIDIOC_STREAMOFF.
int camera_stream_off(struct camera_device *camera)
{
        if (camera_set_stream_state(camera, VIDIOC_STREAMOFF, "ERROR: VIDIOC_STREAMOFF failed") == -1)
        {
                return -1;
        }

        printf("%s Video capture stream stopped (VIDIOC_STREAMOFF).\n", STATUS_PREFIX);
        return 0;
}

// Wait for capture buffer readiness with poll(), retrying on EINTR.
static int camera_wait_for_frame(struct camera_device *camera)
{
        struct pollfd pfd;
        int poll_result;

        if (camera == NULL || camera->fd < 0)
        {
                fprintf(stderr, "%s Invalid camera device for poll.\n", ERROR_PREFIX);
                return -1;
        }

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = camera->fd;
        pfd.events = POLLIN;

        do
        {
                poll_result = poll(&pfd, CAMERA_MIN_FRAME_COUNT, CAMERA_POLL_TIMEOUT_MS);
        }
        while (poll_result == -1 && errno == EINTR);

        if (poll_result == -1)
        {
                perror("ERROR: poll failed while waiting for frame");
                return -1;
        }

        if (poll_result == 0)
        {
                fprintf(stderr, "%s Timed out waiting for frame after %d ms.\n", ERROR_PREFIX, CAMERA_POLL_TIMEOUT_MS);
                return -1;
        }

        if (pfd.revents & (POLLERR | POLLNVAL))
        {
                fprintf(stderr, "%s Poll error event on camera file descriptor.\n", ERROR_PREFIX);
                return -1;
        }

        return 0;
}

// Re-queue a single buffer back to the V4L2 capture driver via VIDIOC_QBUF.
static int camera_requeue_single_buffer(struct camera_device *camera, const struct camera_buffer *buffer, const struct v4l2_buffer *dequeued_buf)
{
        struct v4l2_buffer buf;

        if (camera == NULL || camera->fd < 0 || buffer == NULL || dequeued_buf == NULL)
        {
                fprintf(stderr, "%s Invalid arguments for re-queueing buffer.\n", ERROR_PREFIX);
                return -1;
        }

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = buffer->memory;
        buf.index = dequeued_buf->index;

        if (buffer->memory == V4L2_MEMORY_USERPTR)
        {
                buf.m.userptr = (unsigned long)(uintptr_t)buffer->start;
                buf.length = (__u32)buffer->length;
        }
        else if (buffer->memory == V4L2_MEMORY_DMABUF)
        {
                buf.m.fd = buffer->dmabuf_fd;
                buf.length = (__u32)buffer->length;
        }

        if (camera_ioctl(camera->fd, VIDIOC_QBUF, &buf) == -1)
        {
                perror("ERROR: VIDIOC_QBUF failed while re-queueing frame");
                return -1;
        }

        return 0;
}

// Write raw frame bytes to disk for inspection.
static int camera_write_frame_file(uint32_t frame_index, const void *data, size_t byte_count)
{
        char filename[CAMERA_FRAME_PATH_SIZE];
        FILE *output_file;
        size_t written;

        if (data == NULL || byte_count == 0)
        {
                fprintf(stderr, "%s Invalid frame buffer or zero byte count.\n", ERROR_PREFIX);
                return -1;
        }

        snprintf(filename, sizeof(filename), "%s_%04u.%s", CAMERA_FRAME_FILE_PREFIX, frame_index, CAMERA_FRAME_FILE_EXT);

        output_file = fopen(filename, "wb");
        if (output_file == NULL)
        {
                perror("ERROR: Failed to open output frame file");
                return -1;
        }

        written = fwrite(data, 1U, byte_count, output_file);
        if (written != byte_count)
        {
                perror("ERROR: Failed to write full frame data");
                fclose(output_file);
                return -1;
        }

        if (fclose(output_file) != 0)
        {
                perror("ERROR: Failed to close output frame file");
                return -1;
        }

        return 0;
}

// Extract frame payload safely according to buffer memory type and save to file.
static int camera_save_captured_frame(const struct camera_buffer *buffer, const struct v4l2_buffer *dequeued_buf, uint32_t frame_index)
{
        size_t frame_bytes;
        void *mapped_ptr = NULL;
        int write_status = -1;

        if (buffer == NULL || dequeued_buf == NULL || buffer->length == 0)
        {
                fprintf(stderr, "%s Invalid buffer metadata for captured frame.\n", ERROR_PREFIX);
                return -1;
        }

        frame_bytes = (dequeued_buf->bytesused > 0) ? (size_t)dequeued_buf->bytesused : buffer->length;
        if (frame_bytes > buffer->length)
        {
                frame_bytes = buffer->length;
        }

        if (buffer->memory == V4L2_MEMORY_MMAP)
        {
                if (buffer->start == NULL)
                {
                        fprintf(stderr, "%s NULL MMAP pointer for frame %u.\n", ERROR_PREFIX, frame_index);
                        return -1;
                }

                write_status = camera_write_frame_file(frame_index, buffer->start, frame_bytes);
        }
        else if (buffer->memory == V4L2_MEMORY_USERPTR)
        {
                void *user_ptr = (void *)(uintptr_t)dequeued_buf->m.userptr;
                if (user_ptr == NULL)
                {
                        user_ptr = buffer->start;
                }

                if (user_ptr == NULL)
                {
                        fprintf(stderr, "%s NULL USERPTR pointer for frame %u.\n", ERROR_PREFIX, frame_index);
                        return -1;
                }

                write_status = camera_write_frame_file(frame_index, user_ptr, frame_bytes);
        }
        else if (buffer->memory == V4L2_MEMORY_DMABUF)
        {
                if (buffer->dmabuf_fd < 0)
                {
                        fprintf(stderr, "%s Invalid DMABUF fd for frame %u.\n", ERROR_PREFIX, frame_index);
                        return -1;
                }

                mapped_ptr = mmap(NULL, buffer->length, PROT_READ, MAP_SHARED, buffer->dmabuf_fd, 0);
                if (mapped_ptr == MAP_FAILED)
                {
                        perror("ERROR: mmap failed for DMABUF frame read");
                        return -1;
                }

                write_status = camera_write_frame_file(frame_index, mapped_ptr, frame_bytes);

                if (munmap(mapped_ptr, buffer->length) == -1)
                {
                        perror("ERROR: munmap failed after DMABUF frame read");
                        return -1;
                }
        }
        else
        {
                fprintf(stderr, "%s Unsupported buffer memory type during capture.\n", ERROR_PREFIX);
                return -1;
        }

        return write_status;
}

// Capture requested number of frames using poll(), VIDIOC_DQBUF, and VIDIOC_QBUF.
int camera_capture_frames(struct camera_device *camera, const struct camera_capture_config *config, const struct camera_buffer_pool *pool)
{
        struct v4l2_buffer buf;
        uint32_t captured = 0;

        if (camera == NULL || camera->fd < 0 || config == NULL || pool == NULL || pool->buffers == NULL || pool->count == 0)
        {
                fprintf(stderr, "%s Invalid arguments for frame capture loop.\n", ERROR_PREFIX);
                return -1;
        }

        printf("\n========== FRAME CAPTURE LOOP ==========\n");
        printf("%s Capturing %u frames...\n", STATUS_PREFIX, config->frame_count);

        while (captured < config->frame_count)
        {
                if (camera_wait_for_frame(camera) == -1)
                {
                        return -1;
                }

                memset(&buf, 0, sizeof(buf));
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = pool->buffers[0].memory;

                if (buf.memory == V4L2_MEMORY_USERPTR || buf.memory == V4L2_MEMORY_DMABUF)
                {
                        buf.length = (__u32)pool->buffers[0].length;
                }

                if (camera_ioctl(camera->fd, VIDIOC_DQBUF, &buf) == -1)
                {
                        perror("ERROR: VIDIOC_DQBUF failed");
                        return -1;
                }

                if (buf.index >= pool->count)
                {
                        fprintf(stderr, "%s Dequeued buffer index %u out of range (%u).\n", ERROR_PREFIX, buf.index, pool->count);
                        return -1;
                }

                if (camera_save_captured_frame(&pool->buffers[buf.index], &buf, captured) == -1)
                {
                        return -1;
                }

                captured++;
                printf("%s Captured frame %u / %u (buffer index %u, %u bytes).\n", STATUS_PREFIX, captured, config->frame_count, buf.index, buf.bytesused);

                if (camera_requeue_single_buffer(camera, &pool->buffers[buf.index], &buf) == -1)
                {
                        return -1;
                }
        }

        printf("%s Frame capture completed (%u frames captured).\n", STATUS_PREFIX, captured);
        return 0;
}

// Close open camera descriptor and reset device struct fields.
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
