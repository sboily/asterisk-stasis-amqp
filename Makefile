#
# Makefile for Asterisk stasis to amqp resource
# Copyright (C) 2017, Sylvain Boily
#
# This program is free software, distributed under the terms of
# the GNU General Public License Version 3. See the COPYING file
# at the top of the source tree.
#

ASTLIBDIR:=$(shell awk '/moddir/{print $$3}' /etc/asterisk/asterisk.conf 2> /dev/null)
ifeq ($(strip $(ASTLIBDIR)),)
	MODULES_DIR:=$(INSTALL_PREFIX)/usr/lib/asterisk/modules
else
	MODULES_DIR:=$(INSTALL_PREFIX)$(ASTLIBDIR)
endif
ifeq ($(strip $(DOCDIR)),)
	DOCUMENTATION_DIR:=$(INSTALL_PREFIX)/usr/share/asterisk/documentation/thirdparty
else
	DOCUMENTATION_DIR:=$(INSTALL_PREFIX)$(DOCDIR)
endif
INSTALL = install
ASTETCDIR = $(INSTALL_PREFIX)/etc/asterisk
SAMPLENAME = stasis_amqp.conf.sample
CONFNAME = $(basename $(SAMPLENAME))

TARGET = res_stasis_amqp.so
OBJECTS = res_stasis_amqp.o
CFLAGS += -I../asterisk-amqp
CFLAGS += -DHAVE_STDINT_H=1
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Winit-self -Wmissing-format-attribute \
          -Wformat=2 -g -fPIC -D_GNU_SOURCE -D'AST_MODULE="res_stasis_amqp"' -D'AST_MODULE_SELF_SYM=__internal_res_stasis_amqp_self'
LDFLAGS = -Wall -shared

.PHONY: install clean

$(TARGET): $(OBJECTS) res_ari_amqp.so
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) -c $(CFLAGS) -o $@ $<
install: $(TARGET)
	mkdir -p $(DESTDIR)$(MODULES_DIR)
	mkdir -p $(DESTDIR)$(DOCUMENTATION_DIR)
	install -m 644 $(TARGET) $(DESTDIR)$(MODULES_DIR)
	install -m 644 res_ari_amqp.so $(DESTDIR)$(MODULES_DIR)
	install -m 644 documentation/* $(DESTDIR)$(DOCUMENTATION_DIR)
	install -D amqp.json /usr/share/asterisk/rest-api/amqp.json
	cp /wazo-res-amqp/documentation/res_amqp_config-en_US.xml /usr/share/asterisk/documentation/thirdparty/
	patch /usr/share/asterisk/rest-api/resources.json resources.json.patch

	@echo " +-------- res_stasis_amqp installed --------+"
	@echo " +                                           +"
	@echo " + res_amqp has successfully been installed  +"
	@echo " + If you would like to install the sample   +"
	@echo " + configuration file run:                   +"
	@echo " +                                           +"
	@echo " +              make samples                 +"
	@echo " +-------------------------------------------+"

install-dev:
	install -m 644 asterisk/stasis_amqp.h /usr/include/asterisk
	@echo " +-------- res_stasis_amqp headers installed --------+"

uninstall:
	rm /usr/share/asterisk/rest-api/amqp.json || true
	patch -R /usr/share/asterisk/rest-api/resources.json resources.json.patch

res_ari_amqp.so: res_ari_amqp.o resource_amqp.o
	$(CC) $(LDFLAGS)  -o $@ $^ $(LIBS)


clean:
	rm -f *.so *.o

samples:
	$(INSTALL) -m 644 $(SAMPLENAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME)
	@echo " ------- res_stasis_amqp config installed ---------"
