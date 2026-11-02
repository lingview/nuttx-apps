/****************************************************************************
 * apps/examples/velaops/velaops_ui.c
 *
 * VelaOps 320x240 landscape dashboard renderer.
 * ASCII uses the built-in 5x7 font, Chinese labels use a 16x16 subset
 * generated into velaops_cn_font.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "velaops_ui.h"
#include "velaops_cn_font.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FONT_WIDTH  5
#define FONT_HEIGHT 7
#define ADVANCE     6
#define CJK_SIZE    16

#define COL_BG      0x0000
#define COL_PANEL   0x18e3
#define COL_BORDER  0x4208
#define COL_TEXT    0xffff
#define COL_DIM     0x8410
#define COL_GREEN   0x07e0
#define COL_AMBER   0xfd20
#define COL_RED     0xf800
#define COL_CYAN    0x07ff

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void fill_rect(uint16_t *fb, int w, int h,
                      int x, int y, int width, int height, uint16_t color)
{
  int px;
  int py;

  for (py = y; py < y + height; py++)
    {
      if (py < 0 || py >= h)
        {
          continue;
        }

      for (px = x; px < x + width; px++)
        {
          if (px >= 0 && px < w)
            {
              fb[py * w + px] = color;
            }
        }
    }
}

static void glyph_for(char c, uint8_t glyph[FONT_WIDTH])
{
  static const uint8_t letters[][FONT_WIDTH] =
  {
    {0x1e, 0x05, 0x05, 0x1e, 0}, {0x1f, 0x15, 0x15, 0x0a, 0},
    {0x0e, 0x11, 0x11, 0x0a, 0}, {0x1f, 0x11, 0x11, 0x0e, 0},
    {0x1f, 0x15, 0x15, 0x11, 0}, {0x1f, 0x05, 0x05, 0x01, 0},
    {0x0e, 0x11, 0x15, 0x1d, 0}, {0x1f, 0x04, 0x04, 0x1f, 0},
    {0x11, 0x1f, 0x11, 0, 0},    {0x08, 0x10, 0x10, 0x0f, 0},
    {0x1f, 0x04, 0x0a, 0x11, 0}, {0x1f, 0x10, 0x10, 0x10, 0},
    {0x1f, 0x02, 0x04, 0x02, 0x1f}, {0x1f, 0x02, 0x04, 0x1f, 0},
    {0x0e, 0x11, 0x11, 0x0e, 0}, {0x1f, 0x05, 0x05, 0x02, 0},
    {0x0e, 0x11, 0x19, 0x1e, 0}, {0x1f, 0x05, 0x0d, 0x12, 0},
    {0x12, 0x15, 0x15, 0x09, 0}, {0x01, 0x1f, 0x01, 0, 0},
    {0x0f, 0x10, 0x10, 0x0f, 0}, {0x07, 0x08, 0x10, 0x08, 0x07},
    {0x1f, 0x08, 0x04, 0x08, 0x1f}, {0x11, 0x0a, 0x04, 0x0a, 0x11},
    {0x03, 0x04, 0x18, 0x04, 0x03}, {0x19, 0x15, 0x13, 0x11, 0}
  };
  static const uint8_t digits[][FONT_WIDTH] =
  {
    {0x0e, 0x11, 0x11, 0x0e, 0}, {0x12, 0x1f, 0x10, 0, 0},
    {0x19, 0x15, 0x15, 0x12, 0}, {0x11, 0x15, 0x15, 0x0a, 0},
    {0x07, 0x04, 0x1f, 0x04, 0}, {0x17, 0x15, 0x15, 0x09, 0},
    {0x0e, 0x15, 0x15, 0x08, 0}, {0x01, 0x01, 0x1d, 0x03, 0},
    {0x0a, 0x15, 0x15, 0x0a, 0}, {0x02, 0x15, 0x15, 0x0e, 0}
  };

  memset(glyph, 0, FONT_WIDTH);

  if (c >= 'a' && c <= 'z')
    {
      c = (char)(c - 'a' + 'A');
    }

  if (c >= 'A' && c <= 'Z')
    {
      memcpy(glyph, letters[c - 'A'], FONT_WIDTH);
    }
  else if (c >= '0' && c <= '9')
    {
      memcpy(glyph, digits[c - '0'], FONT_WIDTH);
    }
  else if (c == '%')
    {
      const uint8_t v[] = {0x23, 0x13, 0x08, 0x64, 0x62};
      memcpy(glyph, v, sizeof(v));
    }
  else if (c == ':')
    {
      glyph[1] = 0x0a;
      glyph[3] = 0x0a;
    }
  else if (c == '.')
    {
      glyph[1] = 0x10;
    }
  else if (c == '-')
    {
      glyph[1] = 0x04;
      glyph[2] = 0x04;
    }
  else if (c == '/')
    {
      glyph[0] = 0x10;
      glyph[1] = 0x08;
      glyph[2] = 0x04;
      glyph[3] = 0x02;
      glyph[4] = 0x01;
    }
  else if (c == '=')
    {
      glyph[1] = 0x0a;
      glyph[3] = 0x0a;
    }
  else if (c == '!')
    {
      glyph[1] = 0x17;
      glyph[3] = 0x11;
    }
  else if (c == '?')
    {
      glyph[0] = 0x19;
      glyph[1] = 0x15;
      glyph[2] = 0x15;
      glyph[3] = 0x12;
    }
}

static uint32_t utf8_next(const char **pp)
{
  const unsigned char *p = (const unsigned char *)*pp;
  uint32_t cp;

  if (*p < 0x80)
    {
      cp = *p;
      *pp += 1;
    }
  else if ((*p & 0xe0) == 0xc0 && p[1] != 0)
    {
      cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
      *pp += 2;
    }
  else if ((*p & 0xf0) == 0xe0 && p[1] != 0 && p[2] != 0)
    {
      cp = ((uint32_t)(p[0] & 0x0f) << 12) |
           ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
      *pp += 3;
    }
  else
    {
      cp = '?';
      *pp += 1;
    }

  return cp;
}

static const uint8_t *cjk_glyph(uint32_t cp)
{
  int lo = 0;
  int hi = (int)(sizeof(g_cjk_font) / sizeof(g_cjk_font[0])) - 1;

  while (lo <= hi)
    {
      int mid = (lo + hi) / 2;

      if (g_cjk_font[mid].codepoint == cp)
        {
          return g_cjk_font[mid].bitmap;
        }

      if (g_cjk_font[mid].codepoint < cp)
        {
          lo = mid + 1;
        }
      else
        {
          hi = mid - 1;
        }
    }

  return NULL;
}

static void draw_cjk(uint16_t *fb, int w, int h, int x, int y,
                     const uint8_t *bmp, uint16_t color)
{
  int gx;
  int gy;

  for (gy = 0; gy < CJK_SIZE; gy++)
    {
      for (gx = 0; gx < CJK_SIZE; gx++)
        {
          if ((bmp[gy * 2 + gx / 8] & (0x80 >> (gx % 8))) != 0)
            {
              fill_rect(fb, w, h, x + gx, y + gy, 1, 1, color);
            }
        }
    }
}

static void draw_text(uint16_t *fb, int w, int h, int x, int y,
                      const char *text, int scale, uint16_t color)
{
  uint8_t glyph[FONT_WIDTH];
  const char *p = text;
  int cx = x;

  while (*p != '\0')
    {
      uint32_t cp = utf8_next(&p);

      if (cp < 0x80)
        {
          /* Center the 5x7 cell inside a 16px CJK row: the ASCII ink is
           * 10px tall at 2x and would otherwise sit on the very top. */

          int dy = (scale == 2) ? 3 : 0;
          int gx;
          int gy;

          glyph_for((char)cp, glyph);
          for (gx = 0; gx < FONT_WIDTH; gx++)
            {
              for (gy = 0; gy < FONT_HEIGHT; gy++)
                {
                  if ((glyph[gx] & (1U << gy)) != 0)
                    {
                      fill_rect(fb, w, h, cx + gx * scale,
                                y + dy + gy * scale, scale, scale, color);
                    }
                }
            }

          cx += ADVANCE * scale;
        }
      else
        {
          const uint8_t *bmp = cjk_glyph(cp);

          if (bmp != NULL)
            {
              draw_cjk(fb, w, h, cx, y, bmp, color);
            }
          else
            {
              draw_text(fb, w, h, cx, y, "?", scale, color);
            }

          cx += CJK_SIZE;
        }
    }
}

static int text_width(const char *text, int scale)
{
  const char *p = text;
  int width = 0;

  while (*p != '\0')
    {
      uint32_t cp = utf8_next(&p);
      width += (cp < 0x80) ? ADVANCE * scale : CJK_SIZE;
    }

  return width;
}

static void draw_text_right(uint16_t *fb, int w, int h, int right, int y,
                            const char *text, int scale, uint16_t color)
{
  draw_text(fb, w, h, right - text_width(text, scale), y, text, scale,
            color);
}

static void draw_text_center(uint16_t *fb, int w, int h, int cx, int y,
                             const char *text, int scale, uint16_t color)
{
  draw_text(fb, w, h, cx - text_width(text, scale) / 2, y, text, scale,
            color);
}

static void draw_bar(uint16_t *fb, int w, int h, int x, int y,
                     int width, float percent, uint16_t color)
{
  int filled;

  if (percent < 0.0f)
    {
      percent = 0.0f;
    }

  if (percent > 100.0f)
    {
      percent = 100.0f;
    }

  fill_rect(fb, w, h, x, y, width, 8, COL_BORDER);
  filled = (int)((float)(width - 2) * percent / 100.0f);
  fill_rect(fb, w, h, x + 1, y + 1, filled, 6, color);
}

static uint16_t level_color(float percent)
{
  if (percent >= 90.0f)
    {
      return COL_RED;
    }

  if (percent >= 75.0f)
    {
      return COL_AMBER;
    }

  return COL_GREEN;
}

#define TEXT_LINE_MAX 80

static int wrap_text(const char *text, char lines[][TEXT_LINE_MAX],
                     int max_lines, int max_width)
{
  int count = 0;
  const char *p = text;

  while (*p != '\0' && count < max_lines)
    {
      const char *start = p;
      const char *last_space = NULL;
      int width = 0;
      int len;

      while (*p != '\0' && *p != '\n')
        {
          const char *save = p;
          uint32_t cp = utf8_next(&p);
          int cw = (cp < 0x80) ? ADVANCE * 2 : CJK_SIZE;

          if (width + cw > max_width)
            {
              p = save;
              break;
            }

          width += cw;

          if (cp == ' ')
            {
              last_space = save;
            }

          if ((size_t)(p - start) >= TEXT_LINE_MAX - 4)
            {
              break;
            }
        }

      if (*p != '\0' && *p != '\n' && last_space != NULL &&
          last_space > start)
        {
          p = last_space;
        }

      len = (int)(p - start);

      while (len > 0 && start[len - 1] == ' ')
        {
          len--;
        }

      memcpy(lines[count], start, len);
      lines[count][len] = '\0';
      count++;

      if (*p == ' ' || *p == '\n')
        {
          p++;
        }
    }

  return count;
}

static const char *overall_word(const velaops_ui_state_t *s, uint16_t *color)
{
  if (!s->online)
    {
      *color = COL_RED;
      return "离线";
    }

  if (s->cpu < 0.0f)
    {
      *color = COL_AMBER;
      return "无数据";
    }

  if (s->cpu >= 90.0f || s->mem >= 90.0f || s->disk >= 92.0f ||
      (s->services_total > 0 && s->services_ok < s->services_total) ||
      (s->ports_total > 0 && s->ports_ok < s->ports_total))
    {
      *color = COL_RED;
      return "严重";
    }

  if (s->cpu >= 75.0f || s->mem >= 75.0f || s->disk >= 80.0f)
    {
      *color = COL_AMBER;
      return "警告";
    }

  *color = COL_GREEN;
  return "正常";
}

static void draw_header(uint16_t *fb, int w, int h,
                        const velaops_ui_state_t *s, int page)
{
  uint16_t color;
  const char *word = overall_word(s, &color);
  char buf[24];

  fill_rect(fb, w, h, 0, 0, w, 28, COL_PANEL);
  fill_rect(fb, w, h, 0, 28, w, 1, COL_BORDER);

  draw_text(fb, w, h, 8, 6, "维拉哨兵", 1, COL_GREEN);

  fill_rect(fb, w, h, 84, 12, 8, 8, color);
  draw_text(fb, w, h, 96, 6, word, 2, color);

  snprintf(buf, sizeof(buf), "第%d/6页", page + 1);
  draw_text_right(fb, w, h, w - 8, 6, buf, 2, COL_TEXT);
}

static void draw_footer(uint16_t *fb, int w, int h,
                        const velaops_ui_state_t *s)
{
  fill_rect(fb, w, h, 0, h - 20, w, 1, COL_BORDER);
  draw_text(fb, w, h, 8, h - 18, "短按BOOT翻页", 2, COL_DIM);
  draw_text_right(fb, w, h, w - 8, h - 18,
                  s->clock != NULL ? s->clock : "", 2, COL_DIM);
}

static void draw_metric_card(uint16_t *fb, int w, int h, int x, int y,
                             const char *label, float value)
{
  char buf[12];
  uint16_t color = value < 0.0f ? COL_DIM : level_color(value);

  fill_rect(fb, w, h, x, y, 96, 78, COL_PANEL);
  fill_rect(fb, w, h, x, y, 96, 1, COL_BORDER);
  fill_rect(fb, w, h, x, y + 77, 96, 1, COL_BORDER);

  draw_text(fb, w, h, x + 8, y + 8, label, 1, COL_DIM);

  if (value < 0.0f)
    {
      snprintf(buf, sizeof(buf), "--");
      draw_text_center(fb, w, h, x + 48, y + 30, buf, 3, COL_DIM);
    }
  else
    {
      snprintf(buf, sizeof(buf), "%d%%", (int)(value + 0.5f));
      draw_text_center(fb, w, h, x + 48, y + 30, buf, 3, color);
      draw_bar(fb, w, h, x + 10, y + 62, 76, value, color);
    }
}

static void draw_overview(uint16_t *fb, int w, int h,
                          const velaops_ui_state_t *s)
{
  char buf[64];

  draw_text(fb, w, h, 10, 36, "主机", 2, COL_DIM);
  draw_text(fb, w, h, 46, 36, s->host != NULL ? s->host : "-", 2, COL_TEXT);
  draw_text_right(fb, w, h, w - 10, 36,
                  s->online ? "在线" : "离线", 2,
                  s->online ? COL_GREEN : COL_RED);

  draw_metric_card(fb, w, h, 8, 58, "CPU", s->cpu);
  draw_metric_card(fb, w, h, 112, 58, "MEM", s->mem);
  draw_metric_card(fb, w, h, 216, 58, "DISK", s->disk);

  snprintf(buf, sizeof(buf), "服务 %d/%d 正常", s->services_ok,
           s->services_total);
  draw_text(fb, w, h, 10, 150, buf, 2,
            (s->services_total > 0 && s->services_ok == s->services_total)
            ? COL_GREEN : COL_AMBER);

  snprintf(buf, sizeof(buf), "端口 %d/%d 可达", s->ports_ok, s->ports_total);
  draw_text_right(fb, w, h, w - 10, 150, buf, 2,
                  (s->ports_total > 0 && s->ports_ok == s->ports_total)
                  ? COL_GREEN : COL_AMBER);

  fill_rect(fb, w, h, 8, 172, w - 16, 1, COL_BORDER);

  snprintf(buf, sizeof(buf), "告警 %d", s->alerts);
  draw_text(fb, w, h, 10, 178, buf, 2, s->alerts > 0 ? COL_RED : COL_DIM);

  if (s->uptime != NULL)
    {
      draw_text(fb, w, h, 10, 203, s->uptime, 2, COL_DIM);
    }
}

static void draw_sparkline(uint16_t *fb, int w, int h, int x, int y,
                           int width, int height,
                           const float *values, int count)
{
  int i;
  int last_x = -1;
  int last_y = -1;

  fill_rect(fb, w, h, x, y, width, height, COL_PANEL);
  fill_rect(fb, w, h, x, y + height / 2, width, 1, COL_BORDER);

  for (i = 0; i < count && i < width; i++)
    {
      int vx = x + i * width / (count > 1 ? count - 1 : 1);
      int vy = y + height - 1 -
               (int)(values[i] * (height - 1) / 100.0f);
      int px;
      int py;
      int y0;
      int y1;

      if (vy < y)
        {
          vy = y;
        }

      if (last_x >= 0)
        {
          y0 = last_y < vy ? last_y : vy;
          y1 = last_y < vy ? vy : last_y;

          for (py = y0; py <= y1; py++)
            {
              fill_rect(fb, w, h, vx, py, 1, 1, COL_CYAN);
            }
        }

      for (px = (last_x >= 0 ? last_x : vx); px <= vx; px++)
        {
          fill_rect(fb, w, h, px, vy, 1, 1, COL_CYAN);
        }

      last_x = vx;
      last_y = vy;
    }
}

static void draw_performance(uint16_t *fb, int w, int h,
                             const velaops_ui_state_t *s)
{
  char buf[32];

  draw_text(fb, w, h, 10, 36, "CPU 历史", 2, COL_DIM);
  draw_sparkline(fb, w, h, 10, 56, w - 20, 78,
                 s->cpu_hist, s->cpu_hist_len);

  draw_text(fb, w, h, 10, 136, "内存", 2, COL_DIM);
  if (s->mem >= 0.0f)
    {
      snprintf(buf, sizeof(buf), "%d%%", (int)(s->mem + 0.5f));
      draw_text_right(fb, w, h, w - 10, 136, buf, 2, level_color(s->mem));
    }

  draw_bar(fb, w, h, 10, 158, w - 20, s->mem, level_color(s->mem));

  draw_text(fb, w, h, 10, 172, "磁盘", 2, COL_DIM);
  if (s->disk >= 0.0f)
    {
      snprintf(buf, sizeof(buf), "%d%%", (int)(s->disk + 0.5f));
      draw_text_right(fb, w, h, w - 10, 172, buf, 2, level_color(s->disk));
    }

  draw_bar(fb, w, h, 10, 192, w - 20, s->disk, level_color(s->disk));

  if (s->load1 >= 0.0f)
    {
      snprintf(buf, sizeof(buf), "负载 %.2f", (double)s->load1);
      draw_text(fb, w, h, 10, 206, buf, 1, COL_DIM);
    }
}

static void draw_ops(uint16_t *fb, int w, int h,
                     const velaops_ui_state_t *s)
{
  char buf[48];
  int i;
  int y = 52;

  draw_text(fb, w, h, 10, 36, "服务", 2, COL_DIM);
  draw_text_right(fb, w, h, w - 10, 36, "状态", 2, COL_DIM);
  fill_rect(fb, w, h, 8, 50, w - 16, 1, COL_BORDER);

  for (i = 0; i < s->services_total && i < 6; i++)
    {
      int ok = i < s->services_ok;

      snprintf(buf, sizeof(buf), "服务-%d", i + 1);
      draw_text(fb, w, h, 12, y, buf, 2, COL_TEXT);
      draw_text_right(fb, w, h, w - 12, y, ok ? "运行中" : "失败", 2,
                      ok ? COL_GREEN : COL_RED);
      y += 20;
    }

  y += 6;
  fill_rect(fb, w, h, 8, y, w - 16, 1, COL_BORDER);
  y += 8;

  snprintf(buf, sizeof(buf), "端口可达 %d/%d", s->ports_ok, s->ports_total);
  draw_text(fb, w, h, 10, y, buf, 2,
            (s->ports_total > 0 && s->ports_ok == s->ports_total)
            ? COL_GREEN : COL_AMBER);
}

static void draw_text_page(uint16_t *fb, int w, int h, const char *title,
                           const char *body, uint16_t title_color)
{
  char lines[16][TEXT_LINE_MAX];
  int count;
  int i;

  draw_text(fb, w, h, 10, 36, title, 2, title_color);
  fill_rect(fb, w, h, 8, 56, w - 16, 1, COL_BORDER);

  count = wrap_text(body != NULL ? body : "-", lines, 9, w - 20);
  for (i = 0; i < count; i++)
    {
      draw_text(fb, w, h, 10, 64 + i * 18, lines[i], 2, COL_TEXT);
    }
}

static void draw_summary(uint16_t *fb, int w, int h,
                         const velaops_ui_state_t *s)
{
  char buf[64];

  draw_text_page(fb, w, h, "诊断摘要", s->summary, COL_CYAN);

  snprintf(buf, sizeof(buf), "模型 MIMO  告警 %d", s->alerts);
  draw_text(fb, w, h, 10, 214, buf, 1, COL_DIM);
}

static void draw_rootcause(uint16_t *fb, int w, int h,
                           const velaops_ui_state_t *s)
{
  draw_text_page(fb, w, h, "根因", s->root_cause, COL_AMBER);
}

static void draw_action(uint16_t *fb, int w, int h,
                        const velaops_ui_state_t *s)
{
  int waiting = s->action_state != NULL &&
                strcmp(s->action_state, "WAITING") == 0;

  draw_text(fb, w, h, 10, 36, "处置", 2, COL_GREEN);
  fill_rect(fb, w, h, 8, 56, w - 16, 1, COL_BORDER);

  draw_text(fb, w, h, 10, 66, "状态", 2, COL_DIM);
  draw_text(fb, w, h, 60, 66, waiting ? "等待批准" : "空闲", 2,
            waiting ? COL_AMBER : COL_DIM);

  draw_text(fb, w, h, 10, 88, "目标", 2, COL_DIM);
  draw_text(fb, w, h, 60, 88,
            s->action_service != NULL ? s->action_service : "-", 2,
            COL_TEXT);

  draw_text(fb, w, h, 10, 110, "结果", 2, COL_DIM);
  draw_text(fb, w, h, 60, 110,
            s->action_result != NULL ? s->action_result : "-", 2,
            COL_TEXT);

  fill_rect(fb, w, h, 8, 132, w - 16, 1, COL_BORDER);

  draw_text(fb, w, h, 10, 140, "仅允许白名单服务", 2, COL_DIM);
  draw_text(fb, w, h, 10, 164, "需按住BOOT 2秒批准", 2, COL_DIM);
  draw_text(fb, w, h, 10, 188, "修复后重新SSH取证复核", 2, COL_DIM);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velaops_ui_render(uint16_t *fb, int width, int height,
                      const velaops_ui_state_t *s, int page)
{
  if (fb == NULL || s == NULL || width < 320 || height < 240)
    {
      return -1;
    }

  fill_rect(fb, width, height, 0, 0, width, height, COL_BG);
  draw_header(fb, width, height, s, page);

  switch (page % VELAOPS_UI_PAGES)
    {
      case VELAOPS_PAGE_OVERVIEW:
        draw_overview(fb, width, height, s);
        break;
      case VELAOPS_PAGE_PERFORMANCE:
        draw_performance(fb, width, height, s);
        break;
      case VELAOPS_PAGE_OPS:
        draw_ops(fb, width, height, s);
        break;
      case VELAOPS_PAGE_SUMMARY:
        draw_summary(fb, width, height, s);
        break;
      case VELAOPS_PAGE_ROOTCAUSE:
        draw_rootcause(fb, width, height, s);
        break;
      case VELAOPS_PAGE_ACTION:
      default:
        draw_action(fb, width, height, s);
        break;
    }

  draw_footer(fb, width, height, s);
  return 0;
}

int velaops_ui_render_alert(uint16_t *fb, int width, int height,
                            const char *title, const char *text,
                            int blink_phase)
{
  char lines[8][TEXT_LINE_MAX];
  int count;
  int i;
  int box_x = 12;
  int box_y = 62;
  int box_w = width - 24;
  int box_h = 128;
  uint16_t border = blink_phase ? COL_RED : COL_PANEL;

  if (fb == NULL || title == NULL || text == NULL)
    {
      return -1;
    }

  fill_rect(fb, width, height, 0, 0, width, height, COL_BG);

  fill_rect(fb, width, height, box_x, box_y, box_w, box_h, COL_PANEL);
  fill_rect(fb, width, height, box_x, box_y, box_w, 2, border);
  fill_rect(fb, width, height, box_x, box_y + box_h - 2, box_w, 2, border);
  fill_rect(fb, width, height, box_x, box_y, 2, box_h, border);
  fill_rect(fb, width, height, box_x + box_w - 2, box_y, 2, box_h, border);

  draw_text_center(fb, width, height, width / 2, box_y + 12, title, 2,
                   COL_RED);

  count = wrap_text(text, lines, 4, width - 86);
  for (i = 0; i < count; i++)
    {
      draw_text(fb, width, height, box_x + 14, box_y + 44 + i * 18,
                lines[i], 2, COL_TEXT);
    }

  draw_text_center(fb, width, height, width / 2, box_y + box_h - 22,
                   "按BOOT关闭", 2, COL_AMBER);
  return 0;
}

int velaops_ui_render_prov(uint16_t *fb, int width, int height,
                           const char *ssid, const char *pass,
                           const char *url, const char *pin)
{
  int y = 70;

  if (fb == NULL || ssid == NULL)
    {
      return -1;
    }

  fill_rect(fb, width, height, 0, 0, width, height, COL_BG);
  fill_rect(fb, width, height, 0, 0, width, 28, COL_PANEL);
  fill_rect(fb, width, height, 0, 28, width, 1, COL_BORDER);

  draw_text(fb, width, height, 8, 6, "维拉哨兵", 1, COL_GREEN);
  draw_text_right(fb, width, height, width - 8, 6, "配网", 2, COL_AMBER);

  draw_text_center(fb, width, height, width / 2, 42, "配网模式", 2,
                   COL_TEXT);

  draw_text(fb, width, height, 10, y, "热点", 2, COL_DIM);
  draw_text(fb, width, height, 70, y, ssid, 2, COL_CYAN);

  y += 26;
  draw_text(fb, width, height, 10, y, "密码", 2, COL_DIM);
  draw_text(fb, width, height, 70, y, pass, 2, COL_CYAN);

  y += 26;
  draw_text(fb, width, height, 10, y, "网址", 2, COL_DIM);
  draw_text(fb, width, height, 70, y, url, 2, COL_TEXT);

  y += 30;
  draw_text(fb, width, height, 10, y, "PIN", 2, COL_DIM);
  draw_text(fb, width, height, 70, y, pin, 3, COL_AMBER);

  fill_rect(fb, width, height, 0, height - 20, width, 1, COL_BORDER);
  draw_text_center(fb, width, height, width / 2, height - 16,
                   "保存后自动重启", 2, COL_DIM);
  return 0;
}
