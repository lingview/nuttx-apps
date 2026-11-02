/****************************************************************************
 * apps/examples/lcdtest/lcdtest_main.c
 *
 * Draw colour bars and orientation markers on /dev/lcd0 to validate the
 * LCD driver and panel wiring.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <nuttx/lcd/lcd_dev.h>

static const uint16_t g_bars[8] =
{
  0xffff, 0xffe0, 0x07ff, 0x07e0,
  0xf81f, 0xf800, 0x001f, 0x0000
};

static void fill_rect(uint16_t *fb, int w, int h,
                      int x0, int y0, int x1, int y1, uint16_t color)
{
  int x;
  int y;

  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 >= w) x1 = w - 1;
  if (y1 >= h) y1 = h - 1;

  for (y = y0; y <= y1; y++)
    {
      for (x = x0; x <= x1; x++)
        {
          fb[y * w + x] = color;
        }
    }
}

int main(int argc, FAR char *argv[])
{
  struct fb_videoinfo_s vi;
  struct lcddev_area_s area;
  uint16_t *fb;
  int fd;
  int x;
  int y;
  int i;

  fd = open("/dev/lcd0", O_RDWR);
  if (fd < 0)
    {
      printf("lcdtest: open /dev/lcd0 failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  memset(&vi, 0, sizeof(vi));
  if (ioctl(fd, LCDDEVIO_GETVIDEOINFO, (unsigned long)(uintptr_t)&vi) < 0)
    {
      printf("lcdtest: GETVIDEOINFO failed: %d\n", errno);
      close(fd);
      return EXIT_FAILURE;
    }

  printf("lcdtest: %ux%u fmt=%d nplanes=%d\n",
         vi.xres, vi.yres, vi.fmt, vi.nplanes);

  ioctl(fd, LCDDEVIO_SETPOWER, 1);

  fb = calloc((size_t)vi.xres * vi.yres, sizeof(uint16_t));
  if (fb == NULL)
    {
      printf("lcdtest: no memory for %u bytes\n",
             (unsigned)(vi.xres * vi.yres * 2));
      close(fd);
      return EXIT_FAILURE;
    }

  /* Vertical colour bars */

  for (i = 0; i < 8; i++)
    {
      fill_rect(fb, vi.xres, vi.yres,
                i * vi.xres / 8, 0,
                (i + 1) * vi.xres / 8 - 1, vi.yres - 1,
                g_bars[i]);
    }

  /* White border */

  fill_rect(fb, vi.xres, vi.yres, 0, 0, vi.xres - 1, 1, 0xffff);
  fill_rect(fb, vi.xres, vi.yres, 0, vi.yres - 2, vi.xres - 1,
            vi.yres - 1, 0xffff);

  /* Corner markers: white TL, black TR, black BL, white BR */

  fill_rect(fb, vi.xres, vi.yres, 4, 4, 19, 19, 0xffff);
  fill_rect(fb, vi.xres, vi.yres, vi.xres - 20, 4, vi.xres - 5, 19, 0x0000);
  fill_rect(fb, vi.xres, vi.yres, 4, vi.yres - 20, 19, vi.yres - 5, 0x0000);
  fill_rect(fb, vi.xres, vi.yres, vi.xres - 20, vi.yres - 20,
            vi.xres - 5, vi.yres - 5, 0xffff);

  /* Centre cross */

  for (x = vi.xres / 2 - 20; x <= vi.xres / 2 + 20; x++)
    {
      fb[(vi.yres / 2) * vi.xres + x] = 0xffff;
    }

  for (y = vi.yres / 2 - 20; y <= vi.yres / 2 + 20; y++)
    {
      fb[y * vi.xres + vi.xres / 2] = 0xffff;
    }

  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end   = vi.yres - 1;
  area.col_start = 0;
  area.col_end   = vi.xres - 1;
  area.stride    = vi.xres * sizeof(uint16_t);
  area.data      = (uint8_t *)fb;

  if (ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area) < 0)
    {
      printf("lcdtest: PUTAREA failed: %d\n", errno);
      free(fb);
      close(fd);
      return EXIT_FAILURE;
    }

  printf("lcdtest: OK\n");

  free(fb);
  close(fd);
  return EXIT_SUCCESS;
}
