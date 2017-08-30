#
# Makefile for Asterisk stasis to amqp resource
# Copyright (C) 2017, Sylvain Boily
#
# This program is free software, distributed under the terms of
# the GNU General Public License Version 3. See the COPYING file
# at the top of the source tree.
#

INSTALL = install
ASTETCDIR = $(INSTALL_PREFIX)/etc/asterisk
SAMPLENAME = stasis_amqp.conf.sample
CONFNAME = $(basename $(SAMPLENAME))

TARGET = res_stasis_amqp.so
OBJECTS = res_stasis_amqp.o
CFLAGS += -I.
CFLAGS += -DHAVE_STDINT_H=1
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Winit-self -Wmissing-format-attribute \
          -Wformat=2 -g -fPIC -D_GNU_SOURCE -D'AST_MODULE="res_stasis_amqp"' -D'AST_MODULE_SELF_SYM=__internal_res_stasis_amqp_self'
LDFLAGS = -Wall -shared

.PHONY: install clean

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) -c $(CFLAGS) -o $@ $<

install: $(TARGET)
	mkdir -p $(DESTDIR)/usr/lib/asterisk/modules
	install -m 644 $(TARGET) $(DESTDIR)/usr/lib/asterisk/modules/
	install -m 644 documentation/* $(DESTDIR)/usr/share/asterisk/documentation/thirdparty
	@echo " +-------- res_stasis_amqp installed --------+"
	@echo " +                                           +"
	@echo " + res_amqp has successfully been installed  +"
	@echo " + If you would like to install the sample   +"
	@echo " + configuration file run:                   +"
	@echo " +                                           +"
	@echo " +              make samples                 +"
	@echo " +-------------------------------------------+"

clean:
	rm -f $(OBJECTS)
	rm -f $(TARGET)

samples:
	$(INSTALL) -m 644 $(SAMPLENAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME)
	@echo " ------- res_stasis_amqp config installed ---------"
