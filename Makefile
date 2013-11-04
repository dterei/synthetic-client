CC=c++

BDWGC_VERSION=7.3
BDWGC_LOC=${HOME}/Software/bdwgc-${BDWGC_VERSION}

CFLAGS=-O2 -D_GNU_SOURCE -I/usr/local/include -I${BDWGC_LOC}/include
LDFLAGS=-levent -pthread -lgsl -lgslcblas -lgc -L/usr/local/lib -L${BDWGC_LOC}/lib

EXECUTABLE=server
SOURCE_FILES=commands.c connections.c items.c protocol.c server.c settings.c threads.c utils.c memcache_conn.c stats.c locking.c
SOURCES=$(patsubst %,src/%,$(SOURCE_FILES))
OBJECTS=$(patsubst %.c,build/%.o,$(SOURCE_FILES))

V=0

all: $(SOURCES) $(EXECUTABLE)

.PHONY: clean
clean:
	rm -f server
	rm -f build/*.o

$(OBJECTS): | build

build:
	@mkdir -p $@

$(EXECUTABLE): $(OBJECTS)
	$(CC) -o $@ $+ $(LDFLAGS)

build/%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: run
run:
	LD_PRELOAD=${BDWGC_LOC}/lib/libgc.so foreman start -e prod.env

