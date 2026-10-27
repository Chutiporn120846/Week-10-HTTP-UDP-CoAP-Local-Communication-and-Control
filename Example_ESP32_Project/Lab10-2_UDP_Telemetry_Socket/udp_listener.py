import socket

UDP_PORT = 3334

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("", UDP_PORT))

print(f"Listening for UDP Broadcast on port {UDP_PORT}...")

last_seq = None
packet_count = 0
lost_packets = 0

try:
    while True:
        data, addr = sock.recvfrom(128)
        msg = data.decode("utf-8", errors="replace").strip()

        # ตรวจสอบรูปแบบข้อมูลก่อนประมวลผล
        try:
            parts = dict(
                item.split(":", 1)
                for item in msg.split(",")
                if ":" in item
            )

            if "SEQ" not in parts or "POT" not in parts:
                print(f"[IGNORED] Invalid packet from {addr[0]}: {msg!r}")
                continue

            current_seq = int(parts["SEQ"])
            pot_val = int(parts["POT"])

        except (ValueError, TypeError):
            print(f"[IGNORED] Cannot parse packet: {msg!r}")
            continue

        packet_count += 1

        if last_seq is not None:
            diff = current_seq - last_seq

            if diff > 1:
                lost = diff - 1
                lost_packets += lost
                print(
                    f"[PACKET LOSS DETECTED] Lost {lost} packets! "
                    f"(Expected {last_seq + 1}, got {current_seq})"
                )

        last_seq = current_seq

        print(
            f"[{addr[0]}] Seq: {current_seq:<6} | "
            f"Potentiometer: {pot_val:<5} | "
            f"Received: {packet_count} | Lost: {lost_packets}"
        )

except KeyboardInterrupt:
    total_packets = packet_count + lost_packets

    print("\n--- Statistics ---")
    print(f"Received: {packet_count} packets")
    print(f"Lost: {lost_packets} packets")

    if total_packets > 0:
        loss_rate = lost_packets / total_packets * 100
        print(f"Packet Loss Rate: {loss_rate:.2f}%")
    else:
        print("Packet Loss Rate: N/A (no valid packets received)")

finally:
    sock.close()
