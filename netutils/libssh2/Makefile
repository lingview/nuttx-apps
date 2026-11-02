############################################################################
# apps/netutils/libssh2/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += -DHAVE_CONFIG_H -DLIBSSH2_MBEDTLS
CFLAGS += -isystem $(APPDIR)/netutils/libssh2/include
CFLAGS += -I$(APPDIR)/netutils/libssh2


ifneq ($(CONFIG_NETUTILS_LIBSSH2_DEBUG),)
CFLAGS += -DLIBSSH2DEBUG
endif

CSRCS = agent.c bcrypt_pbkdf.c channel.c chacha.c cipher-chachapoly.c \
        comp.c crypt.c crypto.c global.c hostkey.c keepalive.c kex.c \
        knownhost.c mac.c misc.c packet.c pem.c poly1305.c publickey.c \
        scp.c session.c sftp.c transport.c userauth.c \
        userauth_kbd_packet.c version.c mbedtls.c

include $(APPDIR)/Application.mk
