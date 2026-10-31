import struct
import csv

INPUT_FILE = "/tmp/page_window_rate_events.bin"
OUTPUT_FILE = "page_events.csv"

# Layout C x86-64:
# u64 timestamp_ns
# pid_t pid (32 bits)
# pid_t tgid (32 bits)
# u64 page_id
# char event_type
# char* comm
#
# O padding final completa os 24 bytes da estrutura.

EVENT_FORMAT = "@QiiQb16s7x"
EVENT_SIZE = struct.calcsize(EVENT_FORMAT)

with open(INPUT_FILE, "rb") as binary_file, \
     open(OUTPUT_FILE, "w", newline="") as csv_file:

    writer = csv.writer(csv_file)
    writer.writerow([
        "timestamp_ns",
        "pid",
        "tgid",
        "page_id",
        "event_type",
        "comm",
    ])

    print(f"EVENT SIZE: {EVENT_SIZE}")

    while True:
        data = binary_file.read(EVENT_SIZE)

        if not data:
            break

        if len(data) != EVENT_SIZE:
            raise ValueError(
                f"Registro incompleto: {len(data)} bytes"
            )

        timestamp_ns, pid, tgid, page_id, event_type, comm = (
            struct.unpack(EVENT_FORMAT, data)
        )

        writer.writerow([
            timestamp_ns,
            pid,
            tgid,
            page_id,
            chr(event_type),
            comm.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        ])

print(f"Conversão concluída: {OUTPUT_FILE}")
