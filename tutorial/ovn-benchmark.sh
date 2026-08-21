#!/bin/bash

DEFAULT_NODES=200

PROCESS_NAME=()
FILE_NAME=""
NODES=""
PROCESS_PIDS=()
FINAL_PEAK_KB=()
FINAL_PEAK_MB=()
DEBUG=false
BATCH_SIZE=""
VALGRIND_MODE=false
VALGRIND_PIDS=()
MASSIF_FILES=()
BENCHMARK_TMPDIR=""

cleanup() {
    for vpid in "${VALGRIND_PIDS[@]}"; do
        kill "$vpid" 2>/dev/null
    done
    if [ -n "$BENCHMARK_TMPDIR" ]; then
        rm -rf "$BENCHMARK_TMPDIR"
    fi
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help|--usage)
            echo "Usage: $0 [OPTIONS] [NODES] [PROCESS...]"
            echo ""
            echo "Arguments:"
            echo "  NODES         Number of nodes to create" \
                 "(default: $DEFAULT_NODES)"
            echo "  PROCESS       Process(es) to track:" \
                 "ovn-northd, ovn-controller"
            echo "                (default: both)"
            echo ""
            echo "Options:"
            echo "  -f, --file FILE       Load NB database from" \
                 "file instead of generating"
            echo "  -b, --batch-size N    Nodes per chassis" \
                 "(default: NODES/10)"
            echo "  -v, --valgrind        Track heap with Valgrind Massif"
            echo "                        (much slower, most accurate)"
            echo "  -d, --debug           Enable debug output"
            echo "  -h, --help            Show this help message"
            echo ""
            echo "Memory tracking:"
            echo "  Default: peak virtual memory (VmPeak) from /proc/<pid>/status,"
            echo "  capturing all allocated memory including pages not yet accessed."
            echo "  --valgrind: peak heap memory from Valgrind Massif, restarting"
            echo "  the tracked processes under valgrind. Expect 10-50x slowdown."
            echo ""
            echo "Examples:"
            echo "  $0                      # 200 nodes, track both processes"
            echo "  $0 50                   # 50 nodes"
            echo "  $0 50 ovn-northd        # 50 nodes, track only ovn-northd"
            echo "  $0 --debug 20           # 20 nodes with debug output"
            echo "  $0 --file ovnnb_db.db   # Load from file"
            echo "  $0 50 -b 10             # 50 nodes, 10 per chassis"
            exit 0
            ;;
        -b|--batch-size)
            BATCH_SIZE="$2"
            shift 2
            ;;
        -v|--valgrind)
            VALGRIND_MODE=true
            shift
            ;;
        -d|--debug)
            DEBUG=true
            shift
            ;;
        -f|--file)
            FILE_NAME="$2"
            shift 2
            ;;
        -*)
            echo "Unknown option: $1"
            exit 1
            ;;
        *)
            if [ -z "$NODES" ]; then
                NODES="$1"
            else
                # Normalize process names: accept both "northd" and
                # "ovn-northd".
                case "$1" in
                    northd)
                        PROCESS_NAME+=("ovn-northd")
                        ;;
                    controller)
                        PROCESS_NAME+=("ovn-controller")
                        ;;
                    *)
                        PROCESS_NAME+=("$1")
                        ;;
                esac
            fi
            shift
            ;;
    esac
done

# Apply defaults if not set by user.
NODES=${NODES:-$DEFAULT_NODES}

if [ -z "$BATCH_SIZE" ]; then
    BATCH_SIZE=$((NODES / 10))
fi
if [ "$BATCH_SIZE" -lt 1 ]; then
    BATCH_SIZE=1
fi

# Track both processes if not specified.
if [ ${#PROCESS_NAME[@]} -eq 0 ]; then
    PROCESS_NAME=("ovn-controller" "ovn-northd")
fi

if [ "$DEBUG" = true ]; then
    echo "Nodes:       $NODES"
    echo "Batch size:  $BATCH_SIZE"
    echo "Processes:   ${PROCESS_NAME[*]}"
    echo "File:        ${FILE_NAME:-None}"
fi

for pn in "${PROCESS_NAME[@]}"; do
    pid=$(pgrep -x "$pn" | head -n 1)
    if [ -z "$pid" ]; then
        echo "Error: Could not find process matching '$pn'"
        exit 1
    fi
    PROCESS_PIDS+=("$pid")
done

if [ "$DEBUG" = true ]; then
    for i in "${!PROCESS_NAME[@]}"; do
        echo "Tracking memory for ${PROCESS_NAME[$i]}" \
             "(PID: ${PROCESS_PIDS[$i]})"
    done
fi

if [ "$VALGRIND_MODE" = true ]; then
    if ! command -v valgrind >/dev/null 2>&1; then
        echo "Error: valgrind is not installed or not in PATH"
        exit 1
    fi

    BENCHMARK_TMPDIR=$(mktemp -d)
    echo "Restarting processes under Valgrind Massif..."
    echo "(Expect 10-50x slowdown)"

    for i in "${!PROCESS_NAME[@]}"; do
        pn="${PROCESS_NAME[$i]}"
        pid="${PROCESS_PIDS[$i]}"

        # Read the original command line (NUL-separated args) into an array.
        mapfile -d '' orig_args < /proc/$pid/cmdline
        [ -z "${orig_args[-1]}" ] && unset 'orig_args[-1]'

        proc_cwd=$(readlink /proc/$pid/cwd)
        massif_file="$BENCHMARK_TMPDIR/massif.$pn.out"
        MASSIF_FILES+=("$massif_file")

        # Strip args that conflict with foreground execution under valgrind.
        filtered_args=()
        skip_next=false
        for arg in "${orig_args[@]}"; do
            if [ "$skip_next" = true ]; then
                skip_next=false
                continue
            fi
            case "$arg" in
                --detach|--no-chdir) ;;
                --pidfile|--pid-file) skip_next=true ;;
                --pidfile=*|--pid-file=*) ;;
                *) filtered_args+=("$arg") ;;
            esac
        done

        if [ "$DEBUG" = true ]; then
            echo "Stopping $pn (PID $pid)..."
        fi

        kill "$pid"
        while [ -e "/proc/$pid" ]; do sleep 0.1; done

        if [ "$DEBUG" = true ]; then
            echo "Starting $pn under valgrind:"
            echo "  valgrind --tool=massif --massif-out-file=$massif_file" \
                 "${filtered_args[*]}"
        fi

        # exec replaces the subshell with valgrind so $! is valgrind's
        # actual PID and SIGTERM reaches it to finalize massif output.
        (cd "$proc_cwd" && exec valgrind \
            --tool=massif \
            --massif-out-file="$massif_file" \
            "${filtered_args[@]}" \
            >/dev/null 2>&1) &
        VALGRIND_PIDS+=("$!")
    done

    echo "Waiting for processes to reconnect..."
    sleep 5
fi

START_TIME=$(date +%s%2N)

if [ "$DEBUG" = true ]; then
    DEBUG_FLAG="-d"
else
    DEBUG_FLAG=""
fi

# Load database from file or generate with Python script.
if [ -n "$FILE_NAME" ]; then
    echo "Loading database from file: $FILE_NAME"
    if [ ! -f "$FILE_NAME" ]; then
        echo "Error: File '$FILE_NAME' not found"
        exit 1
    fi
    ovsdb-client restore unix:$PWD/sandbox/nb1.ovsdb < "$FILE_NAME"
else
    echo "Generating database with Python script"
    python3 ovn-benchmark.py -n $NODES -b $BATCH_SIZE \
        -r unix:$PWD/sandbox/nb1.ovsdb $DEBUG_FLAG
    if [ $? -ne 0 ]; then
        echo "Error: Failed to generate database"
        exit 1
    fi
fi

# Bind the first port of each switch assigned to chassis-0.
for i in $(seq 0 $((BATCH_SIZE - 1))); do
    ovs-vsctl add-port br-int lsp-${i}-0 -- \
        set interface lsp-${i}-0 \
        external_ids:iface-id=lsp-${i}-0
done

# Wait for ovn-controller to claim ports and finish processing.
ovn-nbctl --wait=hv sync

ovs-appctl -t $PWD/sandbox/nb1 ovsdb-server/compact
ovs-appctl -t $PWD/sandbox/sb1 ovsdb-server/compact

END_TIME=$(date +%s%2N)

ELAPSED_TIME=$((END_TIME - START_TIME))
ELAPSED_SECS=$((ELAPSED_TIME / 100))
ELAPSED_HSECS=$((ELAPSED_TIME % 100))

if [ "$VALGRIND_MODE" = true ]; then
    # Stop valgrind processes so they finalize massif output files.
    for vpid in "${VALGRIND_PIDS[@]}"; do
        kill "$vpid" 2>/dev/null
    done
    for vpid in "${VALGRIND_PIDS[@]}"; do
        while [ -e "/proc/$vpid" ]; do sleep 0.1; done
    done

    for i in "${!PROCESS_NAME[@]}"; do
        pn="${PROCESS_NAME[$i]}"
        massif_file="${MASSIF_FILES[$i]}"
        if [ ! -s "$massif_file" ]; then
            echo "Warning: massif output missing for $pn;" \
                 "did valgrind exit cleanly?"
            FINAL_PEAK_KB[$i]=0
        else
            # Sum heap + allocator overhead per snapshot; report the peak.
            FINAL_PEAK_KB[$i]=$(awk -F= '
                /^mem_heap_B=/{h=$2}
                /^mem_heap_extra_B=/{e=$2; t=h+e; if(t>peak) peak=t}
                END{print int(peak/1024)}
            ' "$massif_file")
        fi
        FINAL_PEAK_MB[$i]=$((FINAL_PEAK_KB[$i] / 1024))
    done
else
    for i in "${!PROCESS_NAME[@]}"; do
        pid=${PROCESS_PIDS[$i]}
        FINAL_PEAK_KB[$i]=$(awk '/^VmPeak:/{print $2}' \
            /proc/$pid/status 2>/dev/null)
        if [ -z "${FINAL_PEAK_KB[$i]}" ]; then
            FINAL_PEAK_KB[$i]=0
        fi
        FINAL_PEAK_MB[$i]=$((FINAL_PEAK_KB[$i] / 1024))
    done
fi

echo ""
echo "=== Benchmark Results ==="
printf "Total time:  %d.%02d seconds\n" $ELAPSED_SECS $ELAPSED_HSECS
if [ "$VALGRIND_MODE" = true ]; then
    echo "Peak heap memory (Valgrind Massif):"
else
    echo "Peak virtual memory (VmPeak):"
fi
for i in "${!PROCESS_NAME[@]}"; do
    printf "  %-15s %d MB\n" \
        "${PROCESS_NAME[$i]}:" "${FINAL_PEAK_MB[$i]}"
done
echo "========================="
echo ""
