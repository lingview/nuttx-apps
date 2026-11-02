/****************************************************************************
 * apps/examples/scp_put/scp_put_main.c
 *
 * Upload a local file to an SSH server with libssh2 SCP.
 *
 *   scp_put [-k keyfile] <host> <port> <user> <password|-> <local> <remote>
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libssh2.h>

static int ssh_connect(const char *host, const char *port)
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
      printf("scp_put: getaddrinfo failed: %d\n", ret);
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

int main(int argc, char *argv[])
{
  LIBSSH2_SESSION *session = NULL;
  LIBSSH2_CHANNEL *channel;
  const char *keyfile = NULL;
  const char *host;
  const char *port;
  const char *user;
  const char *pass;
  const char *local;
  const char *remote;
  struct stat st;
  char *data = NULL;
  size_t sent = 0;
  int fd = -1;
  int lfd = -1;
  int rc;
  int i = 1;
  size_t total;

  if (argc > 2 && strcmp(argv[1], "-k") == 0)
    {
      keyfile = argv[2];
      i = 3;
    }

  if (argc - i < 6)
    {
      printf("usage: scp_put [-k keyfile] <host> <port> <user> "
             "<password|-> <local> <remote>\n");
      return EXIT_FAILURE;
    }

  host   = argv[i++];
  port   = argv[i++];
  user   = argv[i++];
  pass   = argv[i++];
  local  = argv[i++];
  remote = argv[i++];

  if (stat(local, &st) != 0)
    {
      printf("scp_put: stat(%s) failed: %d\n", local, errno);
      return EXIT_FAILURE;
    }

  total = (size_t)st.st_size;

  lfd = open(local, O_RDONLY);
  if (lfd < 0)
    {
      printf("scp_put: open(%s) failed: %d\n", local, errno);
      return EXIT_FAILURE;
    }

  data = malloc(total);
  if (data == NULL || read(lfd, data, total) != (ssize_t)total)
    {
      printf("scp_put: read failed\n");
      goto out;
    }

  libssh2_init(0);

  fd = ssh_connect(host, port);
  if (fd < 0)
    {
      printf("scp_put: cannot connect\n");
      goto out;
    }

  session = libssh2_session_init();
  if (session == NULL)
    {
      goto out;
    }

  libssh2_session_set_blocking(session, 1);

  rc = libssh2_session_handshake(session, fd);
  if (rc != 0)
    {
      printf("scp_put: handshake failed: %d\n", rc);
      goto out;
    }

  printf("scp_put: connected\n");

  if (keyfile != NULL)
    {
      rc = libssh2_userauth_publickey_fromfile(session, user, NULL, keyfile,
                                               NULL);
    }
  else
    {
      rc = libssh2_userauth_password(session, user, pass);
    }

  if (rc != 0)
    {
      char *msg = NULL;
      int len = 0;

      libssh2_session_last_error(session, &msg, &len, 0);
      printf("scp_put: auth failed: %d (%s)\n", rc, msg ? msg : "?");
      goto out;
    }

  printf("scp_put: authenticated as %s, sending %s (%u bytes) -> %s\n",
         user, local, (unsigned)total, remote);

  channel = libssh2_scp_send(session, remote, 0644, total);
  if (channel == NULL)
    {
      char *msg = NULL;
      int len = 0;

      libssh2_session_last_error(session, &msg, &len, 0);
      printf("scp_put: scp_send failed (%s)\n", msg ? msg : "?");
      goto out;
    }

  printf("scp_put: scp_send channel opened\n");

  while (sent < total)
    {
      rc = libssh2_channel_write(channel, data + sent, total - sent);
      if (rc < 0)
        {
          if (rc == LIBSSH2_ERROR_EAGAIN)
            {
              continue;
            }

          printf("scp_put: write failed: %d\n", rc);
          break;
        }

      sent += (size_t)rc;
    }

  printf("scp_put: sent %u/%u bytes\n", (unsigned)sent, (unsigned)total);

  libssh2_channel_send_eof(channel);
  libssh2_channel_wait_eof(channel);
  libssh2_channel_wait_closed(channel);
  libssh2_channel_free(channel);

  printf("scp_put: exit status %d\n",
         sent == total ? 0 : -1);

out:
  if (session != NULL)
    {
      libssh2_session_disconnect(session, "bye");
      libssh2_session_free(session);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  if (lfd >= 0)
    {
      close(lfd);
    }

  free(data);
  libssh2_exit();
  return EXIT_SUCCESS;
}
