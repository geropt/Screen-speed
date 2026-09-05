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

MATCH_SRC := $(FW_ROOT)/components/map_match
RES_SRC  := $(FW_ROOT)/components/res_metrics
VS_SRC   := $(FW_ROOT)/components/vehicle_state
UI_SRC   := $(FW_ROOT)/components/ui_model
# Componente compartido con el piloto (esp/components).
SHARED_SRC := $(abspath $(FW_ROOT)/../components/ruptela_framing)
OUTBOX_SRC := $(abspath $(FW_ROOT)/../components/outbox)
SHIM_SRC := $(CURDIR)/host_shim

INCLUDES := -I$(SHIM_SRC) -I$(MATCH_SRC)/include -I$(RES_SRC)/include -I$(VS_SRC)/include -I$(UI_SRC)/include \
            -I$(SHARED_SRC)/include -I$(OUTBOX_SRC)/include -I$(CURDIR)
DEFINES  := -DDATASET_DIR='"$(DATASET_ROOT)"' \
            -DGOLDEN_PATH='"fixtures/tile_reader_baseline.golden"'

TESTS := test_map_match_equivalence test_map_match_robustness \
         test_res_stats test_ruptela_framing test_outbox \
         test_backup_policy test_ruptela_vectors \
         test_vehicle_state test_ui_model

.PHONY: test all clean check-dataset

all: $(TESTS)

test: check-dataset $(TESTS)
	./test_map_match_equivalence
	./test_map_match_robustness
	./test_res_stats
	./test_ruptela_framing
	./test_outbox
	./test_backup_policy
	./test_ruptela_vectors
	./test_vehicle_state
	./test_ui_model

check-dataset:
	@test -d "$(DATASET_ROOT)/tiles" || { \
	  echo "ERROR: no tile dataset at $(DATASET_ROOT)/tiles"; \
	  echo "       regenerate it with python/extract_tiles.py or pass DATASET_ROOT=..."; \
	  exit 1; }

# El golden de P00 es la referencia de equivalencia: NO se regenera desde acá.
# Una diferencia significa que la extracción cambió el comportamiento.

# Replay de equivalencia: matcher extraído contra el golden congelado en P00.
# La fuente de candidatos del test lee el directorio del dataset, así que el
# matcher se ejercita sin la caché del dispositivo.
test_map_match_equivalence: test_map_match_equivalence.c $(MATCH_SRC)/map_match.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) $(DEFINES) -o $@ $^ -lm

# Robustez con tiles construidos a mano: casos que el dataset real no contiene.
test_map_match_robustness: test_map_match_robustness.c $(MATCH_SRC)/map_match.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^ -lm

# res_stats es C puro sin dependencias de ESP-IDF: no necesita shims.
test_res_stats: test_res_stats.c $(RES_SRC)/res_stats.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# vehicle_state: C puro, el reloj entra por parámetro.
test_vehicle_state: test_vehicle_state.c $(VS_SRC)/vehicle_state.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# ui_model: presentación pura, depende de vehicle_state sólo por el snapshot.
test_ui_model: test_ui_model.c $(UI_SRC)/ui_model.c $(VS_SRC)/vehicle_state.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Outbox de telemetría: pendiente / en vuelo / confirmado.
test_outbox: test_outbox.c $(OUTBOX_SRC)/outbox.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Política de permiso del respaldo.
test_backup_policy: test_backup_policy.c $(OUTBOX_SRC)/backup_policy.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Vectores independientes del protocolo: bytes esperados escritos a mano.
test_ruptela_vectors: test_ruptela_vectors.c $(SHARED_SRC)/ruptela_proto.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Framing compartido con el piloto, también C puro.
test_ruptela_framing: test_ruptela_framing.c \
                      $(SHARED_SRC)/nmea_framer.c \
                      $(SHARED_SRC)/imei_scanner.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

clean:
	rm -f $(TESTS)
	rm -rf $(TESTS:%=%.dSYM)
