/****************************************************************************
 * apps/examples/velaops/velaops_main.c
 *
 * VelaOps dashboard shell.
 *
 *   velaops page <0-5>
 *   velaops demo [secs]
 *   velaops alert [text]
 *   velaops collect <host> <port> <user> <pass> [services] [ports]
 *   velaops run <host> <port> <user> <pass> [services] [ports] [repair] [secs]
 *
 * services and ports are comma separated lists.
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
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/boardctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <nuttx/lcd/lcd_dev.h>
#include <nuttx/ioexpander/gpio.h>
#include <netutils/netlib.h>
#include <netutils/dhcpc.h>
#include <nuttx/wireless/wireless.h>

#include "velaops_ui.h"
#include "velaops_ssh.h"
#include "velaops_health.h"
#include "velaops_prov.h"
#include "velaops_llm.h"
#include "velaops_conf.h"

static int lcd_open(struct fb_videoinfo_s *vi, uint16_t **fb)
{
  int fd;

  fd = open("/dev/lcd0", O_RDWR);
  if (fd < 0)
    {
      printf("velaops: open /dev/lcd0 failed: %d\n", errno);
      return -1;
    }

  memset(vi, 0, sizeof(*vi));
  if (ioctl(fd, LCDDEVIO_GETVIDEOINFO,
            (unsigned long)(uintptr_t)vi) < 0)
    {
      printf("velaops: GETVIDEOINFO failed: %d\n", errno);
      close(fd);
      return -1;
    }

  ioctl(fd, LCDDEVIO_SETPOWER, 1);

  *fb = calloc((size_t)vi->xres * vi->yres, sizeof(uint16_t));
  if (*fb == NULL)
    {
      printf("velaops: no memory\n");
      close(fd);
      return -1;
    }

  return fd;
}

static int lcd_flush(int fd, const struct fb_videoinfo_s *vi,
                     uint16_t *fb)
{
  struct lcddev_area_s area;

  memset(&area, 0, sizeof(area));
  area.row_start = 0;
  area.row_end   = vi->yres - 1;
  area.col_start = 0;
  area.col_end   = vi->xres - 1;
  area.stride    = vi->xres * sizeof(uint16_t);
  area.data      = (uint8_t *)fb;

  if (ioctl(fd, LCDDEVIO_PUTAREA, (unsigned long)(uintptr_t)&area) < 0)
    {
      printf("velaops: PUTAREA failed: %d\n", errno);
      return -1;
    }

  return 0;
}

/****************************************************************************
 * Hardware self-test
 ****************************************************************************/

#define HWTEST_BOOT_DEV "/dev/gpio1"

/* Implemented in the esp32s3-eye board GPIO driver.  The keys, LED and
 * buzzer are driven directly: registering extra /dev/gpio devices breaks
 * the Wi-Fi data path on this board.
 */

extern int board_hwtest_gpio_init(void);
extern int board_hwtest_gpio_read(int id);
extern void board_hwtest_gpio_write(int id, int value);

static volatile int g_stop;

static void stop_handler(int signo)
{
  (void)signo;
  board_hwtest_gpio_write(1, 0);
  board_hwtest_gpio_write(0, 0);
  g_stop = 1;
}

static uint32_t hw_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void hw_fill(uint16_t *fb, int w, int h, uint16_t color)
{
  int i;

  for (i = 0; i < w * h; i++)
    {
      fb[i] = color;
    }
}

static int hwtest_run(void)
{
  static const uint16_t bars[5] =
  {
    0xf800, 0x07e0, 0x001f, 0xffff, 0x0000
  };

  struct fb_videoinfo_s vi;
  uint16_t *fb = NULL;
  velaops_hw_state_t st;
  bool level;
  uint32_t change_ms[4];
  uint32_t buzz_until = 0;
  uint32_t next_render = 0;
  uint32_t boot_since = 0;
  uint32_t start;
  uint32_t now;
  int boot = -1;
  int fd;
  int i;

  fd = lcd_open(&vi, &fb);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  g_stop = 0;
  signal(SIGTERM, stop_handler);
  signal(SIGINT, stop_handler);

  printf("hwtest: lcd %ux%u\n", vi.xres, vi.yres);

  for (i = 0; i < 5; i++)
    {
      hw_fill(fb, vi.xres, vi.yres, bars[i]);
      lcd_flush(fd, &vi, fb);
      usleep(350000);
    }

  if (board_hwtest_gpio_init() != OK)
    {
      printf("hwtest: gpio init failed\n");
      free(fb);
      close(fd);
      return EXIT_FAILURE;
    }

  boot = open(HWTEST_BOOT_DEV, O_RDONLY);
  printf("hwtest: gpio ready boot=%d\n", boot);

  for (i = 0; i < 4; i++)
    {
      printf("hwtest: key%d level=%d\n", i + 1,
             board_hwtest_gpio_read(i));
    }

  /* Startup beeps; the buzzer is active high. */

  board_hwtest_gpio_write(1, false);
  board_hwtest_gpio_write(1, true);
  usleep(120000);
  board_hwtest_gpio_write(1, false);
  usleep(100000);
  board_hwtest_gpio_write(1, true);
  usleep(120000);
  board_hwtest_gpio_write(1, false);

  memset(&st, 0, sizeof(st));
  start = hw_ms();

  for (i = 0; i < 4; i++)
    {
      change_ms[i] = start;
    }

  for (;;)
    {
      bool beep;

      if (g_stop)
        {
          break;
        }

      now = hw_ms();

      st.led_on = ((now - start) / 500) & 1;
      board_hwtest_gpio_write(0, st.led_on != 0);

      for (i = 0; i < 4; i++)
        {
          bool pressed = (board_hwtest_gpio_read(i) == 0);

          if (pressed != (st.key_down[i] != 0))
            {
              if (now - change_ms[i] >= 30)
                {
                  change_ms[i] = now;
                  st.key_down[i] = pressed;

                  if (pressed)
                    {
                      st.key_count[i]++;
                      printf("hwtest: key %d pressed (count %d)\n",
                             i + 1, st.key_count[i]);

                      buzz_until = now + 90;
                    }
                }
            }
          else
            {
              change_ms[i] = now;
            }
        }

      beep = (buzz_until != 0 && now < buzz_until);
      st.buzzer_on = beep ? 1 : 0;

      board_hwtest_gpio_write(1, beep ? true : false);

      if (buzz_until != 0 && now >= buzz_until)
        {
          buzz_until = 0;
        }

      if (boot >= 0)
        {
          level = true;
          ioctl(boot, GPIOC_READ, (unsigned long)(uintptr_t)&level);

          if (!level)
            {
              if (boot_since == 0)
                {
                  boot_since = now;
                }
              else if (now - boot_since >= 2000)
                {
                  break;
                }
            }
          else
            {
              boot_since = 0;
            }
        }

      if (now >= next_render)
        {
          velaops_ui_render_hwtest(fb, vi.xres, vi.yres, &st);
          lcd_flush(fd, &vi, fb);
          next_render = now + 100;
        }

      usleep(20000);
    }

  board_hwtest_gpio_write(1, false);
  board_hwtest_gpio_write(0, false);

  if (boot >= 0)
    {
      close(boot);
    }

  free(fb);
  close(fd);

  printf("hwtest: done\n");
  return EXIT_SUCCESS;
}

static void demo_state(velaops_ui_state_t *s)
{
  int i;

  memset(s, 0, sizeof(*s));
  s->host           = "192.168.100.220";
  s->online         = 1;
  s->cpu            = 96.0f;
  s->mem            = 61.0f;
  s->disk           = 44.0f;
  s->swap           = 8.0f;
  s->inode          = 21.0f;
  s->temp           = 52.0f;
  s->cores          = 8;
  s->load1          = 3.82f;
  s->services_total = 6;
  s->services_ok    = 5;
  s->ports_total    = 4;
  s->ports_ok       = 3;
  s->alerts         = 1;

  s->score          = 58;
  s->s_avail        = 100;
  s->s_res          = 72;
  s->s_svc          = 65;
  s->s_net          = 60;
  s->s_stab         = 100;
  s->health_critical = 0;

  s->svc_failed     = 1;
  s->svc_active_units = 134;
  strlcpy(s->svc_failed0, "demo-web.service", sizeof(s->svc_failed0));

  s->port_count     = 12;
  s->port_new       = 2;
  s->port_lost      = 1;
  s->estab          = 24;
  s->tw             = 7;
  s->nic_err        = 0;
  s->nic_drop       = 12;

  s->top_pid        = 2311;
  strlcpy(s->top_name, "cpu-burner", sizeof(s->top_name));
  s->top_cpu        = 96.0f;

  s->alert_reason   = "CPU 使用率 96%，TOP 进程 cpu-burner 占用 96%";
  s->summary =
    "CPU 使用率持续 96%，主要是 cpu-burner 进程占满全部核心。"
    "服务 demo-web 已失败，其余指标正常。建议先终止该进程并观察。";
  s->root_cause =
    "A RUNAWAY LOOP IN DEMO-WEB CONSUMED ALL CPU TIME, SO HEALTH CHECKS "
    "TIMED OUT AND THE SUPERVISOR RESTARTED THE UNIT. MEMORY AND DISK "
    "ARE NOT INVOLVED.";
  s->action_state   = "WAITING";
  s->action_service = "cpu-burner";
  s->action_result  = "HOLD BOOT 2S TO KILL";
  s->clock          = "12:34:56";
  s->uptime         = "UP 3D 02:03  SSH OK";

  for (i = 0; i < 40; i++)
    {
      s->cpu_hist[i] = 20.0f + (float)((i * 37) % 25);
    }

  for (i = 30; i < 40; i++)
    {
      s->cpu_hist[i] = 85.0f + (float)(i % 12);
    }

  s->cpu_hist_len = 40;
}

static char g_llm_text[384] = "正在采集，等待 AI 诊断";
static char g_root_cause[96];

static velaops_health_cfg_t g_hcfg;
static velaops_health_result_t g_hres;
static int g_hcfg_loaded;
static char g_base_ports[240];
static char g_ignore_ports[96];
static char g_ignore_units[160];
static char g_kill_allow[96] = "cpu-burner";

static float conf_float(const char *key, float def)
{
  char value[16];

  return velaops_conf_get(key, value, sizeof(value)) == 0 ?
         (float)atof(value) : def;
}

static void health_cfg_load(void)
{
  char value[8];

  velaops_health_defaults(&g_hcfg);

  g_hcfg.cpu_w = conf_float("thr_cpu", g_hcfg.cpu_w);
  g_hcfg.cpu_c = g_hcfg.cpu_w + 10.0f;
  g_hcfg.mem_w = conf_float("thr_mem", g_hcfg.mem_w);
  g_hcfg.mem_c = g_hcfg.mem_w + 7.0f;
  g_hcfg.disk_w = conf_float("thr_disk", g_hcfg.disk_w);
  g_hcfg.disk_c = g_hcfg.disk_w + 7.0f;
  g_hcfg.inode_w = conf_float("thr_inode", g_hcfg.inode_w);
  g_hcfg.inode_c = g_hcfg.inode_w + 7.0f;
  g_hcfg.temp_w = conf_float("thr_temp", g_hcfg.temp_w);
  g_hcfg.temp_c = g_hcfg.temp_w + 10.0f;

  if (velaops_conf_get("alert_score", value, sizeof(value)) == 0)
    {
      g_hcfg.alert_score = atoi(value);
    }

  if (velaops_conf_get("recover_score", value, sizeof(value)) == 0)
    {
      g_hcfg.recover_score = atoi(value);
    }

  if (velaops_conf_get("ignore_ports", g_ignore_ports,
                       sizeof(g_ignore_ports)) < 0)
    {
      g_ignore_ports[0] = '\0';
    }

  if (velaops_conf_get("ignore_units", g_ignore_units,
                       sizeof(g_ignore_units)) < 0)
    {
      g_ignore_units[0] = '\0';
    }

  if (velaops_conf_get("kill_allow", g_kill_allow,
                       sizeof(g_kill_allow)) < 0)
    {
      strlcpy(g_kill_allow, "cpu-burner", sizeof(g_kill_allow));
    }

  if (velaops_conf_get("base_ports", g_base_ports,
                       sizeof(g_base_ports)) < 0)
    {
      g_base_ports[0] = '\0';
    }

  g_hcfg_loaded = 1;
}

static void baseline_learn(const velaops_snapshot_t *snap)
{
  char csv[240];
  size_t used = 0;
  int i;

  for (i = 0; i < snap->port_count; i++)
    {
      int port = atoi(snap->ports[i]);
      size_t len = strlen(snap->ports[i]);

      if (port <= 0 || port >= 32768 || used + len + 2 >= sizeof(csv))
        {
          continue;
        }

      if (used > 0)
        {
          csv[used++] = ',';
        }

      memcpy(csv + used, snap->ports[i], len);
      used += len;
    }

  csv[used] = '\0';

  if (velaops_conf_set("base_ports", csv) == 0)
    {
      strlcpy(g_base_ports, csv, sizeof(g_base_ports));
      printf("velaops: baseline learned ports=%d%c", snap->port_count, 10);
    }
}

static void apply_snapshot(velaops_ui_state_t *s,
                           const velaops_snapshot_t *snap,
                           const char *host)
{
  int i;

  if (snap->online && g_base_ports[0] == '\0' && snap->port_count > 0)
    {
      baseline_learn(snap);
    }

  velaops_health_score(snap, &g_hcfg, g_base_ports, g_ignore_ports,
                       g_ignore_units, &g_hres);

  s->host           = host;
  s->online         = snap->online;
  s->cpu            = snap->cpu;
  s->mem            = snap->mem;
  s->disk           = snap->disk;
  s->load1          = snap->load1;
  s->swap           = snap->swap;
  s->inode          = snap->inode;
  s->temp           = snap->temp;
  s->cores          = snap->cores;
  s->zombies        = snap->zombies;

  s->services_total = snap->watch_total;
  s->services_ok    = snap->watch_ok;
  s->ports_total    = snap->ports_watch_total;
  s->ports_ok       = snap->ports_watch_ok;

  s->score          = g_hres.total;
  s->s_avail        = g_hres.s_avail;
  s->s_res          = g_hres.s_res;
  s->s_svc          = g_hres.s_svc;
  s->s_net          = g_hres.s_net;
  s->s_stab         = g_hres.s_stab;
  s->health_critical = g_hres.critical;

  s->svc_failed     = snap->svc_failed;
  s->svc_active_units = snap->svc_active;
  s->svc_failed0[0] = '\0';
  s->svc_failed1[0] = '\0';
  s->svc_failed2[0] = '\0';

  for (i = 0; i < snap->svc_failed && i < 3; i++)
    {
      char *dst = i == 0 ? s->svc_failed0 :
                  (i == 1 ? s->svc_failed1 : s->svc_failed2);

      strlcpy(dst, snap->failed[i], 24);
    }

  s->port_count     = snap->port_count;
  s->port_new       = g_hres.port_new;
  s->port_lost      = g_hres.port_lost;
  s->estab          = snap->estab;
  s->tw             = snap->tw;
  s->nic_err        = snap->nic_err;
  s->nic_drop       = snap->nic_drop;

  s->top_pid        = snap->top_pid;
  s->top_cpu        = snap->top_cpu;
  strlcpy(s->top_name, snap->top_name, sizeof(s->top_name));

  s->alert_reason   = g_hres.reason;
  s->uptime         = snap->uptime;
  s->clock          = snap->clock;

  s->alerts = snap->online ? (g_hres.total < g_hcfg.alert_score) : 1;

  if (snap->cpu >= 0.0f)
    {
      if (s->cpu_hist_len >= 48)
        {
          memmove(&s->cpu_hist[0], &s->cpu_hist[1],
                  47 * sizeof(float));
          s->cpu_hist[47] = snap->cpu;
          s->cpu_hist_len = 48;
        }
      else
        {
          s->cpu_hist[s->cpu_hist_len++] = snap->cpu;
        }
    }

  if (!snap->online)
    {
      s->summary = NULL;
      s->root_cause = NULL;
      return;
    }

  if (snap->top_name[0] != '\0' && snap->top_cpu >= 20.0f)
    {
      snprintf(g_root_cause, sizeof(g_root_cause),
               "健康分 %d：%s；TOP 进程 %s 占用 %.0f%%",
               g_hres.total, g_hres.reason, snap->top_name,
               (double)snap->top_cpu);
    }
  else
    {
      snprintf(g_root_cause, sizeof(g_root_cause),
               "健康分 %d：%s", g_hres.total, g_hres.reason);
    }

  s->root_cause = g_root_cause;
  s->summary = g_llm_text;
}

static int kill_allowed(void)
{
  return g_hres.kill_suggest &&
         velaops_health_csv_has(g_kill_allow, g_hres.kill_name);
}

static int csv_find_index(const char *csv, const char *item, size_t len)
{
  const char *p = csv;
  int index = 0;

  while (p != NULL && *p != '\0')
    {
      const char *comma = strchr(p, ',');
      size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);

      if (n == len && strncmp(p, item, len) == 0)
        {
          return index;
        }

      index++;
      p = comma != NULL ? comma + 1 : NULL;
    }

  return -1;
}

/* Pick the first monitored service that is down AND whitelisted. */

static int pick_repair_target(const char *svcs, const char *repair,
                              const velaops_snapshot_t *snap,
                              char *out, size_t size)
{
  const char *p = repair;

  out[0] = '\0';
  if (repair == NULL || *repair == '\0')
    {
      return 0;
    }

  while (p != NULL && *p != '\0')
    {
      const char *comma = strchr(p, ',');
      size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);
      int index = csv_find_index(svcs, p, n);

      if (index >= 0 && index < snap->watch_count &&
          !snap->watch_active[index])
        {
          if (n >= size)
            {
              n = size - 1;
            }

          memcpy(out, p, n);
          out[n] = '\0';
          return 1;
        }

      p = comma != NULL ? comma + 1 : NULL;
    }

  return 0;
}

static int dhcp_lease(const char *ifname)
{
  struct dhcpc_state ds;
  void *handle;
  uint8_t mac[6];

  if (netlib_getmacaddr(ifname, mac) < 0)
    {
      printf("velaops: get mac failed%c", 10);
      return -1;
    }

  handle = dhcpc_open(ifname, mac, 6);
  if (handle == NULL)
    {
      printf("velaops: dhcpc_open failed%c", 10);
      return -1;
    }

  memset(&ds, 0, sizeof(ds));

  if (dhcpc_request(handle, &ds) < 0)
    {
      printf("velaops: dhcp request failed%c", 10);
      dhcpc_close(handle);
      return -1;
    }

  netlib_set_ipv4addr(ifname, &ds.ipaddr);

  if (ds.netmask.s_addr != 0)
    {
      netlib_set_ipv4netmask(ifname, &ds.netmask);
    }

  if (ds.default_router.s_addr != 0)
    {
      netlib_set_dripv4addr(ifname, &ds.default_router);
    }

  if (ds.dnsaddr.s_addr != 0)
    {
      netlib_set_ipv4dnsaddr(&ds.dnsaddr);
    }

  dhcpc_close(handle);
  printf("velaops: dhcp lease %s%c", inet_ntoa(ds.ipaddr), 10);
  return 0;
}

static int sta_associated(const char *ifname);

static void sta_connect(const char *ssid, const char *password)

{
  char cmd[192];
  int attempt;
  int rc;
  int i;

  if (ssid == NULL || ssid[0] == '\0')
    {
      printf("velaops: no wifi ssid in config, staying offline%c", 10);
      return;
    }

  for (attempt = 0; attempt < 3; attempt++)
    {
      if (attempt == 0)
        {
          rc = system("ifdown wlan0");
          printf("velaops: ifdown rc=%d%c", rc, 10);
          rc = system("wapi mode wlan0 2");
          printf("velaops: mode rc=%d%c", rc, 10);
          rc = system("ifup wlan0");
          printf("velaops: ifup rc=%d%c", rc, 10);
          sleep(1);

          if (password != NULL && password[0] != '\0')
            {
              snprintf(cmd, sizeof(cmd), "wapi psk wlan0 %s 3", password);
              rc = system(cmd);
              printf("velaops: psk rc=%d%c", rc, 10);
            }
        }

      /* The SSID write is what triggers the association. */

      for (i = 0; i < 3; i++)
        {
          rc = netlib_setessid("wlan0", ssid);
          printf("velaops: essid rc=%d try %d.%d%c", rc, attempt + 1,
                 i + 1, 10);

          if (rc == 0)
            {
              break;
            }

          sleep(1);
        }

      for (i = 0; i < 10; i++)
        {
          if (sta_associated("wlan0"))
            {
              break;
            }

          sleep(1);
        }

      if (sta_associated("wlan0"))
        {
          printf("velaops: associated on try %d%c", attempt + 1, 10);
          break;
        }

      printf("velaops: association try %d failed%c", attempt + 1, 10);
    }

  sleep(3);
  printf("velaops: dhcp request%c", 10);
  dhcp_lease("wlan0");
}

static int sta_associated(const char *ifname)
{
  struct iwreq iwr;
  static const unsigned char none[6] =
  {
    0, 0, 0, 0, 0, 0
  };

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  int ok = 0;

  if (fd < 0)
    {
      return 0;
    }

  memset(&iwr, 0, sizeof(iwr));
  strncpy(iwr.ifr_name, ifname, IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIWAP, (unsigned long)(uintptr_t)&iwr) == 0 &&
      memcmp(iwr.u.ap_addr.sa_data, none, 6) != 0)
    {
      ok = 1;
    }

  close(fd);
  return ok;
}

#define VELAOPS_NET_FILE "/data/velaops.net"

static int static_from_config(void)
{
  char ip[32];
  char gw[32];
  char mask[32];
  struct in_addr a;
  struct in_addr m;

  ip[0] = 0;
  gw[0] = 0;
  mask[0] = 0;

  velaops_conf_get("static_ip", ip, sizeof(ip));
  velaops_conf_get("static_gw", gw, sizeof(gw));
  velaops_conf_get("static_mask", mask, sizeof(mask));

  printf("velaops: static cfg ip='%s' gw='%s' mask='%s'%c",
         ip, gw, mask, 10);

  if (ip[0] == 0)
    {
      return -1;
    }

  if (inet_pton(AF_INET, ip, &a) != 1)
    {
      printf("velaops: static ip parse failed%c", 10);
      return -1;
    }

  /* Routes only get installed for an interface that is already up. */

  netlib_ifup("wlan0");
  netlib_set_ipv4addr("wlan0", &a);

  memset(&m, 0, sizeof(m));
  if (inet_pton(AF_INET, mask, &m) == 1 && m.s_addr != 0)
    {
      netlib_set_ipv4netmask("wlan0", &m);
    }

  if (inet_pton(AF_INET, gw, &a) == 1 && a.s_addr != 0)
    {
      netlib_set_dripv4addr("wlan0", &a);
    }

  {
    struct in_addr cur;
    struct in_addr cgw;
    char ipbuf[20];
    char gwbuf[20];
    char maskbuf[20];
    uint8_t flags = 0;
    int up;

    memset(&cur, 0, sizeof(cur));
    memset(&cgw, 0, sizeof(cgw));

    up = netlib_ifup("wlan0");
    netlib_get_ipv4addr("wlan0", &cur);
    netlib_get_dripv4addr("wlan0", &cgw);
    netlib_getifstatus("wlan0", &flags);

    inet_ntoa_r(cur, ipbuf, sizeof(ipbuf));
    inet_ntoa_r(cgw, gwbuf, sizeof(gwbuf));
    inet_ntoa_r(m, maskbuf, sizeof(maskbuf));

    printf("velaops: static ifup=%d flags=0x%02x ip=%s gw=%s mask=%s%c",
           up, flags, ipbuf, gwbuf, maskbuf, 10);
  }

  return 0;
}

static void net_lease_save(const char *ifname)
{
  struct in_addr ip;
  struct in_addr gw;
  struct in_addr mask;
  FILE *fp;

  memset(&ip, 0, sizeof(ip));
  memset(&gw, 0, sizeof(gw));
  memset(&mask, 0, sizeof(mask));

  if (netlib_get_ipv4addr(ifname, &ip) < 0 || ip.s_addr == 0)
    {
      return;
    }

  netlib_get_dripv4addr(ifname, &gw);
  netlib_get_ipv4netmask(ifname, &mask);

  fp = fopen(VELAOPS_NET_FILE, "w");
  if (fp == NULL)
    {
      return;
    }

  fprintf(fp, "%s %s %s\n", inet_ntoa(ip), inet_ntoa(gw), inet_ntoa(mask));
  fclose(fp);
  printf("velaops: lease saved %s%c", inet_ntoa(ip), 10);
}

static int net_lease_load_apply(const char *ifname)
{
  char ip[32];
  char gw[32];
  char mask[32];
  struct in_addr a;
  FILE *fp;

  fp = fopen(VELAOPS_NET_FILE, "r");
  if (fp == NULL)
    {
      return -1;
    }

  ip[0] = 0;
  gw[0] = 0;
  mask[0] = 0;

  if (fscanf(fp, "%31s %31s %31s", ip, gw, mask) != 3)
    {
      fclose(fp);
      return -1;
    }

  fclose(fp);

  if (inet_pton(AF_INET, ip, &a) != 1)
    {
      return -1;
    }

  netlib_set_ipv4addr(ifname, &a);

  if (inet_pton(AF_INET, mask, &a) == 1 && a.s_addr != 0)
    {
      netlib_set_ipv4netmask(ifname, &a);
    }

  if (inet_pton(AF_INET, gw, &a) == 1 && a.s_addr != 0)
    {
      netlib_set_dripv4addr(ifname, &a);
    }

  printf("velaops: static fallback %s gw %s%c", ip, gw, 10);
  return 0;
}

static int tcp_reachable(const char *host, const char *port, int seconds)
{
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct timeval tv;
  int fd;
  int rc = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL)
    {
      return -1;
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd >= 0)
    {
      tv.tv_sec = seconds;
      tv.tv_usec = 0;
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

      if (connect(fd, res->ai_addr, res->ai_addrlen) == 0)
        {
          rc = 0;
        }
      else
        {
          printf("velaops: tcp %s:%s failed errno=%d%c", host, port, errno, 10);
        }

      close(fd);
    }

  freeaddrinfo(res);
  return rc;
}

static int sta_wait_ip(const char *ifname, int seconds)
{
  struct in_addr addr;
  uint8_t flags;
  int i;

  for (i = 0; i < seconds; i++)
    {
      flags = 0;
      memset(&addr, 0, sizeof(addr));

      if (sta_associated(ifname) &&
          netlib_getifstatus(ifname, &flags) == 0 &&
          (flags & IFF_RUNNING) != 0 &&
          netlib_get_ipv4addr(ifname, &addr) == 0 &&
          addr.s_addr != 0 &&
          addr.s_addr != htonl(0x0a000002))
        {
          printf("velaops: %s got %s\n", ifname, inet_ntoa(addr));
          return 0;
        }

      sleep(1);
    }

  return -1;
}

static const char *g_boot_ssid;
static const char *g_boot_pass;
static const char *g_boot_host;
static const char *g_boot_port;

#define VELAOPS_BUILD "R7"

int main(int argc, FAR char *argv[])
{
  static velaops_snapshot_t snap;
  static velaops_ui_state_t state;
  struct fb_videoinfo_s vi;
  uint16_t *fb = NULL;
  int fd;
  int rc;
  int i;

  /* NSH's task_spawn fallback drops argv[0] while system() keeps it. */

  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc > 0 && strcmp(argv[0], "velaops") != 0)
    {
      argv--;
      argc++;
    }

  if (argc < 2)
    {
      printf("usage: velaops page <0-5> | demo [secs] | alert [text]\n"
             "       velaops collect <host> <port> <user> <pass> "
             "[services] [ports]\n"
             "       velaops run <host> <port> <user> <pass> "
             "[services] [ports] [repair] [secs]\n"
             "       velaops hwtest\n"
             "       velaops store load | save <text> | set <key> <value> "
             "| del <key> | clear\n");
      return EXIT_FAILURE;
    }

  printf("velaops: build %s%c", VELAOPS_BUILD, 10);

  velaops_ssh_prealloc();
  velaops_llm_prealloc();
  health_cfg_load();

  if (strcmp(argv[1], "provision") == 0)
    {
      return velaops_prov_run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "store") == 0)
    {
      static char stbuf[1001];
      int src;

      if (argc > 2 && strcmp(argv[2], "save") == 0 && argc > 3)
        {
          src = velaops_conf_save(argv[3]);
          printf("velaops: store save rc=%d%c", src, 10);
          return src == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      if (argc > 2 && strcmp(argv[2], "load") == 0)
        {
          src = velaops_conf_load(stbuf, sizeof(stbuf));
          printf("velaops: store load rc=%d text=%s%c", src,
                 src == 0 ? stbuf : "", 10);
          return src == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      if (argc > 2 && strcmp(argv[2], "clear") == 0)
        {
          src = velaops_conf_clear();
          printf("velaops: store clear rc=%d%c", src, 10);
          return src == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      if (argc > 4 && strcmp(argv[2], "set") == 0)
        {
          static char newbuf[1001];
          size_t klen = strlen(argv[3]);
          size_t vlen = strlen(argv[4]);
          size_t nlen = 0;
          char *cur = stbuf;
          char *nl;
          size_t len;
          int same;

          if (klen == 0 || velaops_conf_load(stbuf, sizeof(stbuf)) < 0)
            {
              stbuf[0] = '\0';
            }

          while (*cur != '\0')
            {
              nl = strchr(cur, '\n');
              len = nl != NULL ? (size_t)(nl - cur) : strlen(cur);
              same = len > klen && cur[klen] == '=' &&
                     strncmp(cur, argv[3], klen) == 0;

              if (!same)
                {
                  if (nlen + len + 1 >= sizeof(newbuf))
                    {
                      printf("velaops: store set too long%c", 10);
                      return EXIT_FAILURE;
                    }

                  memcpy(newbuf + nlen, cur, len);
                  nlen += len;
                  newbuf[nlen++] = '\n';
                }

              cur = nl != NULL ? nl + 1 : cur + len;
            }

          if (nlen + klen + vlen + 3 > sizeof(newbuf))
            {
              printf("velaops: store set too long%c", 10);
              return EXIT_FAILURE;
            }

          memcpy(newbuf + nlen, argv[3], klen);
          nlen += klen;
          newbuf[nlen++] = '=';
          memcpy(newbuf + nlen, argv[4], vlen);
          nlen += vlen;
          newbuf[nlen++] = '\n';
          newbuf[nlen] = '\0';

          src = velaops_conf_save(newbuf);
          printf("velaops: store set %s rc=%d%c", argv[3], src, 10);
          return src == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      if (argc > 3 && strcmp(argv[2], "del") == 0)
        {
          static char newbuf[1001];
          size_t klen = strlen(argv[3]);
          size_t nlen = 0;
          char *cur = stbuf;
          char *nl;
          size_t len;
          int same;

          if (klen == 0 || velaops_conf_load(stbuf, sizeof(stbuf)) < 0)
            {
              stbuf[0] = '\0';
            }

          while (*cur != '\0')
            {
              nl = strchr(cur, '\n');
              len = nl != NULL ? (size_t)(nl - cur) : strlen(cur);
              same = len > klen && cur[klen] == '=' &&
                     strncmp(cur, argv[3], klen) == 0;

              if (!same)
                {
                  if (nlen + len + 1 >= sizeof(newbuf))
                    {
                      return EXIT_FAILURE;
                    }

                  memcpy(newbuf + nlen, cur, len);
                  nlen += len;
                  newbuf[nlen++] = '\n';
                }

              cur = nl != NULL ? nl + 1 : cur + len;
            }

          newbuf[nlen] = '\0';
          src = nlen > 0 ? velaops_conf_save(newbuf) : velaops_conf_clear();
          printf("velaops: store del %s rc=%d%c", argv[3], src, 10);
          return src == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "diag") == 0)
    {
      static char url[128];
      static char model[64];
      static char key[128];
      static char text[512];

      velaops_conf_get("llm_url", url, sizeof(url));
      velaops_conf_get("llm_model", model, sizeof(model));
      velaops_conf_get("llm_key", key, sizeof(key));

      if (url[0] == '\0')
        {
          strcpy(url, "https://token-plan-cn.xiaomimimo.com/v1");
        }

      if (model[0] == '\0')
        {
          strcpy(model, "mimo-v2.6-flash");
        }

      rc = velaops_llm_chat(url, model, key,
                            "服务器 CPU 92%、内存 88%、nginx 已停止，"
                            "用一句中文说明最该先做什么。",
                            text, sizeof(text));
      printf("velaops: diag rc=%d text=%s%c", rc, text, 10);
      return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (strcmp(argv[1], "boot") == 0)
    {
      static char bmode[16];

      if (velaops_conf_get("boot_mode", bmode, sizeof(bmode)) == 0 &&
          strcmp(bmode, "hwtest") == 0)
        {
          return hwtest_run();
        }

      static char conf_ssid[64];
      static char conf_wpass[96];
      static char conf_host[64];
      static char conf_port[8];
      static char conf_user[32];
      static char conf_pass[96];
      static char conf_svcs[96];
      static char conf_ports[64];
      static char conf_repair[96];
      static char conf_secs[4] = "0";
      static FAR char *bargv[10];

      if (argc > 2 && strcmp(argv[2], "setup") == 0)
        {
          printf("velaops: setup mode requested by BOOT button\n");
          return velaops_prov_run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      if ((velaops_conf_flags_get() & VELAOPS_FLAG_RESCAN) != 0)
        {
          printf("velaops: rescan phase, scanning before setup%c", 10);
          velaops_conf_flags_clear();
          velaops_scan_to_file();
          sleep(1);
          boardctl(BOARDIOC_RESET, 0);
          return EXIT_FAILURE;
        }

      if ((velaops_conf_flags_get() & VELAOPS_FLAG_SETUP) != 0)
        {
          printf("velaops: previous wifi failure, enter setup mode%c", 10);
          velaops_conf_flags_clear();
          return velaops_prov_run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      if (!velaops_config_present())
        {
          return velaops_prov_run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        }

      velaops_conf_get("wifi_ssid", conf_ssid, sizeof(conf_ssid));
      velaops_conf_get("wifi_pass", conf_wpass, sizeof(conf_wpass));
      velaops_conf_get("ssh_host", conf_host, sizeof(conf_host));
      velaops_conf_get("ssh_port", conf_port, sizeof(conf_port));
      velaops_conf_get("ssh_user", conf_user, sizeof(conf_user));
      velaops_conf_get("ssh_pass", conf_pass, sizeof(conf_pass));
      velaops_conf_get("services", conf_svcs, sizeof(conf_svcs));
      velaops_conf_get("ports", conf_ports, sizeof(conf_ports));
      velaops_conf_get("repair", conf_repair, sizeof(conf_repair));

      if (conf_host[0] == '\0')
        {
          printf("velaops: config has no ssh_host\n");
          return EXIT_FAILURE;
        }

      if (conf_port[0] == '\0')
        {
          strcpy(conf_port, "22");
        }

      if (conf_user[0] == '\0')
        {
          strcpy(conf_user, "root");
        }

      /* Connect after the framebuffer is allocated: bringing Wi-Fi up
       * fragments the heap and a later 150 KB allocation fails. */

      g_boot_ssid = conf_ssid;
      g_boot_pass = conf_wpass;
      g_boot_host = conf_host;
      g_boot_port = conf_port;

      bargv[0] = argv[0];
      bargv[1] = "run";
      bargv[2] = conf_host;
      bargv[3] = conf_port;
      bargv[4] = conf_user;
      bargv[5] = conf_pass;
      bargv[6] = conf_svcs;
      bargv[7] = conf_ports;
      bargv[8] = conf_repair;
      bargv[9] = conf_secs;
      argc = 10;
      argv = bargv;
    }

  fd = lcd_open(&vi, &fb);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  g_stop = 0;
  signal(SIGTERM, stop_handler);
  signal(SIGINT, stop_handler);

  printf("velaops: lcd %ux%u fmt=%d\n", vi.xres, vi.yres, vi.fmt);
  demo_state(&state);

  if (g_boot_ssid != NULL)
    {
      int attempt;
      int got_ip;

      sta_connect(g_boot_ssid, g_boot_pass);

      got_ip = (sta_wait_ip("wlan0", 12) == 0);

      for (attempt = 1; attempt < 3 && !got_ip; attempt++)
        {
          printf("velaops: dhcp retry %d%c", attempt, 10);
          dhcp_lease("wlan0");
          got_ip = (sta_wait_ip("wlan0", 12) == 0);
        }

      if (got_ip)
        {
          net_lease_save("wlan0");
        }

      if (!got_ip)
        {
          if (static_from_config() == 0 &&
              tcp_reachable(g_boot_host, g_boot_port, 4) == 0)
            {
              printf("velaops: static config works, continue%c", 10);
              g_boot_ssid = NULL;
              goto online_ok;
            }

          if (net_lease_load_apply("wlan0") == 0 &&
              tcp_reachable(g_boot_host, g_boot_port, 4) == 0)
            {
              printf("velaops: cached lease works, continue%c", 10);
              g_boot_ssid = NULL;
              goto online_ok;
            }

          printf("velaops: wifi '%s' not reachable, restart into setup mode%c",
                 g_boot_ssid, 10);

          velaops_conf_flags_set(VELAOPS_FLAG_SETUP);

          sleep(1);
          boardctl(BOARDIOC_RESET, 0);
          free(fb);
          close(fd);
          return EXIT_FAILURE;
        }

      g_boot_ssid = NULL;
    }

online_ok:

  if (strcmp(argv[1], "page") == 0)
    {
      int page = argc > 2 ? atoi(argv[2]) : 0;

      rc = velaops_ui_render(fb, vi.xres, vi.yres, &state,
                             page % VELAOPS_UI_PAGES);
      if (rc == 0)
        {
          rc = lcd_flush(fd, &vi, fb);
        }

      printf("velaops: page %d %s\n", page, rc == 0 ? "OK" : "FAILED");
    }
  else if (strcmp(argv[1], "demo") == 0)
    {
      int seconds = argc > 2 ? atoi(argv[2]) : 24;
      int page = 0;
      int elapsed = 0;

      while (elapsed < seconds)
        {
          velaops_ui_render(fb, vi.xres, vi.yres, &state, page);
          lcd_flush(fd, &vi, fb);
          printf("velaops: page %d\n", page);
          sleep(3);
          elapsed += 3;
          page = (page + 1) % VELAOPS_UI_PAGES;
        }
    }
  else if (strcmp(argv[1], "alert") == 0)
    {
      const char *text = argc > 2 ? argv[2] : "CPU > 90% FOR 3 MIN";

      for (i = 0; i < 6; i++)
        {
          velaops_ui_render_alert(fb, vi.xres, vi.yres,
                                  "告警", text, i & 1);
          lcd_flush(fd, &vi, fb);
          usleep(400000);
        }
    }
  else if (strcmp(argv[1], "approve") == 0)
    {
      const char *svcs;
      const char *ports;
      const char *target;
      int is_kill;
      int idx;

      if (argc < 9)
        {
          printf("velaops: approve <host> <port> <user> <pass> "
                 "<services> <ports> <target|kill:name>\n");
          free(fb);
          close(fd);
          return EXIT_FAILURE;
        }

      svcs   = argv[6];
      ports  = argv[7];
      target = argv[8];
      is_kill = (strncmp(target, "kill:", 5) == 0);

      board_hwtest_gpio_init();
      board_hwtest_gpio_write(0, 0);
      board_hwtest_gpio_write(1, 0);

      if (!is_kill)
        {
          idx = csv_find_index(svcs, target, strlen(target));
          if (idx < 0)
            {
              printf("velaops: target %s is not monitored\n", target);
              free(fb);
              close(fd);
              return EXIT_FAILURE;
            }

          rc = velaops_ssh_restart(argv[2], argv[3], argv[4], argv[5],
                                   target);
        }
      else
        {
          rc = velaops_ssh_kill(argv[2], argv[3], argv[4], argv[5],
                                target + 5);
        }

      if (velaops_ssh_collect(argv[2], argv[3], argv[4], argv[5], svcs,
                              ports, &snap) == 0)
        {
          int fixed;

          apply_snapshot(&state, &snap, argv[2]);

          if (is_kill)
            {
              fixed = strcmp(snap.top_name, target + 5) != 0 ||
                      snap.top_cpu < 50.0f;
            }
          else
            {
              idx = csv_find_index(svcs, target, strlen(target));
              fixed = idx >= 0 && idx < snap.watch_count &&
                      snap.watch_active[idx];
            }

          state.action_state = (rc == 0 && fixed) ? "DONE" : "FAILED";
          state.action_service = target;
          state.action_result = (rc == 0 && fixed) ?
                                (is_kill ? "PROCESS KILLED AND VERIFIED" :
                                 "RESTARTED AND VERIFIED") :
                                (is_kill ? "KILL FAILED" : "RESTART FAILED");
        }
      else
        {
          state.action_state = "FAILED";
          state.action_service = target;
          state.action_result = "VERIFY COLLECT FAILED";
        }

      velaops_ui_render(fb, vi.xres, vi.yres, &state, 5);
      lcd_flush(fd, &vi, fb);
      printf("velaops: approve %s rc=%d state=%s\n", target, rc,
             state.action_state);
    }
  else if (strcmp(argv[1], "collect") == 0 ||
           strcmp(argv[1], "run") == 0)
    {
      static char target[48];
      const char *host;
      const char *port;
      const char *user;
      const char *pass;
      const char *svcs;
      const char *ports;
      const char *repair = "";
      char alert_text[96];
      int run = strcmp(argv[1], "run") == 0;
      int seconds = 0;
      int page = 0;
      int elapsed = 0;
      int alert_streak = 0;
      int fail_streak = 0;
      int alert = 0;
      int show_alert = 0;
      int muted = 0;
      int blink = 0;
      int action_kind = 0;
      int bfd = -1;
      int llm_tick = 0;
      int press_ms = 0;
      int long_fired = 0;
      uint32_t now;
      int tick;

      if (argc < 6)
        {
          printf("velaops: collect/run needs host port user pass\n");
          free(fb);
          close(fd);
          return EXIT_FAILURE;
        }

      host  = argv[2];
      port  = argv[3];
      user  = argv[4];
      pass  = argv[5];
      svcs  = argc > 6 ? argv[6] : "";
      ports = argc > 7 ? argv[7] : "";

      if (run)
        {
          repair = argc > 8 ? argv[8] : "";

          if (argc > 9)
            {
              seconds = atoi(argv[9]);
            }

          board_hwtest_gpio_init();
          board_hwtest_gpio_write(0, 0);
          board_hwtest_gpio_write(1, 0);
        }

      bfd = open("/dev/gpio1", O_RDONLY);

      do
        {
          int crit;
          int just_alert = 0;

          if (velaops_ssh_collect(host, port, user, pass, svcs, ports,
                                  &snap) == 0)
            {
              fail_streak = 0;
              apply_snapshot(&state, &snap, host);
              printf("velaops: %s score=%d%s cpu=%d mem=%d disk=%d "
                     "fail=%d ports=%d top=%s/%d%%%c",
                     host, g_hres.total, g_hres.critical ? " CRIT" : "",
                     (int)(snap.cpu + 0.5f), (int)(snap.mem + 0.5f),
                     (int)(snap.disk + 0.5f), snap.svc_failed,
                     snap.port_count, snap.top_name,
                     (int)(snap.top_cpu + 0.5f), 10);
            }
          else
            {
              fail_streak++;
              printf("velaops: collect failed for %s (%d)%c", host,
                     fail_streak, 10);
              apply_snapshot(&state, &snap, host);
            }

          crit = snap.online ?
                 (g_hres.critical ||
                  g_hres.total < g_hcfg.alert_score) :
                 (fail_streak >= 3);

          if (crit)
            {
              alert_streak++;
            }
          else
            {
              alert_streak = 0;
            }

          if (!alert &&
              ((crit && (!snap.online || g_hres.critical)) ||
               alert_streak >= 2))
            {
              alert = 1;
              show_alert = 1;
              muted = 0;
              just_alert = 1;
              printf("velaops: alert score=%d crit=%d reason=%s%c",
                     g_hres.total, g_hres.critical, g_hres.reason, 10);
            }

          if (alert && g_hres.total >= g_hcfg.recover_score &&
              !g_hres.critical)
            {
              alert = 0;
              show_alert = 0;
              muted = 0;
              printf("velaops: health recovered score=%d%c",
                     g_hres.total, 10);
            }

          target[0] = '\0';
          action_kind = 0;

          if (snap.online)
            {
              if (pick_repair_target(svcs, repair, &snap, target,
                                     sizeof(target)))
                {
                  action_kind = 1;
                }
              else if (kill_allowed())
                {
                  strlcpy(target, g_hres.kill_name, sizeof(target));
                  action_kind = 2;
                }
            }

          if (alert)
            {
              snprintf(alert_text, sizeof(alert_text),
                       "%s 健康分 %d", g_hres.reason, g_hres.total);
            }

          if (just_alert)
            {
              state.action_state = "WAITING";
              state.action_service = target[0] != '\0' ? target : "-";
              state.action_result = action_kind == 2 ?
                                    "HOLD BOOT 2S KILL" :
                                    (action_kind == 1 ?
                                     "HOLD BOOT 2S RESTART" :
                                     "NO ALLOWED ACTION");

              printf("velaops: alert target=%s kind=%d\n",
                     target[0] != '\0' ? target : "none", action_kind);
            }

          if ((just_alert || llm_tick <= 0) && snap.online)
            {
              static char l_url[128];
              static char l_model[64];
              static char l_key[128];
              static char l_prompt[320];
              static char l_ans[384];

              velaops_conf_get("llm_url", l_url, sizeof(l_url));
              velaops_conf_get("llm_model", l_model, sizeof(l_model));
              velaops_conf_get("llm_key", l_key, sizeof(l_key));

              llm_tick = 6;

              if (l_key[0] == '\0')
                {
                  printf("velaops: llm skipped (no api key)%c", 10);
                }
              else
                {
                  if (run)
                    {
                      board_hwtest_gpio_write(1, 0);
                    }

                  snprintf(l_prompt, sizeof(l_prompt),
                           "服务器 %s 健康分 %d/100，CPU %d%% 内存 %d%% "
                           "磁盘 %d%%，失败单元 %d 个，监听端口 %d 个。"
                           "主要问题：%s。TOP 进程 %s 占用 %.0f%%。"
                           "用一句中文给出最重要的运维建议。",
                           host, g_hres.total, (int)(snap.cpu + 0.5f),
                           (int)(snap.mem + 0.5f), (int)(snap.disk + 0.5f),
                           snap.svc_failed, snap.port_count, g_hres.reason,
                           snap.top_name, (double)snap.top_cpu);

                  if (velaops_llm_chat(l_url, l_model, l_key, l_prompt,
                                       l_ans, sizeof(l_ans)) == 0)
                    {
                      printf("velaops: llm ok: %s%c", l_ans, 10);
                      strlcpy(g_llm_text, l_ans, sizeof(g_llm_text));
                      state.summary = g_llm_text;
                    }
                  else
                    {
                      printf("velaops: llm call failed%c", 10);
                    }
                }
            }

          if (llm_tick > 0)
            {
              llm_tick--;
            }

          now = hw_ms();

          if (run)
            {
              bool beep = alert && !muted && ((now / 250) & 1) == 0;

              board_hwtest_gpio_write(1, beep);
              board_hwtest_gpio_write(0, alert && !muted &&
                                      ((now / 250) & 1));
            }

          if (!run)
            {
              velaops_ui_render(fb, vi.xres, vi.yres, &state, page);
              lcd_flush(fd, &vi, fb);
              break;
            }

          if (alert && show_alert)
            {
              velaops_ui_render_alert(fb, vi.xres, vi.yres, "告警",
                                      alert_text, blink & 1);
            }
          else
            {
              velaops_ui_render(fb, vi.xres, vi.yres, &state, page);
            }

          lcd_flush(fd, &vi, fb);

          for (tick = 0; tick < 50; tick++)
            {
              bool pressed = false;

              if (g_stop)
                {
                  break;
                }

              if (bfd >= 0)
                {
                  ioctl(bfd, GPIOC_READ, (unsigned long)(uintptr_t)&pressed);
                }

              if (!pressed)
                {
                  press_ms += 100;

                  if (press_ms == 2000 && !long_fired)
                    {
                      long_fired = 1;

                      if (alert && action_kind != 0 &&
                          state.action_state != NULL &&
                          strcmp(state.action_state, "WAITING") == 0)
                        {
                          int fixed = 0;
                          int idx;

                          printf("velaops: approved %s %s\n",
                                 action_kind == 2 ? "kill" : "restart",
                                 target);

                          state.action_state = "RUNNING";
                          state.action_result = target;
                          velaops_ui_render(fb, vi.xres, vi.yres, &state, 5);
                          lcd_flush(fd, &vi, fb);
                          board_hwtest_gpio_write(1, 0);

                          if (action_kind == 2)
                            {
                              rc = velaops_ssh_kill(host, port, user, pass,
                                                    target);
                            }
                          else
                            {
                              rc = velaops_ssh_restart(host, port, user, pass,
                                                       target);
                            }

                          if (velaops_ssh_collect(host, port, user, pass,
                                                  svcs, ports, &snap) == 0)
                            {
                              apply_snapshot(&state, &snap, host);

                              if (action_kind == 2)
                                {
                                  fixed =
                                    strcmp(snap.top_name, target) != 0 ||
                                    snap.top_cpu < 50.0f;
                                }
                              else
                                {
                                  idx = csv_find_index(svcs, target,
                                                       strlen(target));
                                  fixed = idx >= 0 &&
                                          idx < snap.watch_count &&
                                          snap.watch_active[idx];
                                }

                              if (rc == 0 && fixed)
                                {
                                  state.action_state = "DONE";
                                  state.action_result = action_kind == 2 ?
                                    "KILLED AND VERIFIED" :
                                    "RESTARTED AND VERIFIED";
                                  alert = 0;
                                  show_alert = 0;
                                  muted = 0;
                                  alert_streak = 0;
                                }
                              else
                                {
                                  state.action_state = "FAILED";
                                  state.action_result = action_kind == 2 ?
                                    "KILL FAILED" : "RESTART FAILED";
                                }
                            }
                          else
                            {
                              state.action_state = "FAILED";
                              state.action_result = "VERIFY COLLECT FAILED";
                            }

                          page = 5;
                          velaops_ui_render(fb, vi.xres, vi.yres, &state,
                                            page);
                          lcd_flush(fd, &vi, fb);
                          printf("velaops: action %s state=%s\n",
                                 target, state.action_state);
                        }
                      else if (alert)
                        {
                          show_alert = 0;
                          muted = 1;
                        }
                    }
                }
              else
                {
                  if (press_ms > 0 && press_ms < 1000)
                    {
                      if (show_alert)
                        {
                          show_alert = 0;
                          muted = 1;
                        }
                      else
                        {
                          page = (page + 1) % VELAOPS_UI_PAGES;
                        }

                      velaops_ui_render(fb, vi.xres, vi.yres, &state, page);
                      lcd_flush(fd, &vi, fb);
                    }

                  press_ms = 0;
                  long_fired = 0;
                }

              now = hw_ms();

              if (run)
                {
                  bool beep = alert && !muted && ((now / 250) & 1) == 0;

                  board_hwtest_gpio_write(1, beep);
                  board_hwtest_gpio_write(0, alert && !muted &&
                                          ((now / 250) & 1));
                }

              if (alert && show_alert && (tick % 5) == 4)
                {
                  blink++;
                  velaops_ui_render_alert(fb, vi.xres, vi.yres, "告警",
                                          alert_text, blink & 1);
                  lcd_flush(fd, &vi, fb);
                }

              usleep(100000);
            }

          elapsed += 5;
        }
      while (!g_stop && (seconds <= 0 || elapsed < seconds));

      if (run)
        {
          board_hwtest_gpio_write(1, 0);
          board_hwtest_gpio_write(0, 0);
        }

      if (bfd >= 0)
        {
          close(bfd);
        }
    }

  else
    {
      printf("velaops: unknown command %s\n", argv[1]);
      free(fb);
      close(fd);
      return EXIT_FAILURE;
    }

  free(fb);
  close(fd);
  return EXIT_SUCCESS;
}
