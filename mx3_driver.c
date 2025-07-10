#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <dirent.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>

#define MOUSE_NAME "Logitech USB Receiver Mouse"
#define MOTION_THRESHOLD 50
#define TAP_TIMEOUT 0.2  // seconds
#define MAX_PATH_LEN 512 // Increased buffer size to prevent truncation

volatile sig_atomic_t keep_running = 1;

// Function prototypes
int open_mouse_device(void);
int setup_uinput_device(void);
void send_keys(int fd, const int keys[], int key_count);
double get_time_diff_seconds(struct timespec start, struct timespec end);

// Signal handler to enable clean shutdown
void signal_handler(int signal) {
    fprintf(stderr, "\nSignal %d received. Initiating graceful shutdown...\n", signal);
    keep_running = 0;
}

int main() {
    struct input_event ev;
    int mouse_fd, uinput_fd;
    bool btn_forward_pressed = false;
    bool motion_detected = false;
    int current_x = 0, current_y = 0;
    struct timespec btn_press_time, current_time;

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);

    // Open the mouse device
    mouse_fd = open_mouse_device();
    if (mouse_fd < 0) {
        return 1;
    }

    // Create virtual keyboard
    uinput_fd = setup_uinput_device();
    printf("Monitoring mouse events... Press Ctrl+C to stop.\n");

    // Switch to blocking mode for the main read loop
    int flags = fcntl(mouse_fd, F_GETFL, 0);
    fcntl(mouse_fd, F_SETFL, flags & ~O_NONBLOCK);

    // Main event loop
    while (keep_running) {
        ssize_t bytes_read = read(mouse_fd, &ev, sizeof(struct input_event));
        
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue; // Signal interrupted the read, try again
            }
            perror("Error reading from mouse device");
            break;
        }
        
        if (bytes_read != sizeof(struct input_event)) {
            continue;
        }

        if (ev.type == EV_KEY && ev.code == BTN_FORWARD) {
            if (ev.value == 1) {  // Button pressed
                btn_forward_pressed = true;
                motion_detected = false;
                current_x = 0;
                current_y = 0;
                clock_gettime(CLOCK_MONOTONIC, &btn_press_time);
                // printf("DEBUG: BTN_FORWARD pressed. Motion accumulators reset.\n");
            } else if (ev.value == 0) {  // Button released
                btn_forward_pressed = false;
                
                // Now apply actions ONLY on release, based on accumulated motion
                if (motion_detected) {
                    if (abs(current_x) > abs(current_y)) {
                        if (current_x > 0) {
                            // Mouse moved right
                            int keys[] = {KEY_LEFTMETA, KEY_LEFTBRACE};
                            send_keys(uinput_fd, keys, 2);
                        } else {
                            // Mouse moved left
                            int keys[] = {KEY_LEFTMETA, KEY_RIGHTBRACE};
                            send_keys(uinput_fd, keys, 2);
                        }
                    } else {
                        if (current_y > 0) {
                            // Mouse moved down
                        } else {
                            // Mouse moved up
                        }
                    }
                } else {
                    // No motion detected - just a tap
                    clock_gettime(CLOCK_MONOTONIC, &current_time);
                    double press_duration = get_time_diff_seconds(btn_press_time, current_time);
                    
                    if (press_duration < TAP_TIMEOUT) {
                        // printf("ACTION: BTN_FORWARD - Just a Tap! (Sending Left Meta Key)\n");
                        int keys[] = {KEY_LEFTMETA};
                        send_keys(uinput_fd, keys, 1);
                    } else {
                        // printf("ACTION: BTN_FORWARD - Long Press Detected (Duration: %.2fs, no gesture).\n", press_duration);
                    }
                }
                
                // Reset for next gesture
                current_x = 0;
                current_y = 0;
                motion_detected = false;
            }
        } else if (ev.type == EV_REL && btn_forward_pressed) {
            if (ev.code == REL_X) {
                current_x += ev.value;
                if (abs(current_x) > MOTION_THRESHOLD) {
                    motion_detected = true;
                }
            } else if (ev.code == REL_Y) {
                current_y += ev.value;
                if (abs(current_y) > MOTION_THRESHOLD) {
                    motion_detected = true;
                }
            }
        }
    }

    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        printf("Virtual keyboard device closed.\n");
    }
    
    printf("Script terminated.\n");
    return 0;
}

int open_mouse_device(void) {
    DIR *dir;
    struct dirent *entry;
    char device_path[MAX_PATH_LEN]; // Using increased buffer size
    int fd = -1;
    char name[256];

    dir = opendir("/dev/input");
    if (!dir) {
        perror("Cannot open /dev/input");
        return -1;
    }

    printf("Looking for mouse device: %s\n", MOUSE_NAME);

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) == 0) {
            snprintf(device_path, sizeof(device_path), "/dev/input/%s", entry->d_name);
            fd = open(device_path, O_RDONLY);
            
            if (fd < 0) {
                perror(device_path);
                continue;
            }
            
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
                printf("Checking device: %s (%s)\n", device_path, name);
                
                if (strstr(name, MOUSE_NAME) != NULL) {
                    printf("Found '%s' mouse device: %s\n", MOUSE_NAME, device_path);
                    break;
                }
            }
            
            close(fd);
            fd = -1;
        }
    }

    closedir(dir);

    if (fd < 0) {
        fprintf(stderr, "ERROR: '%s' not found. Please verify the exact device name. Exiting.\n", MOUSE_NAME);
        return -1;
    }
    
    return fd;
}

int setup_uinput_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Cannot open /dev/uinput");
        fprintf(stderr, "This might require the 'uinput' kernel module loaded and/or root privileges.\n");
        fprintf(stderr, "Try 'sudo modprobe uinput' and ensure your user is in the 'input' and 'uinput' groups.\n");
        return -1;
    }

    // Enable key events
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
        perror("Cannot set EV_KEY bit");
        close(fd);
        return -1;
    }

    // Enable specific keys
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTMETA);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHTBRACE);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTBRACE);
    ioctl(fd, UI_SET_KEYBIT, KEY_MUTE);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFTALT);
    ioctl(fd, UI_SET_KEYBIT, KEY_LEFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, KEY_F13);
    ioctl(fd, UI_SET_KEYBIT, KEY_F14);
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP);

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    usetup.id.version = 1;
    strcpy(usetup.name, "MouseGestureVirtualKeyboard");

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("Cannot setup uinput device");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("Cannot create uinput device");
        close(fd);
        return -1;
    }

    printf("Created virtual keyboard device for sending keypresses.\n");
    return fd;
}

// Simplified key sending function that handles any number of keys
void send_keys(int fd, const int keys[], int key_count) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    int i;

    // Press all keys in sequence
    for (i = 0; i < key_count; i++) {
        ev.type = EV_KEY;
        ev.code = keys[i];
        ev.value = 1; // Press
        write(fd, &ev, sizeof(ev));
    }

    // Sync after all key presses
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));

    // Small delay to ensure the key combination is registered
    usleep(10000);

    // Release all keys in reverse order
    for (i = key_count - 1; i >= 0; i--) {
        ev.type = EV_KEY;
        ev.code = keys[i];
        ev.value = 0; // Release
        write(fd, &ev, sizeof(ev));
    }

    // Final sync
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));
}

double get_time_diff_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
}
