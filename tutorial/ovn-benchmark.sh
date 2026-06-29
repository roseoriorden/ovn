#!/bin/bash

DEFAULT_NODES=200

PROCESS_NAME=()
FILE_NAME=""
NODES=""
PROCESS_PIDS=()
CURRENT_MEM=()
PEAK_MEM=()
FINAL_PEAK_KB=()
FINAL_PEAK_MB=()
DEBUG=false

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
            echo "  -f, --file FILE    Load NB database from file" \
                 "instead of generating"
            echo "  -d, --debug        Enable debug output"
            echo "  -h, --help         Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                      # 200 nodes, track both processes"
            echo "  $0 50                   # 50 nodes"
            echo "  $0 50 ovn-northd        # 50 nodes, track only ovn-northd"
            echo "  $0 --debug 20           # 20 nodes with debug output"
            echo "  $0 --file ovnnb_db.db   # Load from file"
            exit 0
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

# Apply default if not set by user.
NODES=${NODES:-$DEFAULT_NODES}

# Track both processes if not specified.
if [ ${#PROCESS_NAME[@]} -eq 0 ]; then
    PROCESS_NAME=("ovn-controller" "ovn-northd")
fi

if [ "$DEBUG" = true ]; then
    echo "Nodes:       $NODES"
    echo "Processes:   ${PROCESS_NAME[*]}"
    echo "File:        ${FILE_NAME:-None}"
fi

for pn in ${PROCESS_NAME[@]}; do
    PROCESS_PIDS+=($(pgrep -f "$pn" | head -n 1))
done

for pid in ${PROCESS_PIDS[@]}; do
    if [ -z "$pid" ]; then
        echo "Error: Could not find process matching '$pid'"
        exit 1
    fi
done

if [ "$DEBUG" = true ]; then
    for i in "${!PROCESS_NAME[@]}"; do
        echo "Tracking memory for ${PROCESS_NAME[$i]}" \
             "(PID: ${PROCESS_PIDS[$i]})"
    done
fi

# Create a temporary file to store the highest memory value we see.
for pn in "${PROCESS_NAME[@]}"; do
    echo 0 > peak_mem_$pn.txt
done

# Start the background "Watcher" loop.
while true; do
    for i in "${!PROCESS_NAME[@]}"; do
        pn="${PROCESS_NAME[$i]}"
        pid="${PROCESS_PIDS[$i]}"

        # Get the Resident Set Size (RSS) memory in KB.
        CURRENT_MEM[$i]=$(ps -p $pid -o rss= 2>/dev/null)

        # If the process died, break.
        if [ -z "${CURRENT_MEM[$i]}" ]; then break; fi

        PEAK_MEM[$i]=$(cat peak_mem_$pn.txt)

        if [ "${CURRENT_MEM[$i]}" -gt "${PEAK_MEM[$i]}" ]; then
            echo "${CURRENT_MEM[$i]}" > peak_mem_$pn.txt
        fi
    done

    sleep 0.5
done &

WATCHER_PID=$!

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
        kill $WATCHER_PID 2>/dev/null
        exit 1
    fi
    ovsdb-client restore unix:$PWD/sandbox/nb1.ovsdb < "$FILE_NAME"
else
    echo "Generating database with Python script"
    python ovn-benchmark.py -n $NODES \
        -r unix:$PWD/sandbox/nb1.ovsdb $DEBUG_FLAG
    if [ $? -ne 0 ]; then
        echo "Error: Failed to generate database"
        kill $WATCHER_PID 2>/dev/null
        exit 1
    fi
fi

# Bind a port from the first LS locally.
ovs-vsctl add-port br-int lsp-1 -- \
    set interface lsp-1 external_ids:iface-id=lsp-1

ovs-appctl -t $PWD/sandbox/nb1 ovsdb-server/compact
ovs-appctl -t $PWD/sandbox/sb1 ovsdb-server/compact

END_TIME=$(date +%s%2N)

kill $WATCHER_PID 2>/dev/null

ELAPSED_TIME=$((END_TIME - START_TIME))
SECONDS=$((ELAPSED_TIME / 100))
HUNDREDTHS=$((ELAPSED_TIME % 100))

for i in "${!PROCESS_NAME[@]}"; do
    pn=${PROCESS_NAME[$i]}
    FINAL_PEAK_KB[$i]=$(cat peak_mem_$pn.txt)
    FINAL_PEAK_MB[$i]=$((FINAL_PEAK_KB[$i] / 1024))
done

echo ""
echo "=== Benchmark Results ==="
printf "Total time:                  %d.%02d seconds\n" \
    $SECONDS $HUNDREDTHS

for i in "${!PROCESS_NAME[@]}"; do
    printf "%-28s %s MB\n" \
        "${PROCESS_NAME[$i]} peak memory:" "${FINAL_PEAK_MB[$i]}"
done
echo "========================="
echo ""

for pn in "${PROCESS_NAME[@]}"; do
    rm peak_mem_$pn.txt
done
