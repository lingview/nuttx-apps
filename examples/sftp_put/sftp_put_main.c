/****************************************************************************
 * apps/examples/sftp_put/sftp_put_main.c
 *
 * Upload a local file to an SSH server with libssh2 SFTP.
 *
 *   sftp_put [-k keyfile] <host> <port> <user> <password|-> <local> <remote>
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
#include <libssh2_sftp.h>

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
      printf("sftp_put: getaddrinfo failed: %d\n", ret);
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
  LIBSSH2_SFTP *sftp = NULL;
  LIBSSH2_SFTP_HANDLE *handle = NULL;
  LIBSSH2_SFTP_ATTRIBUTES attrs;
  const char *keyfile = NULL;
  const char *host;
  const char *port;
  const char *user;
  const char *pass;
  const char *local;
  const char *remote;
  struct stat st;
  char buf[2048];
  size_t total = 0;
  int fd = -1;
  int lfd = -1;
  int rc;
  int i = 1;
  ssize_t n;

  if (argc > 2 && strcmp(argv[1], "-k") == 0)
    {
      keyfile = argv[2];
      i = 3;
    }

  if (argc - i < 6)
    {
      printf("usage: sftp_put [-k keyfile] <host> <port> <user> "
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
      printf("sftp_put: stat(%s) failed: %d\n", local, errno);
      return EXIT_FAILURE;
    }

  lfd = open(local, O_RDONLY);
  if (lfd < 0)
    {
      printf("sftp_put: open(%s) failed: %d\n", local, errno);
      return EXIT_FAILURE;
    }

  libssh2_init(0);

  fd = ssh_connect(host, port);
  if (fd < 0)
    {
      printf("sftp_put: cannot connect\n");
      goto out;
    }

  session = libssh2_session_init();
  if (session == NULL)
    {
      printf("sftp_put: libssh2_session_init failed\n");
      goto out;
    }

  libssh2_session_set_blocking(session, 1);

  rc = libssh2_session_handshake(session, fd);
  if (rc != 0)
    {
      char *msg = NULL;
      int len = 0;

      libssh2_session_last_error(session, &msg, &len, 0);
      printf("sftp_put: handshake failed: %d (%s)\n", rc, msg ? msg : "?");
      goto out;
    }

  printf("sftp_put: connected, host key type: %s\n",
         libssh2_session_hostkey(session, NULL, NULL) ? "present" : "none");

  if (keyfile != NULL)
    {
      char pubkey[160];
      const char *pub = NULL;

      snprintf(pubkey, sizeof(pubkey), "%s.pub", keyfile);
      if (access(pubkey, F_OK) == 0)
        {
          pub = pubkey;
        }

      rc = libssh2_userauth_publickey_fromfile(session, user, pub, keyfile,
                                               NULL);
      if (rc != 0)
        {
          char *msg = NULL;
          int len = 0;

          libssh2_session_last_error(session, &msg, &len, 0);
          printf("sftp_put: key auth failed: %d (%s)\n", rc, msg ? msg : "?");
          goto out;
        }

      printf("sftp_put: authenticated with key as %s\n", user);
    }
  else
    {
      rc = libssh2_userauth_password(session, user, pass);
      if (rc != 0)
        {
          char *msg = NULL;
          int len = 0;

          libssh2_session_last_error(session, &msg, &len, 0);
          printf("sftp_put: auth failed: %d (%s)\n", rc, msg ? msg : "?");
          goto out;
        }

      printf("sftp_put: authenticated with password as %s\n", user);
    }

  sftp = libssh2_sftp_init(session);
  if (sftp == NULL)
    {
      printf("sftp_put: sftp_init failed: %d\n",
             libssh2_sftp_last_error(session));
      goto out;
    }

  handle = libssh2_sftp_open(sftp, remote,
                             LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT |
                             LIBSSH2_FXF_TRUNC,
                             LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                             LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
  if (handle == NULL)
    {
      printf("sftp_put: sftp_open(%s) failed: %d\n", remote,
             libssh2_sftp_last_error(sftp));
      goto out;
    }

  printf("sftp_put: sending %s (%ld bytes) -> %s\n",
         local, (long)st.st_size, remote);

  for (;;)
    {
      n = read(lfd, buf, sizeof(buf));
      if (n < 0)
        {
          printf("sftp_put: read failed: %d\n", errno);
          goto out;
        }

      if (n == 0)
        {
          break;
        }

      rc = 0;
      while (rc < n)
        {
          ssize_t w = libssh2_sftp_write(handle, buf + rc, (size_t)(n - rc));
          if (w < 0)
            {
              printf("sftp_put: sftp_write failed: %d\n",
                     libssh2_sftp_last_error(sftp));
              goto out;
            }

          rc += (int)w;
        }

      total += (size_t)n;
    }

  rc = libssh2_sftp_close(handle);
  handle = NULL;
  if (rc != 0)
    {
      printf("sftp_put: sftp_close failed: %d\n",
             libssh2_sftp_last_error(sftp));
      goto out;
    }

  printf("sftp_put: sent %u/%ld bytes\n", (unsigned)total, (long)st.st_size);

  memset(&attrs, 0, sizeof(attrs));
  if (libssh2_sftp_stat(sftp, remote, &attrs) == 0)
    {
      printf("sftp_put: remote size %lu\n", (unsigned long)attrs.filesize);
      if (attrs.filesize != (unsigned long)st.st_size)
        {
          printf("sftp_put: SIZE MISMATCH\n");
          goto out;
        }
    }
  else
    {
      printf("sftp_put: stat(%s) failed: %d\n", remote,
             libssh2_sftp_last_error(sftp));
    }

  printf("sftp_put: exit status 0\n");
  libssh2_sftp_shutdown(sftp);
  sftp = NULL;

out:
  if (handle != NULL)
    {
      libssh2_sftp_close(handle);
    }

  if (sftp != NULL)
    {
      libssh2_sftp_shutdown(sftp);
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

  if (lfd >= 0)
    {
      close(lfd);
    }

  libssh2_exit();
  return EXIT_SUCCESS;
}
