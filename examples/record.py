import pylink
import struct
import json
import csv
import argparse
import time
import numpy as np
import os
import shutil

# Boost Python process priority on Windows
p = psutil.Process(os.getpid())
p.nice(psutil.HIGH_PRIORITY_CLASS)

# ------------------------- Firmware-specific parameters -------------------------
N_CIR_SAMPLES = 128
TOTAL_PACKET_SIZE = 532

CIR_SAMPLE_FORMAT = "<hh"
CIR_SAMPLE_SIZE = struct.calcsize(CIR_SAMPLE_FORMAT)

RX_DIAG_FORMAT = "<8H"
RX_DIAG_SIZE = struct.calcsize(RX_DIAG_FORMAT)

RANGE_FORMAT = "<f"
RANGE_SIZE = struct.calcsize(RANGE_FORMAT)

print("Total packet size:", TOTAL_PACKET_SIZE)

# ------------------------- Helper functions -------------------------
def read_full_packet(jlink, size, channel=0):
    """Read exactly 'size' bytes from RTT Up buffer."""
    data = bytearray()
    while len(data) < size:
        chunk = jlink.rtt_read(channel, size - len(data))
        if not chunk:
            time.sleep(0.001)
            continue
        data.extend(chunk)
    return data

def parse_packet(data):
    offset = 0
    (range_val,) = struct.unpack_from(RANGE_FORMAT, data, offset)
    offset += RANGE_SIZE

    diagnostics = np.array(
        struct.unpack_from(RX_DIAG_FORMAT, data, offset),
        dtype=np.uint16
    )
    offset += RX_DIAG_SIZE

    cir = np.frombuffer(
        data,
        dtype=np.int16,
        count=N_CIR_SAMPLES * 2,
        offset=offset
    ).reshape((N_CIR_SAMPLES, 2))

    return {
        "timestamp": time.time_ns() / 1e3,
        "range": float(range_val),
        "diagnostics": diagnostics,
        "cir": cir,
    }

def save_json(filename, packets):
    json_data = []
    for p in packets:
        json_data.append({
            "timestamp": p["timestamp"],
            "range": p["range"],
            "diagnostics": p["diagnostics"].tolist(),
            "cir": p["cir"].tolist(),
        })
    with open(filename, "w") as f:
        json.dump(json_data, f, indent=2)

def save_csv(filename, packets):
    with open(filename, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp", "range"])
        for p in packets:
            writer.writerow([p["timestamp"], p["range"]])

def save_cir_csv(prefix, packets):
    for idx, p in enumerate(packets):
        cir_file = f"{prefix}_cir_{idx}.csv"
        np.savetxt(cir_file, p["cir"], delimiter=",", fmt="%d")

# ------------------------- Main -------------------------
def main():
    parser = argparse.ArgumentParser(description="Decode DWM1001 range_packet_t stream via PyLink RTT")
    parser.add_argument("trial", help="Trial name")
    parser.add_argument("--serial", type=int, default=None, help="J-Link serial number")
    args = parser.parse_args()

    print("Connecting to J-Link...")
    jlink = pylink.JLink()
    if args.serial:
        jlink.open(serial_no=args.serial)
    else:
        jlink.open()
    print(f"Connected: {jlink.product_name} ({jlink.serial_number})")

    jlink.rtt_start()
    print("RTT started.")

    packets = []
    try:
        while True:
            data = read_full_packet(jlink, TOTAL_PACKET_SIZE)
            packet = parse_packet(data)
            packets.append(packet)
            print(f"Packet {len(packets)}: range={packet['range']:.3f} m")
    except KeyboardInterrupt:
        print("\nCTRL+C detected — stopping capture.")
    finally:
        jlink.rtt_stop()
        jlink.close()

    OUTPATH = os.path.join("examples\\out", args.trial)
    if os.path.exists(OUTPATH):
        shutil.rmtree(OUTPATH)
    os.makedirs(OUTPATH)

    save_json(os.path.join(OUTPATH, "packets.json"), packets)
    save_csv(os.path.join(OUTPATH, "ranges.csv"), packets)
    save_cir_csv(os.path.join(OUTPATH, "cir"), packets)

    print("Saved packets.json, ranges.csv, cir_*.csv")

if __name__ == "__main__":
    main()
