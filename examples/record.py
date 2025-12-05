import serial
import struct
import json
import csv
import argparse
import time
import numpy as np
import os
import shutil

# -------------------------
# Update these based on your definition!
# -------------------------
N_CIR_SAMPLES = 128  # <-- CHANGE THIS TO MATCH YOUR FIRMWARE

# cir_sample_t = int16 real, int16 imag
CIR_SAMPLE_FORMAT = "<hh"
CIR_SAMPLE_SIZE = struct.calcsize(CIR_SAMPLE_FORMAT)  # = 4 bytes

# dwt_rxdiag_t = 8 × uint16
RX_DIAG_FORMAT = "<8H"
RX_DIAG_SIZE = struct.calcsize(RX_DIAG_FORMAT)        # = 16 bytes

# float range
RANGE_FORMAT = "<f"
RANGE_SIZE = struct.calcsize(RANGE_FORMAT)            # = 4 bytes

TOTAL_PACKET_SIZE = RANGE_SIZE + RX_DIAG_SIZE + N_CIR_SAMPLES * CIR_SAMPLE_SIZE
# Should equal: 20 + 4*N_CIR_SAMPLES
print("Total packet size:", TOTAL_PACKET_SIZE)

# ---------------------------------------------------------
# Decode a full packet
# ---------------------------------------------------------
def read_packet(ser):
    data = ser.read(TOTAL_PACKET_SIZE)
    if len(data) != TOTAL_PACKET_SIZE:
        print(len(data))
        return None
    

    offset = 0

    # float range
    (range_val,) = struct.unpack_from(RANGE_FORMAT, data, offset)
    offset += RANGE_SIZE

    # 8 × uint16 rx diagnostics
    diagnostics = np.array(
        struct.unpack_from(RX_DIAG_FORMAT, data, offset),
        dtype=np.uint16
    )
    offset += RX_DIAG_SIZE

    # CIR: N samples × (int16,int16)
    cir = np.frombuffer(
        data,
        dtype=np.int16,
        count=N_CIR_SAMPLES * 2,
        offset=offset
    ).reshape((N_CIR_SAMPLES, 2))

    packet = {
        "timestamp": time.time_ns() / 1e3,
        "range": float(range_val),
        "diagnostics": diagnostics,
        "cir": cir,
    }

    print(packet)

    return 

# ----------------------------------------------
# Save as JSON
# ----------------------------------------------
def save_json(filename, packets):
    # Convert numpy → lists
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

# ----------------------------------------------
# Save CSV (range + timestamp only)
# CIR goes in separate CSV
# ----------------------------------------------
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

# ----------------------------------------------
# Main
# ----------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Decode DWM1001 range_packet_t stream")
    parser.add_argument("trial", help = "Trial name")
    parser.add_argument("port", help="COM port (COM4, /dev/ttyUSB0, etc.)")
    args = parser.parse_args()

    BAUD = 115200

    PORT = f"COM{args.port}"
    ser = serial.Serial(PORT, BAUD, timeout=2)
    print(f"Listening on {PORT} at {BAUD}...")
    print("Press CTRL+C to stop and save.")

    packets = []

    try:
        while True:
            pkt = read_packet(ser)
            if pkt:
                packets.append(pkt)
                print(f"Packet {len(packets)}: range={pkt['range']:.3f} m")
            else:
                print("Timeout or partial packet...")
    except KeyboardInterrupt:
        print("\nCTRL+C detected — stopping capture.")
    finally:
        ser.close()

    OUTPATH = f"C:\\Users\\soula\\OneDrive\\Desktop\\Programming\\dwm1001-examples-CIR\\out\\{args.trial}"
    # If directory exists → delete it completely
    if os.path.exists(OUTPATH):
        shutil.rmtree(OUTPATH)

    # Recreate it cleanly
    os.makedirs(OUTPATH)

    # Save outputs after interrupt
    save_json(f"{OUTPATH}\\packets.json", packets)
    save_csv(f"{OUTPATH}\\ranges.csv", packets)
    save_cir_csv(f"{OUTPATH}\\cir", packets)

    print("Saved packets.json, ranges.csv, cir_*.csv")

if __name__ == "__main__":
    main()
