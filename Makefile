CC      := cc
WARN    := #-Wall -Wextra
CFLAGS  := $(WARN) -O2 -D_GNU_SOURCE -std=c99
LDFLAGS := -pthread

PKGCONF_CF := $(shell pkg-config --cflags gsl libevent)
PKGCONF_LD := $(shell pkg-config --libs gsl libevent)
CFLAGS  += $(PKGCONF_CF)
LDFLAGS += $(PKGCONF_LD)

SRCDIR := src
OBJDIR := build

EXECUTABLE := server
SOURCES    := $(wildcard $(SRCDIR)/*.c)
OBJECTS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

all: $(OBJECTS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) -o $@ $+ $(LDFLAGS)

$(OBJECTS): | build

build:
	@mkdir -p $@

build/%.o: $(SRCDIR)/%.c $(SRCDIR)/%.h
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f server
	rm -rf build
