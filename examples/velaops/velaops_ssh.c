/****************************************************************************
 * apps/examples/velaops/velaops_ssh.c
 *
 * Read-only evidence collection over an on-board libssh2 session.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libssh2.h>

#include "velaops_pool.h"
#include "velaops_ssh.h"

static int g_ssh_inited;

#define COLLECT_BUF 12288
#define SSH_POOL_SIZE (256 * 1024)

int velaops_ssh_prealloc(void)
{
  return velaops_pool_init(SSH_POOL_SIZE);
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int tcp_connect(const char *host, const char *port)
{
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct addrinfo *ai;
  int fd = -1;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  ret = getaddrinfo(host, port, &hints, &res);
  if (ret != 0 || res == NULL)
    {
      return -1;
    }

  for (ai = res; ai != NULL; ai = ai->ai_next)
    {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
        {
          continue;
        }

      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
          break;
        }

      close(fd);
      fd = -1;
    }

  freeaddrinfo(res);
  return fd;
}

/* Connect and authenticate. The caller owns *session / *fd on success. */

static int open_authed_session(const char *host, const char *port,
                               const char *user, const char *pass,
                               LIBSSH2_SESSION **session, int *fd)
{
  int rc;

  *fd = tcp_connect(host, port);
  if (*fd < 0)
    {
      printf("velaops: ssh connect failed\n");
      return -1;
    }

  *session = libssh2_session_init_ex(velaops_pool_alloc, velaops_pool_free,
                                     velaops_pool_realloc, NULL);
  if (*session == NULL)
    {
      printf("velaops: session init failed%c", 10);
      close(*fd);
      *fd = -1;
      return -1;
    }

  libssh2_session_set_blocking(*session, 1);

  libssh2_session_method_pref(*session, LIBSSH2_METHOD_KEX,
                              "diffie-hellman-group14-sha256");
  libssh2_session_method_pref(*session, LIBSSH2_METHOD_CRYPT_CS, "aes128-ctr");
  libssh2_session_method_pref(*session, LIBSSH2_METHOD_CRYPT_SC, "aes128-ctr");
  libssh2_session_method_pref(*session, LIBSSH2_METHOD_MAC_CS,
                              "hmac-sha2-256");
  libssh2_session_method_pref(*session, LIBSSH2_METHOD_MAC_SC,
                              "hmac-sha2-256");

  rc = libssh2_session_handshake(*session, *fd);
  if (rc != 0)
    {
      printf("velaops: ssh handshake failed: %d\n", rc);
      goto err;
    }

  if (pass != NULL && strcmp(pass, "-") == 0)
    {
      rc = libssh2_userauth_publickey_fromfile(*session, user,
                                               "/data/velaops_key.pub",
                                               "/data/velaops_key", NULL);
    }
  else
    {
      rc = libssh2_userauth_password(*session, user, pass);
    }

  if (rc != 0)
    {
      printf("velaops: ssh auth failed: %d (pass '-' means "
             "/data/velaops_key)\n", rc);
      goto err;
    }

  return 0;

err:
  libssh2_session_free(*session);
  *session = NULL;
  close(*fd);
  *fd = -1;
  return -1;
}

/* Single systemd unit name, no separators or spaces. */

static int unit_safe(const char *name)
{
  const char *p;

  if (name == NULL || *name == '\0')
    {
      return 0;
    }

  for (p = name; *p != '\0'; p++)
    {
      if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
          *p == '-' || *p == '@')
        {
          continue;
        }

      return 0;
    }

  return 1;
}

/* Accept only safe systemd unit / port list characters. */

static int csv_safe(const char *csv)
{
  const char *p;

  for (p = csv; *p != '\0'; p++)
    {
      if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
          *p == '-' || *p == '@' || *p == ',' || *p == ':')
        {
          continue;
        }

      return 0;
    }

  return 1;
}

static void build_command(const char *services_csv, char *cmd, size_t size)
{
  snprintf(cmd, size,
           "cat /proc/meminfo; echo @@V@@; df -P /; echo @@V@@; "
           "cat /proc/loadavg; echo @@V@@; "
           "cat /proc/stat | head -1; sleep 1; cat /proc/stat | head -1; "
           "echo @@V@@; "
           "for s in $(echo %s | tr ',' ' '); do systemctl is-active \"$s\"; "
           "done; echo @@V@@; "
           "ss -ltn 2>/dev/null; echo @@V@@; "
           "uptime -p 2>/dev/null | cut -c1-24 || uptime | cut -c1-24; "
           "echo @@V@@; "
           "date +%%H:%%M:%%S",
           services_csv != NULL && services_csv[0] != '\0' ? services_csv :
           "-");
}

/* Split the response into NUL terminated sections. */

static int split_sections(char *buf, char *sec[], int max)
{
  int count = 0;
  char *p = buf;

  while (count < max)
    {
      char *m;

      sec[count++] = p;
      m = strstr(p, "@@V@@");
      if (m == NULL)
        {
          break;
        }

      *m = '\0';
      p = m + 5;

      /* Skip the newline that echo added after the marker. */

      if (*p == '\n')
        {
          p++;
        }
    }

  return count;
}

static float parse_mem(char *sec)
{
  unsigned long total = 0;
  unsigned long avail = 0;
  char *p;

  p = strstr(sec, "MemTotal:");
  if (p != NULL)
    {
      sscanf(p + 9, "%lu", &total);
    }

  p = strstr(sec, "MemAvailable:");
  if (p != NULL)
    {
      sscanf(p + 13, "%lu", &avail);
    }

  if (total == 0)
    {
      return -1.0f;
    }

  return (float)(total - avail) * 100.0f / (float)total;
}

static float parse_disk(char *sec)
{
  char *pct;
  int value;

  pct = strchr(sec, '%');
  if (pct == NULL)
    {
      return -1.0f;
    }

  /* Walk back to the first digit of "NN%". */

  while (pct > sec && pct[-1] >= '0' && pct[-1] <= '9')
    {
      pct--;
    }

  if (sscanf(pct, "%d", &value) != 1)
    {
      return -1.0f;
    }

  return (float)value;
}

static float parse_load(char *sec)
{
  float value = -1.0f;

  sscanf(sec, "%f", &value);
  return value;
}

static float parse_cpu(char *sec)
{
  unsigned long long v[4][8];
  char *p = sec;
  char *nl;
  int row;

  for (row = 0; row < 2; row++)
    {
      int field;

      nl = strchr(p, '\n');
      if (nl != NULL)
        {
          *nl = '\0';
        }

      memset(v[row], 0, sizeof(v[row]));
      for (field = 0; field < 8 && p != NULL; field++)
        {
          char *end;

          p = strpbrk(p, "0123456789");
          if (p == NULL)
            {
              break;
            }

          v[row][field] = strtoull(p, &end, 10);
          p = end;
        }

      if (nl == NULL)
        {
          break;
        }

      p = nl + 1;
    }

  {
    unsigned long long idle0 = v[0][3] + v[0][4];
    unsigned long long idle1 = v[1][3] + v[1][4];
    unsigned long long sum0 = 0;
    unsigned long long sum1 = 0;
    int i;

    for (i = 0; i < 8; i++)
      {
        sum0 += v[0][i];
        sum1 += v[1][i];
      }

    if (sum1 <= sum0)
      {
        return -1.0f;
      }

    return (float)((sum1 - sum0) - (idle1 - idle0)) * 100.0f /
           (float)(sum1 - sum0);
  }
}

static void parse_services(char *sec, const char *csv, velaops_snapshot_t *snap)
{
  char *p = sec;
  const char *c = csv;
  int total = 0;
  int ok = 0;

  snap->svc_count = 0;

  while (c != NULL && *c != '\0')
    {
      const char *comma = strchr(c, ',');
      char *nl = strchr(p, '\n');
      int active = (strncmp(p, "active", 6) == 0);

      total++;
      if (active)
        {
          ok++;
        }

      if (snap->svc_count < 8)
        {
          snap->svc_active[snap->svc_count++] = active ? 1 : 0;
        }

      if (nl != NULL)
        {
          p = nl + 1;
        }
      else
        {
          p += strlen(p);
        }

      c = comma != NULL ? comma + 1 : NULL;
    }

  snap->services_total = total;
  snap->services_ok = ok;
}

static void parse_ports(char *sec, const char *csv, velaops_snapshot_t *snap)
{
  int total = 0;
  int ok = 0;
  const char *c = csv;

  while (c != NULL && *c != '\0')
    {
      const char *comma = strchr(c, ',');
      int len = comma != NULL ? (int)(comma - c) : (int)strlen(c);
      char token[16];
      char needle[24];
      char *line;

      if (len > 0 && len < (int)sizeof(token))
        {
          memcpy(token, c, len);
          token[len] = '\0';
          total++;

          snprintf(needle, sizeof(needle), ":%s ", token);

          for (line = sec; (line = strstr(line, needle)) != NULL; line++)
            {
              ok++;
              break;
            }
        }

      c = comma != NULL ? comma + 1 : NULL;
    }

  snap->ports_total = total;
  snap->ports_ok = ok;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velaops_ssh_collect(const char *host, const char *port,
                        const char *user, const char *pass,
                        const char *services_csv,
                        const char *ports_csv,
                        velaops_snapshot_t *out)
{
  LIBSSH2_SESSION *session = NULL;
  LIBSSH2_CHANNEL *channel = NULL;
  char cmd[512];
  char *buf = NULL;
  int fd = -1;
  int rc;
  int total = 0;

  memset(out, 0, sizeof(*out));
  out->cpu = -1.0f;
  out->mem = -1.0f;
  out->disk = -1.0f;
  out->load1 = -1.0f;

  if ((services_csv != NULL && !csv_safe(services_csv)) ||
      (ports_csv != NULL && !csv_safe(ports_csv)))
    {
      printf("velaops: invalid service/port list\n");
      return -1;
    }

  static char g_collect_buf[COLLECT_BUF];

  buf = g_collect_buf;

  if (!g_ssh_inited)
    {
      if (libssh2_init(0) != 0)
        {
          printf("velaops: libssh2_init failed%c", 10);
          return -1;
        }

      g_ssh_inited = 1;
    }

  rc = open_authed_session(host, port, user, pass, &session, &fd);
  if (rc != 0)
    {
          return -1;
    }

  build_command(services_csv, cmd, sizeof(cmd));

  channel = libssh2_channel_open_session(session);
  if (channel == NULL)
    {
      printf("velaops: channel open failed\n");
      goto out;
    }

  rc = libssh2_channel_exec(channel, cmd);
  if (rc != 0)
    {
      printf("velaops: exec failed: %d\n", rc);
      goto out;
    }

  for (;;)
    {
      ssize_t n;

      if (total >= COLLECT_BUF - 1)
        {
          break;
        }

      n = libssh2_channel_read(channel, buf + total,
                               (size_t)(COLLECT_BUF - 1 - total));
      if (n > 0)
        {
          total += (int)n;
        }
      else if (n == LIBSSH2_ERROR_EAGAIN)
        {
          continue;
        }
      else
        {
          break;
        }
    }

  buf[total] = '\0';

  {
    char *sec[8] = { 0 };
    char *mem;
    char *disk;
    char *load;
    char *cpu;
    char *svc;
    char *ports;
    char *up;
    char *clk;

    split_sections(buf, sec, 8);
    if (sec[0] == NULL)
      {
        printf("velaops: ssh no markers head='%.40s'%c", buf, 10);
      }
    mem   = sec[0];
    disk  = sec[1];
    load  = sec[2];
    cpu   = sec[3];
    svc   = sec[4];
    ports = sec[5];
    up    = sec[6];
    clk   = sec[7];

    if (mem != NULL)   out->mem = parse_mem(mem);
    if (disk != NULL)  out->disk = parse_disk(disk);
    if (load != NULL)  out->load1 = parse_load(load);
    if (cpu != NULL)   out->cpu = parse_cpu(cpu);
    if (svc != NULL)   parse_services(svc, services_csv, out);
    if (ports != NULL) parse_ports(ports, ports_csv, out);

    if (up != NULL)
      {
        char *nl = strchr(up, '\n');

        if (nl != NULL)
          {
            *nl = '\0';
          }

        strncpy(out->uptime, up, sizeof(out->uptime) - 1);

        if (nl != NULL)
          {
            *nl = '\n';
          }
      }

    if (clk != NULL)
      {
        char *nl = strchr(clk, '\n');

        if (nl != NULL)
          {
            *nl = '\0';
          }

        strncpy(out->clock, clk, sizeof(out->clock) - 1);
      }
  }

  out->online = 1;
  printf("velaops: collect ok cpu=%.0f mem=%.0f disk=%.0f load=%.2f "
         "svc=%d/%d ports=%d/%d\n",
         (double)out->cpu, (double)out->mem, (double)out->disk,
         (double)out->load1, out->services_ok, out->services_total,
         out->ports_ok, out->ports_total);

out:
  if (channel != NULL)
    {
      libssh2_channel_free(channel);
    }

  if (session != NULL)
    {
      libssh2_session_disconnect(session, "bye");
      libssh2_session_free(session);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  return out->online ? 0 : -1;
}

int velaops_ssh_restart(const char *host, const char *port,
                        const char *user, const char *pass,
                        const char *service)
{
  LIBSSH2_SESSION *session = NULL;
  LIBSSH2_CHANNEL *channel = NULL;
  char cmd[160];
  char buf[256];
  int fd = -1;
  int rc;
  int status = -1;

  if (!unit_safe(service))
    {
      printf("velaops: invalid service name\n");
      return -1;
    }

  if (open_authed_session(host, port, user, pass, &session, &fd) != 0)
    {
      return -1;
    }

  snprintf(cmd, sizeof(cmd),
           "systemctl restart %s || sudo -n systemctl restart %s",
           service, service);

  channel = libssh2_channel_open_session(session);
  if (channel == NULL)
    {
      printf("velaops: restart channel failed\n");
      goto out;
    }

  rc = libssh2_channel_exec(channel, cmd);
  if (rc != 0)
    {
      printf("velaops: restart exec failed: %d\n", rc);
      goto out;
    }

  for (;;)
    {
      ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));

      if (n > 0)
        {
          continue;
        }

      if (n == LIBSSH2_ERROR_EAGAIN)
        {
          continue;
        }

      break;
    }

  status = libssh2_channel_get_exit_status(channel);

out:
  if (channel != NULL)
    {
      libssh2_channel_free(channel);
    }

  libssh2_session_disconnect(session, "bye");
  libssh2_session_free(session);
  close(fd);

  printf("velaops: restart %s status=%d\n", service, status);
  return status == 0 ? 0 : -1;
}
