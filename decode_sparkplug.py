import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Error: 'paho-mqtt' library is missing. Install it using: pip install paho-mqtt")
    sys.exit(1)

try:
    # pip install sparkplug-b
    from sparkplug_b import sparkplug_b_pb2
except ImportError:
    print("Error: 'sparkplug-b' library is missing. Install it using: pip install sparkplug-b")
    sys.exit(1)

BROKER = "192.168.1.13"
PORT = 1883
TOPIC = "spBv1.0/#"

def on_connect(client, userdata, flags, rc):
    print(f"[*] Connected to MQTT Broker {BROKER}:{PORT} with result code {rc}")
    client.subscribe(TOPIC)
    print(f"[*] Subscribed to {TOPIC}. Waiting for messages...\n")

def on_message(client, userdata, msg):
    print("=" * 60)
    print(f"[Topic] {msg.topic}")
    print(f"[Time]  {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("-" * 60)
    
    payload = sparkplug_b_pb2.Payload()
    try:
        payload.ParseFromString(msg.payload)
        
        # Print global message sequence if present
        if payload.HasField("seq"):
            print(f"Sequence: {payload.seq}")
            
        print("Metrics:")
        for metric in payload.metrics:
            val = None
            if metric.HasField("float_value"):
                val = metric.float_value
            elif metric.HasField("double_value"):
                val = metric.double_value
            elif metric.HasField("int_value"):
                val = metric.int_value
            elif metric.HasField("long_value"):
                val = metric.long_value
            elif metric.HasField("boolean_value"):
                val = metric.boolean_value
            elif metric.HasField("string_value"):
                val = metric.string_value
            
            # Print metric name and value
            print(f"  - {metric.name:<35} = {val}")
            
    except Exception as e:
        print(f"[ERROR] Failed to parse Sparkplug B Protobuf payload: {e}")
        print(f"Raw Bytes (hex): {msg.payload.hex()}")
    print("=" * 60 + "\n")

def main():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    
    try:
        client.connect(BROKER, PORT, 60)
    except Exception as e:
        print(f"Failed to connect to broker at {BROKER}: {e}")
        sys.exit(1)
        
    client.loop_forever()

if __name__ == "__main__":
    main()
