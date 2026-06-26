const config = window.FLIP_TIMER_MQTT_CONFIG || {};

const els = {
  cards: [...document.querySelectorAll(".flip-card")],
  status: document.querySelector("#connectionStatus"),
  stage: document.querySelector(".timer-stage"),
  title: document.querySelector("#timerTitle"),
  message: document.querySelector("#timerMessage"),
  fullscreenButton: document.querySelector("#fullscreenButton")
};

const state = {
  client: null,
  running: false,
  startedAt: 0,
  elapsedBeforeRun: 0,
  lastDigits: "000000",
  animationId: 0
};

function mqttUrl() {
  return config.MQTT_WS_URL || `wss://${config.DEFAULT_MQTT_HOST || "broker.emqx.io"}:8084/mqtt`;
}

function mqttTopic() {
  return config.TOPIC || "jj/ble/button/OY9LRGbg";
}

function formatElapsed(ms) {
  const totalMs = Math.max(0, Math.floor(ms));
  const minutes = Math.floor(totalMs / 60000) % 100;
  const seconds = Math.floor(totalMs / 1000) % 60;
  const centiseconds = Math.floor((totalMs % 1000) / 10);
  return `${String(minutes).padStart(2, "0")}${String(seconds).padStart(2, "0")}${String(centiseconds).padStart(2, "0")}`;
}

function currentElapsed() {
  if (!state.running) return state.elapsedBeforeRun;
  return state.elapsedBeforeRun + performance.now() - state.startedAt;
}

function setStatus(mode, text) {
  els.status.classList.toggle("online", mode === "online");
  els.status.classList.toggle("error", mode === "error");
  els.status.textContent = text;
}

function setMode(mode, text) {
  els.stage.classList.toggle("running", mode === "running");
  els.stage.classList.toggle("finished", mode === "finished");
  els.message.textContent = text;
}

function renderDigits(force = false) {
  const digits = formatElapsed(currentElapsed());
  els.cards.forEach((card, index) => {
    const nextDigit = digits[index] || "0";
    const digit = card.querySelector(".flip-digit");
    if (force || digit.textContent !== nextDigit) {
      digit.textContent = nextDigit;
      card.classList.remove("changing");
      void card.offsetWidth;
      card.classList.add("changing");
    }
  });
  state.lastDigits = digits;
}

function frame() {
  renderDigits();
  state.animationId = requestAnimationFrame(frame);
}

function startNewTiming() {
  state.elapsedBeforeRun = 0;
  state.startedAt = performance.now();
  state.running = true;
  renderDigits(true);
  setMode("running", "Timing");
}

function stopTiming() {
  state.elapsedBeforeRun = currentElapsed();
  state.running = false;
  renderDigits(true);
  setMode("finished", "Finished");
}

function toggleTiming() {
  if (state.running) {
    stopTiming();
  } else {
    startNewTiming();
  }
}

function parsePayload(message) {
  try {
    return JSON.parse(message.toString());
  } catch {
    return null;
  }
}

function isButtonOnePressed(payload) {
  return payload && payload.button1 === "pressed";
}

function handleMqttMessage(message) {
  const payload = parsePayload(message);
  if (!isButtonOnePressed(payload)) return;
  toggleTiming();
}

function connectMqtt() {
  els.title.textContent = mqttTopic();

  if (!window.mqtt) {
    setStatus("error", "MQTT CDN Failed");
    els.message.textContent = "MQTT library not loaded";
    return;
  }

  state.client = mqtt.connect(mqttUrl(), {
    clientId: `flip_timer_${Math.random().toString(16).slice(2, 10)}`,
    username: config.DEFAULT_MQTT_USER || undefined,
    password: config.DEFAULT_MQTT_PASS || undefined,
    reconnectPeriod: 2500,
    connectTimeout: 8000,
    clean: true
  });

  state.client.on("connect", () => {
    setStatus("online", "Online");
    state.client.subscribe(mqttTopic(), { qos: 0 });
    setMode(state.running ? "running" : "idle", "Waiting for button1 pressed");
  });

  state.client.on("message", (_topic, message) => handleMqttMessage(message));
  state.client.on("reconnect", () => setStatus("idle", "Reconnecting"));
  state.client.on("close", () => setStatus("idle", "Offline"));
  state.client.on("error", () => setStatus("error", "Error"));
}

async function requestFullscreen() {
  const root = document.documentElement;
  const fullscreenElement =
    document.fullscreenElement || document.webkitFullscreenElement;

  try {
    if (fullscreenElement) {
      if (document.exitFullscreen) {
        await document.exitFullscreen();
      } else if (document.webkitExitFullscreen) {
        document.webkitExitFullscreen();
      }
      return;
    }

    if (root.requestFullscreen) {
      await root.requestFullscreen({ navigationUI: "hide" });
    } else if (root.webkitRequestFullscreen) {
      root.webkitRequestFullscreen();
    } else {
      document.documentElement.classList.toggle("fullscreen-fallback");
    }

    if (screen.orientation?.lock) await screen.orientation.lock("landscape");
  } catch {
    document.documentElement.classList.toggle("fullscreen-fallback");
  }
}

function updateFullscreenButton() {
  const active =
    document.fullscreenElement ||
    document.webkitFullscreenElement ||
    document.documentElement.classList.contains("fullscreen-fallback");
  els.fullscreenButton.textContent = active ? "Exit" : "Fullscreen";
}

els.fullscreenButton.addEventListener("click", requestFullscreen);
document.addEventListener("fullscreenchange", updateFullscreenButton);
document.addEventListener("webkitfullscreenchange", updateFullscreenButton);
document.addEventListener("keydown", (event) => {
  if (event.code === "Space") toggleTiming();
  if (event.code === "KeyF") requestFullscreen();
});

renderDigits(true);
updateFullscreenButton();
frame();
connectMqtt();
