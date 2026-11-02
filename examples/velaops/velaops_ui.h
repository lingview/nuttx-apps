/****************************************************************************
 * apps/examples/velaops/velaops_ui.h
 *
 * VelaOps 320x240 landscape dashboard renderer.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef VELAOPS_UI_H
#define VELAOPS_UI_H

#include <stdint.h>

#define VELAOPS_UI_PAGES 6

enum velaops_ui_page
{
  VELAOPS_PAGE_OVERVIEW = 0,
  VELAOPS_PAGE_PERFORMANCE,
  VELAOPS_PAGE_OPS,
  VELAOPS_PAGE_SUMMARY,
  VELAOPS_PAGE_ROOTCAUSE,
  VELAOPS_PAGE_ACTION,
};

typedef struct
{
  const char *host;
  int online;

  float cpu;                    /* percent, <0 = unknown */
  float mem;
  float disk;
  float load1;

  float cpu_hist[48];
  int cpu_hist_len;

  int services_total;
  int services_ok;
  int ports_total;
  int ports_ok;
  int alerts;

  const char *summary;          /* ASCII, wrapped by renderer */
  const char *root_cause;
  const char *action_state;
  const char *action_service;
  const char *action_result;

  const char *clock;            /* "12:34:56" */
  const char *uptime;           /* "UP 3D 02:03" */
} velaops_ui_state_t;

int velaops_ui_render(uint16_t *fb, int width, int height,
                      const velaops_ui_state_t *state, int page);

int velaops_ui_render_alert(uint16_t *fb, int width, int height,
                            const char *title, const char *text,
                            int blink_phase);

int velaops_ui_render_prov(uint16_t *fb, int width, int height,
                           const char *ssid, const char *pass,
                           const char *url, const char *pin);

typedef struct
{
  int led_on;
  int buzzer_on;
  int key_down[4];
  int key_count[4];
} velaops_hw_state_t;

int velaops_ui_render_hwtest(uint16_t *fb, int width, int height,
                             const velaops_hw_state_t *state);

#endif /* VELAOPS_UI_H */
