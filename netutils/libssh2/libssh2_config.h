/* libssh2_config.h — hand-written configuration for NuttX/openvela.
 *
 * libssh2's libssh2_setup.h only defines platform features for Windows or
 * when HAVE_CONFIG_H is set, so NuttX needs this header.
 */

#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

/* Standard headers present in NuttX */

#define HAVE_UNISTD_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETDB_H 1
#define HAVE_ALLOCA 1
#define HAVE_ALLOCA_H 1

/* libc features */

#define HAVE_SELECT 1
#define HAVE_SNPRINTF 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_STRTOLL 1
#define HAVE_SSIZE_T 1
#define HAVE_O_NONBLOCK 1

/* No compression support */

/* #undef HAVE_ZLIB */

#endif /* LIBSSH2_CONFIG_H */
