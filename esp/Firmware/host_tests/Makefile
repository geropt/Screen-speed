# Host test harness for the HUD firmware (esp/Firmware).
#
# Compiles firmware components on the developer machine against the shims in
# host_shim/, so logic can be exercised under ASan/UBSan without a board.
# See README.md for what this can and cannot prove.
#
#   make test              build and run everything
#   make update-golden     regenerate characterization goldens (review the diff!)
#   make clean

CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -O1 -g
SANITIZE ?= -fsanitize=address,undefined

FW_ROOT     := $(abspath $(CURDIR)/..)
REPO_ROOT   := $(abspath $(FW_ROOT)/../..)
# Dataset root: the directory that contains the `tiles/` tree the device would
# find on the SD card. Override with `make DATASET_ROOT=/path test`.
DATASET_ROOT ?= $(REPO_ROOT)/python

TILE_SRC := $(FW_ROOT)/components/tile_reader
RES_SRC  := $(FW_ROOT)/components/res_metrics
VS_SRC   := $(FW_ROOT)/components/vehicle_state
# Componente compartido con el piloto (esp/components).
SHARED_SRC := $(abspath $(FW_ROOT)/../components/ruptela_framing)
SHIM_SRC := $(CURDIR)/host_shim

INCLUDES := -I$(SHIM_SRC) -I$(TILE_SRC) -I$(RES_SRC)/include -I$(VS_SRC)/include \
            -I$(SHARED_SRC)/include -I$(CURDIR)
DEFINES  := -DHOST_MOUNT_POINT='"$(DATASET_ROOT)"' \
            -DGOLDEN_PATH='"fixtures/tile_reader_baseline.golden"'

TESTS := test_tile_reader_baseline test_res_stats test_ruptela_framing \
         test_vehicle_state

.PHONY: test all update-golden clean check-dataset

all: $(TESTS)

test: check-dataset $(TESTS)
	./test_tile_reader_baseline
	./test_res_stats
	./test_ruptela_framing
	./test_vehicle_state

check-dataset:
	@test -d "$(DATASET_ROOT)/tiles" || { \
	  echo "ERROR: no tile dataset at $(DATASET_ROOT)/tiles"; \
	  echo "       regenerate it with python/extract_tiles.py or pass DATASET_ROOT=..."; \
	  exit 1; }

update-golden: $(TESTS)
	./test_tile_reader_baseline --update

test_tile_reader_baseline: test_tile_reader_baseline.c \
                           $(TILE_SRC)/tile_reader.c \
                           $(TILE_SRC)/tile_cache.c \
                           $(SHIM_SRC)/host_shim.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) $(DEFINES) -o $@ $^ -lm

# res_stats es C puro sin dependencias de ESP-IDF: no necesita shims.
test_res_stats: test_res_stats.c $(RES_SRC)/res_stats.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# vehicle_state: C puro, el reloj entra por parámetro.
test_vehicle_state: test_vehicle_state.c $(VS_SRC)/vehicle_state.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Framing compartido con el piloto, también C puro.
test_ruptela_framing: test_ruptela_framing.c \
                      $(SHARED_SRC)/nmea_framer.c \
                      $(SHARED_SRC)/imei_scanner.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

clean:
	rm -f $(TESTS)
	rm -rf $(TESTS:%=%.dSYM)
