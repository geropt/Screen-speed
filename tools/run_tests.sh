#!/usr/bin/env bash
# Runner único de pruebas del repositorio.
#
# El plan de evolución citaba este archivo como evidencia disponible; no existía. Se
# creó en la etapa de calificación integrada para que haya un solo comando que corra
# todo lo que hoy se puede correr sin hardware, y para que la CI y una persona
# ejecuten exactamente lo mismo.
#
# Uso:
#   tools/run_tests.sh              todo lo que no necesita hardware
#   tools/run_tests.sh --with-build  además compila el firmware con ESP-IDF
#   tools/run_tests.sh --list        muestra qué haría, sin ejecutar
#
# Códigos de salida: 0 todo verde, 1 alguna suite falló, 2 problema de entorno.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

WITH_BUILD=0
LIST_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --with-build) WITH_BUILD=1 ;;
        --list)       LIST_ONLY=1 ;;
        -h|--help)
            sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "argumento no reconocido: $arg" >&2
            exit 2
            ;;
    esac
done

# ---------- salida ----------

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_FAIL=$'\033[31m'; C_SKIP=$'\033[33m'; C_OFF=$'\033[0m'
else
    C_OK=""; C_FAIL=""; C_SKIP=""; C_OFF=""
fi

PASSED=0
FAILED=0
SKIPPED=0
FAILED_NAMES=()

run_suite() {
    local name="$1"; shift
    if [ "$LIST_ONLY" -eq 1 ]; then
        echo "  $name: $*"
        return 0
    fi
    printf '\n=== %s ===\n' "$name"
    if "$@"; then
        printf '%s[OK]%s %s\n' "$C_OK" "$C_OFF" "$name"
        PASSED=$((PASSED + 1))
    else
        printf '%s[FALLA]%s %s\n' "$C_FAIL" "$C_OFF" "$name"
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$name")
    fi
}

skip_suite() {
    local name="$1"
    local why="$2"
    if [ "$LIST_ONLY" -eq 1 ]; then
        echo "  $name: OMITIDA ($why)"
        return 0
    fi
    printf '\n=== %s ===\n' "$name"
    printf '%s[OMITIDA]%s %s\n' "$C_SKIP" "$C_OFF" "$why"
    SKIPPED=$((SKIPPED + 1))
}

# ---------- comprobaciones de entorno ----------

need() {
    command -v "$1" >/dev/null 2>&1
}

if ! need cc && ! need gcc; then
    echo "no hay compilador de C disponible (cc/gcc)" >&2
    exit 2
fi
if ! need make; then
    echo "make no está disponible" >&2
    exit 2
fi
if ! need python3; then
    echo "python3 no está disponible" >&2
    exit 2
fi

# ---------- suites ----------

if [ "$LIST_ONLY" -eq 1 ]; then
    echo "Suites que se ejecutarían:"
fi

# 1. Pruebas del HUD en host. Incluyen el replay de equivalencia del matcher contra el
#    golden congelado en P00 y la equivalencia de la cápsula, que son las dos puertas
#    de aceptación de las extracciones.
#    La cápsula la genera el propio Makefile desde el dataset.
if [ -d python/tiles ]; then
    run_suite "HUD host_tests (incluye replay de equivalencia y cápsula)" \
        make -C esp/Firmware/host_tests test
else
    # Sin dataset no se pueden correr los replays; el resto sí, pero el Makefile los
    # exige, así que se informa en lugar de dar un verde parcial engañoso.
    skip_suite "HUD host_tests" \
        "falta python/tiles; regenerar con python/extract_tiles.py"
fi

# 2. Pruebas del piloto Starlink. Compilan desde el componente compartido.
run_suite "Piloto Starlink host_tests" \
    make -C esp/starlink_pilot/host_tests test

# 3. Herramientas Python: que el empaquetador de cápsulas al menos importe y responda
#    --help. No hay suite de Python en el repositorio; decirlo es mejor que sugerir
#    una cobertura que no existe.
if [ -f python/pack_capsule.py ]; then
    run_suite "Herramientas Python (import y --help)" \
        python3 python/pack_capsule.py --help
fi

# 4. Build del firmware. Opcional porque necesita el entorno ESP-IDF instalado.
IDF_ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v5.4.4.sh}"
if [ "$WITH_BUILD" -eq 1 ]; then
    if [ -f "$IDF_ACTIVATE" ]; then
        build_firmware() {
            # shellcheck disable=SC1090
            . "$IDF_ACTIVATE" >/dev/null 2>&1 || return 1
            # Directorio y sdkconfig aparte: no se toca el build ni la configuración
            # del usuario. El directorio se crea antes porque idf.py no lo hace para
            # el archivo de sdkconfig, sólo para el de build.
            mkdir -p esp/Firmware/build_ci || return 1
            ( cd esp/Firmware && \
              python "$IDF_PATH/tools/idf.py" -B build_ci \
                  -D SDKCONFIG="build_ci/sdkconfig.ci" build )
        }
        run_suite "Build del firmware (ESP-IDF)" build_firmware
    else
        skip_suite "Build del firmware" \
            "no se encontró $IDF_ACTIVATE; exportar IDF_ACTIVATE con la ruta correcta"
    fi
else
    skip_suite "Build del firmware" "no pedido; usar --with-build"
fi

# ---------- resumen ----------

if [ "$LIST_ONLY" -eq 1 ]; then
    exit 0
fi

printf '\n========================================\n'
printf 'Resumen: %s%d en verde%s' "$C_OK" "$PASSED" "$C_OFF"
if [ "$FAILED" -gt 0 ]; then
    printf ', %s%d con fallas%s' "$C_FAIL" "$FAILED" "$C_OFF"
fi
if [ "$SKIPPED" -gt 0 ]; then
    printf ', %s%d omitidas%s' "$C_SKIP" "$SKIPPED" "$C_OFF"
fi
printf '\n'

if [ "$FAILED" -gt 0 ]; then
    printf 'Fallaron: %s\n' "${FAILED_NAMES[*]}"
    printf '========================================\n'
    exit 1
fi

# Recordatorio honesto: verde acá NO significa que el producto funcione.
cat <<'EOF'

Lo que este verde NO significa:
  - No se flasheó ninguna placa. Memoria, latencia, colores, fluidez y consumo
    siguen sin medir.
  - No se probó contra un Ruptela real; hace falta banco con IMEI propio.
  - No hay pruebas de corte físico de alimentación en la tarjeta.
  - Los tiempos del host no son comparables con los del dispositivo.
Detalle en esp/Firmware/docs/p11-calificacion.md
EOF
printf '========================================\n'
exit 0
