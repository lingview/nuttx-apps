/****************************************************************************
 * apps/examples/velaops/velaops_ssh.h
 *
 * Read-only server evidence collection over on-board SSH.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef VELAOPS_SSH_H
#define VELAOPS_SSH_H

typedef struct
{
  int online;

  float cpu;                    /* percent, -1 unknown */
  float mem;
  float disk;
  float load1;

  int services_total;
  int services_ok;
  int svc_count;
  unsigned char svc_active[8];
  int ports_total;
  int ports_ok;

  char uptime[48];
  char clock[16];
} velaops_snapshot_t;

/* Reserve the libssh2 arena while the heap is still unfragmented. */

int velaops_ssh_prealloc(void);

/* Run the fixed read-only evidence command set. */

int velaops_ssh_collect(const char *host, const char *port,
                        const char *user, const char *pass,
                        const char *services_csv,
                        const char *ports_csv,
                        velaops_snapshot_t *out);

/* Restart one whitelisted service. Returns 0 when the command succeeded. */

int velaops_ssh_restart(const char *host, const char *port,
                        const char *user, const char *pass,
                        const char *service);

#endif /* VELAOPS_SSH_H */
