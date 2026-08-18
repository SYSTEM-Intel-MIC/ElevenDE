/*
 * elevende-screenshot - full-screen screenshot for ElevenDE (PrintScreen).
 *
 * Grabs the root window with XGetImage and writes a PNG via libpng to
 * ~/Pictures/Screenshots/. Then raises a desktop notification through
 * notify-send when available (handled by elevende-notifyd).
 *
 * Usage:  elevende-screenshot [--quiet]
 */

#include <libpng/png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

static int write_png(const char *path, XImage *img)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return 0;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if (!png || !info) {
        fclose(fp);
        return 0;
    }
    if (setjmp(png_jmpbuf(png))) {
        fclose(fp);
        return 0;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, img->width, img->height, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    unsigned char *row = malloc((size_t)img->width * 3);
    if (!row) {
        fclose(fp);
        return 0;
    }
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long px = XGetPixel(img, x, y);
            row[x * 3 + 0] = (px >> 16) & 0xFF;
            row[x * 3 + 1] = (px >> 8) & 0xFF;
            row[x * 3 + 2] = px & 0xFF;
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 1;
}

int main(int argc, char **argv)
{
    int quiet = argc > 1 && strcmp(argv[1], "--quiet") == 0;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "elevende-screenshot: cannot open X display\n");
        return 1;
    }
    const int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    XImage *img = XGetImage(dpy, root, 0, 0,
                            (unsigned)DisplayWidth(dpy, scr),
                            (unsigned)DisplayHeight(dpy, scr),
                            AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "elevende-screenshot: XGetImage failed\n");
        XCloseDisplay(dpy);
        return 1;
    }

    /* target dir */
    const char *home = getenv("HOME");
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/Pictures/Screenshots", home ? home : "/tmp");
    mkdir(dir, 0755);   /* mkdir -p equivalent for the two levels below */
    char pics[1024];
    snprintf(pics, sizeof pics, "%s/Pictures", home ? home : "/tmp");
    mkdir(pics, 0755);
    mkdir(dir, 0755);

    char path[1280];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tmv);
    snprintf(path, sizeof path, "%s/ElevenDE_%s.png", dir, stamp);

    const int ok = write_png(path, img);
    XDestroyImage(img);
    XCloseDisplay(dpy);

    if (!ok) {
        fprintf(stderr, "elevende-screenshot: failed to write %s\n", path);
        return 1;
    }
    printf("%s\n", path);

    if (!quiet) {
        char body[1400];
        snprintf(body, sizeof body, "已保存到 %s", path);
        pid_t pid = fork();
        if (pid == 0) {
            execlp("notify-send", "notify-send", "-a", "ElevenDE",
                   "-i", "camera-photo", "截图完成", body, (char *)NULL);
            _exit(127);
        }
    }
    return 0;
}
