export const MQTT_WEBSOCKET_PATH = "/mqtt";

export function buildMqttWebSocketUrl(locationLike = globalThis.location) {
  if (!locationLike || typeof locationLike.host !== "string") {
    throw new TypeError("location with host is required");
  }

  const protocol = locationLike.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${locationLike.host}${MQTT_WEBSOCKET_PATH}`;
}

export function mqttTransportDescriptor(locationLike = globalThis.location) {
  return Object.freeze({
    path: MQTT_WEBSOCKET_PATH,
    url: buildMqttWebSocketUrl(locationLike),
    automaticReconnect: true,
    stage: "DEV-011A-static-foundation",
  });
}

/*
 * DEV-011A deliberately does not open a WebSocket.
 *
 * DEV-011B will extend this module with the MQTT-over-WebSocket transport,
 * subscriptions, reconnect and publish APIs. Keeping URL derivation here now
 * ensures the browser endpoint is based only on the current page hostname and
 * the required local /mqtt path.
 */
