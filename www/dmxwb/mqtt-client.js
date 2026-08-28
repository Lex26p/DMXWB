export const MQTT_WEBSOCKET_PATH = "/mqtt";
export const MQTT_PROTOCOL_NAME = "mqtt";
export const MQTT_CONFIG_TOPIC = "/dmxwb/config";
export const MQTT_STATE_TOPIC = "/dmxwb/state";
export const MQTT_STATUS_TOPIC = "/dmxwb/status";
export const MQTT_SYSTEM_SOURCE_COMMAND_TOPIC =
  "/devices/dmxwb/controls/source/on";
const LIVE_DEVICE_CONTROLS = new Set([
  "name",
  "power",
  "red",
  "green",
  "blue",
  "color",
  "brightness",
  "temperature",
  "reset",
]);

const GROUP_STATE_CONTROLS = Object.freeze([
  "name",
  "power",
  "red",
  "green",
  "blue",
  "color",
  "brightness",
  "temperature",
]);

function normalizePositiveId(value, label) {
  const id = Number(value);
  if (!Number.isSafeInteger(id) || id <= 0) {
    throw new RangeError(`${label} id must be a positive safe integer`);
  }
  return id;
}

function liveDeviceCommandTopic(prefix, idValue, control, label) {
  const id = normalizePositiveId(idValue, label);
  if (!LIVE_DEVICE_CONTROLS.has(control)) {
    throw new RangeError(`unsupported ${label} control: ${control}`);
  }
  return `/devices/${prefix}_${id}/controls/${control}/on`;
}

// Fixture command shape: /devices/dmxwb_fixture_${id}/controls/${control}/on
export function fixtureCommandTopic(fixtureId, control) {
  return liveDeviceCommandTopic(
    "dmxwb_fixture",
    fixtureId,
    control,
    "Fixture",
  );
}

export function groupCommandTopic(groupId, control) {
  return liveDeviceCommandTopic(
    "dmxwb_group",
    groupId,
    control,
    "Group",
  );
}

export function groupStateTopics(groupIdValue) {
  const groupId = normalizePositiveId(groupIdValue, "Group");
  return GROUP_STATE_CONTROLS.map(
    (control) => `/devices/dmxwb_group_${groupId}/controls/${control}`,
  );
}

export function parseGroupStateTopic(topic) {
  const match =
    /^\/devices\/dmxwb_group_([1-9][0-9]*)\/controls\/(name|power|red|green|blue|color|brightness|temperature)$/.exec(
      String(topic),
    );
  if (!match) {
    return null;
  }

  const id = Number(match[1]);
  if (!Number.isSafeInteger(id) || id <= 0) {
    return null;
  }

  return Object.freeze({
    groupId: id,
    control: match[2],
  });
}

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder("utf-8", { fatal: true });

function encodeUtf8String(value) {
  const encoded = textEncoder.encode(String(value));
  if (encoded.length > 0xffff) {
    throw new RangeError("MQTT UTF-8 string is too long");
  }

  const output = new Uint8Array(encoded.length + 2);
  output[0] = (encoded.length >> 8) & 0xff;
  output[1] = encoded.length & 0xff;
  output.set(encoded, 2);
  return output;
}

function concatenate(parts) {
  const size = parts.reduce((total, part) => total + part.length, 0);
  const output = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.length;
  }
  return output;
}

function encodeRemainingLength(length) {
  if (!Number.isInteger(length) || length < 0 || length > 268435455) {
    throw new RangeError("invalid MQTT remaining length");
  }

  const bytes = [];
  let value = length;
  do {
    let encoded = value % 128;
    value = Math.floor(value / 128);
    if (value > 0) {
      encoded |= 0x80;
    }
    bytes.push(encoded);
  } while (value > 0);

  return Uint8Array.from(bytes);
}

function encodePacket(header, body = new Uint8Array()) {
  return concatenate([
    Uint8Array.of(header),
    encodeRemainingLength(body.length),
    body,
  ]);
}

function decodeRemainingLength(bytes, offset) {
  let multiplier = 1;
  let value = 0;
  let used = 0;

  while (used < 4) {
    if (offset + used >= bytes.length) {
      throw new Error("truncated MQTT remaining length");
    }

    const current = bytes[offset + used];
    value += (current & 0x7f) * multiplier;
    used += 1;

    if ((current & 0x80) === 0) {
      return { value, used };
    }

    multiplier *= 128;
  }

  throw new Error("malformed MQTT remaining length");
}

function decodeUtf8String(bytes, offset) {
  if (offset + 2 > bytes.length) {
    throw new Error("truncated MQTT UTF-8 length");
  }

  const length = (bytes[offset] << 8) | bytes[offset + 1];
  const start = offset + 2;
  const end = start + length;
  if (end > bytes.length) {
    throw new Error("truncated MQTT UTF-8 string");
  }

  return {
    value: textDecoder.decode(bytes.subarray(start, end)),
    nextOffset: end,
  };
}

export function buildMqttWebSocketUrl(locationLike = globalThis.location) {
  if (!locationLike || typeof locationLike.host !== "string" || !locationLike.host) {
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
  });
}

export function encodeConnectPacket(clientId, keepAliveSeconds = 30) {
  const keepAlive = Math.max(1, Math.min(0xffff, Number(keepAliveSeconds) || 30));
  const variableHeader = concatenate([
    encodeUtf8String("MQTT"),
    Uint8Array.of(
      0x04, // MQTT 3.1.1
      0x02, // clean session, no auth/will
      (keepAlive >> 8) & 0xff,
      keepAlive & 0xff,
    ),
  ]);
  const payload = encodeUtf8String(clientId);
  return encodePacket(0x10, concatenate([variableHeader, payload]));
}

export function encodeSubscribePacket(packetId, topics) {
  if (!Number.isInteger(packetId) || packetId < 1 || packetId > 0xffff) {
    throw new RangeError("invalid MQTT packet id");
  }

  const topicList = [...new Set(topics.map(String))].filter(Boolean);
  if (topicList.length === 0) {
    throw new Error("at least one MQTT topic is required");
  }

  const payloadParts = [];
  for (const topic of topicList) {
    payloadParts.push(encodeUtf8String(topic), Uint8Array.of(0x00));
  }

  const body = concatenate([
    Uint8Array.of((packetId >> 8) & 0xff, packetId & 0xff),
    ...payloadParts,
  ]);
  return encodePacket(0x82, body);
}

export function encodePublishPacket(topic, payload, retain = false) {
  const body = concatenate([
    encodeUtf8String(topic),
    textEncoder.encode(String(payload)),
  ]);
  return encodePacket(retain ? 0x31 : 0x30, body);
}

export function decodeMqttPackets(data) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  const packets = [];
  let offset = 0;

  while (offset < bytes.length) {
    const header = bytes[offset++];
    const remaining = decodeRemainingLength(bytes, offset);
    offset += remaining.used;

    const end = offset + remaining.value;
    if (end > bytes.length) {
      throw new Error("truncated MQTT packet");
    }

    packets.push({
      type: header >> 4,
      flags: header & 0x0f,
      body: bytes.slice(offset, end),
    });
    offset = end;
  }

  return packets;
}

function makeClientId() {
  const randomPart = globalThis.crypto?.getRandomValues
    ? Array.from(globalThis.crypto.getRandomValues(new Uint32Array(2)))
        .map((value) => value.toString(16).padStart(8, "0"))
        .join("")
    : Math.random().toString(16).slice(2);
  return `dmxwb-web-${randomPart}`.slice(0, 48);
}

export class MqttWebSocketClient {
  constructor({
    url,
    clientId = makeClientId(),
    keepAliveSeconds = 30,
    reconnectMinMs = 500,
    reconnectMaxMs = 10000,
    webSocketFactory = null,
    onConnectionChange = () => {},
    onMessage = () => {},
    onProtocolError = () => {},
  }) {
    if (!url) {
      throw new TypeError("MQTT WebSocket URL is required");
    }

    this.url = String(url);
    this.clientId = String(clientId);
    this.keepAliveSeconds = Math.max(5, Number(keepAliveSeconds) || 30);
    this.reconnectMinMs = Math.max(100, Number(reconnectMinMs) || 500);
    this.reconnectMaxMs = Math.max(
      this.reconnectMinMs,
      Number(reconnectMaxMs) || 10000,
    );
    this.webSocketFactory =
      webSocketFactory ??
      ((socketUrl, protocols) => new WebSocket(socketUrl, protocols));
    this.onConnectionChange = onConnectionChange;
    this.onMessage = onMessage;
    this.onProtocolError = onProtocolError;

    this.running = false;
    this.connected = false;
    this.socket = null;
    this.reconnectTimer = null;
    this.keepAliveTimer = null;
    this.reconnectAttempt = 0;
    this.packetId = 1;
    this.subscriptions = new Set();
    this.awaitingPingResponse = false;
  }

  start() {
    if (this.running) {
      return;
    }
    this.running = true;
    this.reconnectAttempt = 0;
    this.#connect("connecting");
  }

  stop() {
    this.running = false;
    this.connected = false;
    this.#clearReconnectTimer();
    this.#clearKeepAliveTimer();

    const socket = this.socket;
    this.socket = null;
    if (socket && socket.readyState === 1) {
      try {
        socket.send(encodePacket(0xe0));
      } catch {
        // Best-effort DISCONNECT only.
      }
    }
    if (socket && socket.readyState < 2) {
      socket.close();
    }

    this.#emitConnection("offline", false);
  }

  subscribe(topics) {
    const values = Array.isArray(topics) ? topics : [topics];
    const added = [];

    for (const topic of values) {
      const normalized = String(topic);
      if (!normalized || this.subscriptions.has(normalized)) {
        continue;
      }
      this.subscriptions.add(normalized);
      added.push(normalized);
    }

    if (this.connected && added.length > 0) {
      this.#sendSubscriptions(added);
    }
  }

  publish(topic, payload, { retain = false } = {}) {
    if (!this.connected || !this.socket || this.socket.readyState !== 1) {
      return false;
    }

    this.socket.send(encodePublishPacket(topic, payload, retain));
    return true;
  }

  #connect(state) {
    if (!this.running || this.socket) {
      return;
    }

    this.#emitConnection(state, false);

    let socket;
    try {
      socket = this.webSocketFactory(this.url, [MQTT_PROTOCOL_NAME]);
    } catch (error) {
      this.#protocolError(error);
      this.#scheduleReconnect();
      return;
    }

    this.socket = socket;
    socket.binaryType = "arraybuffer";

    socket.onopen = () => {
      if (socket !== this.socket || !this.running) {
        return;
      }
      try {
        socket.send(encodeConnectPacket(this.clientId, this.keepAliveSeconds));
      } catch (error) {
        this.#protocolError(error);
        socket.close();
      }
    };

    socket.onmessage = (event) => {
      if (socket !== this.socket || !this.running) {
        return;
      }

      try {
        for (const packet of decodeMqttPackets(event.data)) {
          this.#handlePacket(packet);
        }
      } catch (error) {
        this.#protocolError(error);
        socket.close();
      }
    };

    socket.onerror = () => {
      if (socket === this.socket && socket.readyState < 2) {
        socket.close();
      }
    };

    socket.onclose = () => {
      if (socket !== this.socket) {
        return;
      }

      this.socket = null;
      this.connected = false;
      this.awaitingPingResponse = false;
      this.#clearKeepAliveTimer();

      if (this.running) {
        this.#scheduleReconnect();
      }
    };
  }

  #handlePacket(packet) {
    switch (packet.type) {
      case 2:
        this.#handleConnAck(packet);
        return;
      case 3:
        this.#handlePublish(packet);
        return;
      case 9:
        return; // SUBACK
      case 13:
        this.awaitingPingResponse = false;
        return; // PINGRESP
      default:
        return;
    }
  }

  #handleConnAck(packet) {
    if (packet.body.length !== 2) {
      throw new Error("invalid MQTT CONNACK length");
    }
    if (packet.body[1] !== 0) {
      throw new Error(`MQTT CONNACK refused with code ${packet.body[1]}`);
    }

    this.connected = true;
    this.reconnectAttempt = 0;
    this.awaitingPingResponse = false;
    this.#emitConnection("connected", true);
    this.#sendSubscriptions();
    this.#startKeepAlive();
  }

  #handlePublish(packet) {
    const qos = (packet.flags >> 1) & 0x03;
    if (qos !== 0) {
      throw new Error("DMXWB web supports incoming MQTT QoS 0 only");
    }

    const topic = decodeUtf8String(packet.body, 0);
    const payload = textDecoder.decode(packet.body.subarray(topic.nextOffset));
    this.onMessage(topic.value, payload, {
      retained: (packet.flags & 0x01) !== 0,
      duplicate: (packet.flags & 0x08) !== 0,
    });
  }

  #sendSubscriptions(topics = [...this.subscriptions]) {
    if (!this.connected || !this.socket || this.socket.readyState !== 1) {
      return;
    }

    const requested = [...new Set(topics.map(String))].filter(Boolean);
    if (requested.length === 0) {
      return;
    }

    this.socket.send(
      encodeSubscribePacket(this.#nextPacketId(), requested),
    );
  }

  #nextPacketId() {
    const value = this.packetId;
    this.packetId = this.packetId >= 0xffff ? 1 : this.packetId + 1;
    return value;
  }

  #startKeepAlive() {
    this.#clearKeepAliveTimer();
    const intervalMs = Math.max(1000, Math.floor(this.keepAliveSeconds * 500));

    this.keepAliveTimer = globalThis.setInterval(() => {
      if (!this.connected || !this.socket || this.socket.readyState !== 1) {
        return;
      }

      if (this.awaitingPingResponse) {
        this.socket.close();
        return;
      }

      try {
        this.socket.send(encodePacket(0xc0));
        this.awaitingPingResponse = true;
      } catch (error) {
        this.#protocolError(error);
        this.socket.close();
      }
    }, intervalMs);
  }

  #scheduleReconnect() {
    if (!this.running || this.reconnectTimer) {
      return;
    }

    this.reconnectAttempt += 1;
    const exponential =
      this.reconnectMinMs * Math.pow(2, Math.min(this.reconnectAttempt - 1, 6));
    const delay = Math.min(this.reconnectMaxMs, exponential);
    this.#emitConnection("reconnecting", false);

    this.reconnectTimer = globalThis.setTimeout(() => {
      this.reconnectTimer = null;
      this.#connect("reconnecting");
    }, delay);
  }

  #emitConnection(state, connected) {
    this.onConnectionChange({
      state,
      connected,
      url: this.url,
      attempt: this.reconnectAttempt,
    });
  }

  #protocolError(error) {
    this.onProtocolError(error instanceof Error ? error : new Error(String(error)));
  }

  #clearReconnectTimer() {
    if (this.reconnectTimer !== null) {
      globalThis.clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  #clearKeepAliveTimer() {
    if (this.keepAliveTimer !== null) {
      globalThis.clearInterval(this.keepAliveTimer);
      this.keepAliveTimer = null;
    }
  }
}
