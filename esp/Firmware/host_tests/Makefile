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
PAIR_SRC := $(abspath $(FW_ROOT)/../components/pairing)
OTA_SRC := $(abspath $(FW_ROOT)/../components/ota_policy)
CAPSULE_SRC := $(abspath $(FW_ROOT)/../components/map_capsule)
CFG_SRC := $(FW_ROOT)/components/backup_cfg
# Cápsula generada para la prueba de equivalencia; se construye desde el dataset.
CAPSULE_FILE ?= $(CURDIR)/build/mapa.cap
# Streams reconstruidos de las capturas de campo, para el replay de trayectorias.
CAPTURAS := pana2 lugnes panamericana
STREAMS  := $(CAPTURAS:%=$(CURDIR)/build/%.bin)
SHIM_SRC := $(CURDIR)/host_shim

INCLUDES := -I$(SHIM_SRC) -I$(MATCH_SRC)/include -I$(RES_SRC)/include -I$(VS_SRC)/include -I$(UI_SRC)/include \
            -I$(SHARED_SRC)/include -I$(OUTBOX_SRC)/include -I$(CAPSULE_SRC)/include -I$(PAIR_SRC)/include -I$(OTA_SRC)/include -I$(CFG_SRC)/include -I$(CURDIR)
DEFINES  := -DDATASET_DIR='"$(DATASET_ROOT)"' \
            -DGOLDEN_PATH='"fixtures/tile_reader_baseline.golden"'

TESTS := test_map_match_equivalence test_map_match_robustness \
         test_res_stats test_ruptela_framing test_outbox \
         test_backup_policy test_ruptela_vectors test_capsule_equivalence \
         test_map_installer test_vehicle_state test_ui_model \
         test_capture_replay test_trajectory_replay \
         test_pairing test_ota_policy test_backup_cfg

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
	./test_capsule_equivalence
	./test_map_installer
	./test_vehicle_state
	./test_ui_model
	./test_pairing
	./test_ota_policy
	./test_backup_cfg
	@for c in $(CAPTURAS); do \
	    echo "--- captura $$c ---"; \
	    ./test_capture_replay $(CURDIR)/build/$$c.bin || exit 1; \
	    ./test_trajectory_replay $(CURDIR)/build/$$c.bin \
	        --golden fixtures/trayectorias/$$c.golden || exit 1; \
	done

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

# La cápsula se genera desde el mismo dataset que usa el replay de equivalencia.
$(CAPSULE_FILE): $(DATASET_ROOT)/tiles
	@mkdir -p $(dir $@)
	cd $(DATASET_ROOT) && python3 pack_capsule.py tiles/ $@ --region "AMBA" \
	    --tile-config $(MATCH_SRC)/include/tile_config.h

test_capsule_equivalence: test_capsule_equivalence.c $(MATCH_SRC)/map_match.c \
                          $(CAPSULE_SRC)/map_capsule.c | $(CAPSULE_FILE)
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) $(DEFINES) \
	    -DCAPSULE_PATH='"$(CAPSULE_FILE)"' -o $@ $^ -lm

# Instalador: inyección de fallas en cada transición persistente.
test_map_installer: test_map_installer.c $(CAPSULE_SRC)/map_installer.c \
                   $(CAPSULE_SRC)/map_capsule.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Pairing: contratos de vinculación y sesión.
test_pairing: test_pairing.c $(PAIR_SRC)/pairing.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Política de OTA A/B.
test_ota_policy: test_ota_policy.c $(OTA_SRC)/ota_policy.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Streams reconstruidos desde las capturas de campo.
$(CURDIR)/build/%.bin: fixtures/capturas/%.log
	@mkdir -p $(dir $@)
	python3 $(REPO_ROOT)/tools/capture_to_stream.py $< $@

# Replay de una captura real: framing, IMEI y records de IO, con invariancia al troceo.
test_capture_replay: test_capture_replay.c $(SHARED_SRC)/nmea_framer.c \
                     $(SHARED_SRC)/imei_scanner.c $(SHARED_SRC)/ruptela_io_parser.c \
                     | $(STREAMS)
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Replay de trayectorias reales contra el sistema de tiles.
test_trajectory_replay: test_trajectory_replay.c $(SHARED_SRC)/nmea_framer.c \
                        $(MATCH_SRC)/map_match.c | $(STREAMS)
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) $(DEFINES) -o $@ $^ -lm

# Outbox de telemetría: pendiente / en vuelo / confirmado.
test_outbox: test_outbox.c $(OUTBOX_SRC)/outbox.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Política de permiso del respaldo.
test_backup_policy: test_backup_policy.c $(OUTBOX_SRC)/backup_policy.c
	$(CC) $(CFLAGS) $(SANITIZE) $(INCLUDES) -o $@ $^

# Texto de /sdcard/backup.cfg: host, puerto y redes.
test_backup_cfg: test_backup_cfg.c $(CFG_SRC)/backup_cfg.c
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
	rm -rf build
	rm -rf $(TESTS:%=%.dSYM)
