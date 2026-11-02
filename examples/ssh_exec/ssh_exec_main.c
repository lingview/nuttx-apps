/****************************************************************************
 * apps/examples/ssh_exec/ssh_exec_main.c
 *
 * libssh2 client used to validate the NuttX port.
 *
 *   ssh_exec [options] <host> <port> <user> <password|-> <command> [args...]
 *
 * options:
 *   -k <privatekey>   authenticate with a PEM private key file
 *   -K <seconds>      keepalive interval (0 = disabled, default 0)
 *   -r <count>        repeat the command <count> times in one session
 *   -R <count>        reconnect up to <count> times when the session drops
 *   -d <seconds>      delay before reconnecting (default 2)
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libssh2.h>

struct ssh_conn
{
  int fd;
  LIBSSH2_SESSION *session;
};

static int ssh_tcp_connect(const char *host, const char *port)
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
      printf("ssh_exec: getaddrinfo(%s:%s) failed: %d\n", host, port, ret);
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

      printf("ssh_exec: connect failed: %d\n", errno);
      close(fd);
      fd = -1;
    }

  freeaddrinfo(res);
  return fd;
}

static int ssh_session_open(struct ssh_conn *conn, const char *host,
                            const char *port, const char *user,
                            const char *pass, const char *keyfile,
                            int keepalive)
{
  int rc;

  conn->fd = ssh_tcp_connect(host, port);
  if (conn->fd < 0)
    {
      printf("ssh_exec: cannot connect\n");
      return -1;
    }

  conn->session = libssh2_session_init();
  if (conn->session == NULL)
    {
      printf("ssh_exec: libssh2_session_init failed\n");
      return -1;
    }

  libssh2_session_set_blocking(conn->session, 1);

  rc = libssh2_session_handshake(conn->session, conn->fd);
  if (rc != 0)
    {
      char *msg = NULL;
      int len = 0;

      libssh2_session_last_error(conn->session, &msg, &len, 0);
      printf("ssh_exec: handshake failed: %d (%s)\n", rc, msg ? msg : "?");
      return -1;
    }

  printf("ssh_exec: connected, host key type: %s\n",
         libssh2_session_hostkey(conn->session, NULL, NULL) ? "present" :
         "none");

  if (keepalive > 0)
    {
      libssh2_keepalive_config(conn->session, 1, (unsigned int)keepalive);
    }

  if (keyfile != NULL)
    {
      char pubkey[160];
      const char *pub = NULL;

      snprintf(pubkey, sizeof(pubkey), "%s.pub", keyfile);
      if (access(pubkey, F_OK) == 0)
        {
          pub = pubkey;
        }

      rc = libssh2_userauth_publickey_fromfile(conn->session, user, pub,
                                               keyfile, NULL);
      if (rc != 0)
        {
          char *msg = NULL;
          int len = 0;

          libssh2_session_last_error(conn->session, &msg, &len, 0);
          printf("ssh_exec: key auth failed: %d (%s)\n", rc, msg ? msg : "?");
          return -1;
        }

      printf("ssh_exec: authenticated with key as %s\n", user);
    }
  else
    {
      rc = libssh2_userauth_password(conn->session, user, pass);
      if (rc != 0)
        {
          char *msg = NULL;
          int len = 0;

          libssh2_session_last_error(conn->session, &msg, &len, 0);
          printf("ssh_exec: auth failed: %d (%s)\n", rc, msg ? msg : "?");
          return -1;
        }

      printf("ssh_exec: authenticated with password as %s\n", user);
    }

  return 0;
}

static void ssh_session_close(struct ssh_conn *conn)
{
  if (conn->session != NULL)
    {
      libssh2_session_disconnect(conn->session, "bye");
      libssh2_session_free(conn->session);
      conn->session = NULL;
    }

  if (conn->fd >= 0)
    {
      close(conn->fd);
      conn->fd = -1;
    }
}

static int run_exec(LIBSSH2_SESSION *session, const char *cmd)
{
  LIBSSH2_CHANNEL *channel;
  char buf[512];
  int rc;
  int total = 0;

  channel = libssh2_channel_open_session(session);
  if (channel == NULL)
    {
      printf("ssh_exec: channel open failed: %d\n",
             libssh2_session_last_errno(session));
      return -1;
    }

  rc = libssh2_channel_exec(channel, cmd);
  if (rc != 0)
    {
      printf("ssh_exec: exec failed: %d\n", rc);
      libssh2_channel_free(channel);
      return -1;
    }

  for (;;)
    {
      int n = libssh2_channel_read(channel, buf, sizeof(buf) - 1);
      if (n > 0)
        {
          buf[n] = '\0';
          printf("%s", buf);
          total += n;
        }
      else if (n == LIBSSH2_ERROR_EAGAIN)
        {
          continue;
        }
      else if (n < 0)
        {
          printf("\nssh_exec: read failed: %d\n", n);
          libssh2_channel_free(channel);
          return -1;
        }
      else
        {
          break;
        }
    }

  printf("\nssh_exec: exit status %d, %d bytes\n",
         libssh2_channel_get_exit_status(channel), total);

  libssh2_channel_free(channel);
  return 0;
}

int main(int argc, char *argv[])
{
  struct ssh_conn conn;
  const char *keyfile = NULL;
  int keepalive = 0;
  int repeat = 1;
  int maxretry = 0;
  int delay = 2;
  int retries = 0;
  const char *host;
  const char *port;
  const char *user;
  const char *pass;
  char cmd[512];
  int rc;
  int i;
  int round = 0;

  i = 1;
  while (i < argc && argv[i][0] == '-')
    {
      if (strcmp(argv[i], "-k") == 0 && i + 1 < argc)
        {
          keyfile = argv[++i];
        }
      else if (strcmp(argv[i], "-K") == 0 && i + 1 < argc)
        {
          keepalive = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
        {
          repeat = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc)
        {
          maxretry = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
        {
          delay = atoi(argv[++i]);
        }
      else
        {
          break;
        }

      i++;
    }

  if (argc - i < 5)
    {
      printf("usage: ssh_exec [-k keyfile] [-K keepalive_sec] [-r count] "
             "[-R reconnect_count] [-d delay_sec] "
             "<host> <port> <user> <password|-> <command> [args...]\n");
      return EXIT_FAILURE;
    }

  host = argv[i++];
  port = argv[i++];
  user = argv[i++];
  pass = argv[i++];

  cmd[0] = '\0';
  for (; i < argc; i++)
    {
      if (cmd[0] != '\0')
        {
          strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        }

      strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }

  if (repeat < 1)
    {
      repeat = 1;
    }

  if (delay < 0)
    {
      delay = 1;
    }

  conn.fd = -1;
  conn.session = NULL;

  libssh2_init(0);

  if (ssh_session_open(&conn, host, port, user, pass, keyfile, keepalive) != 0)
    {
      goto out;
    }

  if (keepalive > 0)
    {
      printf("ssh_exec: keepalive every %d s\n", keepalive);
    }

  while (round < repeat)
    {
      int secs;

      if (conn.session == NULL)
        {
          if (maxretry <= 0 || retries >= maxretry)
            {
              printf("ssh_exec: giving up (retries %d/%d)\n", retries,
                     maxretry);
              break;
            }

          retries++;
          printf("ssh_exec: reconnecting %d/%d in %d s\n", retries, maxretry,
                 delay);
          sleep((unsigned int)delay);

          if (ssh_session_open(&conn, host, port, user, pass, keyfile,
                               keepalive) != 0)
            {
              printf("ssh_exec: reconnect %d/%d failed\n", retries, maxretry);
              continue;
            }

          printf("ssh_exec: reconnected, continuing at round %d/%d\n",
                 round + 1, repeat);

          if (keepalive > 0)
            {
              printf("ssh_exec: keepalive every %d s\n", keepalive);
            }
        }

      if (repeat > 1)
        {
          printf("ssh_exec: --- round %d/%d ---\n", round + 1, repeat);
        }

      if (run_exec(conn.session, cmd) == 0)
        {
          round++;
          retries = 0;

          if (keepalive > 0 &&
              libssh2_keepalive_send(conn.session, &secs) < 0)
            {
              printf("ssh_exec: keepalive failed, session is dead\n");
              ssh_session_close(&conn);
            }

          continue;
        }

      printf("ssh_exec: session error %d after round %d\n",
             libssh2_session_last_errno(conn.session), round + 1);

      ssh_session_close(&conn);
    }

out:
  ssh_session_close(&conn);
  libssh2_exit();
  return EXIT_SUCCESS;
}
