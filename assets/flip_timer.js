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
  mqttUrlIndex: 0,
  mqttRetryTimer: 0,
  running: false,
  startedAt: 0,
  elapsedBeforeRun: 0,
  lastDigits: "000000",
  animationId: 0
};

function mqttUrls() {
  if (Array.isArray(config.MQTT_WS_URLS) && config.MQTT_WS_URLS.length) {
    return config.MQTT_WS_URLS;
  }
  if (config.MQTT_WS_URL) return [config.MQTT_WS_URL];
  const host = config.DEFAULT_MQTT_HOST || "broker.emqx.io";
  return [
    `wss://${host}:8084/mqtt`,
    `wss://${host}:8084`,
    `ws://${host}:8083/mqtt`,
    `ws://${host}:8083`
  ];
}

function currentMqttUrl() {
  const urls = mqttUrls();
  return urls[state.mqttUrlIndex % urls.length];
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

function scheduleMqttReconnect(reason = "Reconnect") {
  window.clearTimeout(state.mqttRetryTimer);
  const urls = mqttUrls();
  state.mqttUrlIndex = (state.mqttUrlIndex + 1) % urls.length;
  setStatus("idle", `${reason} ${state.mqttUrlIndex + 1}/${urls.length}`);
  state.mqttRetryTimer = window.setTimeout(connectMqtt, 1800);
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
  window.clearTimeout(state.mqttRetryTimer);

  if (!window.mqtt) {
    setStatus("error", "MQTT CDN Failed");
    els.message.textContent = "MQTT library not loaded";
    return;
  }

  if (state.client) {
    state.client.end(true);
    state.client = null;
  }

  const url = currentMqttUrl();
  const options = {
    clientId: `flip_timer_${Math.random().toString(16).slice(2, 10)}`,
    reconnectPeriod: 0,
    connectTimeout: 9000,
    clean: true,
    keepalive: 45,
    protocolVersion: 4
  };

  if (config.DEFAULT_MQTT_USER) options.username = config.DEFAULT_MQTT_USER;
  if (config.DEFAULT_MQTT_PASS) options.password = config.DEFAULT_MQTT_PASS;

  setStatus("idle", `Connecting ${state.mqttUrlIndex + 1}/${mqttUrls().length}`);
  state.client = mqtt.connect(url, options);
  const client = state.client;

  client.on("connect", () => {
    if (client !== state.client) return;
    setStatus("online", "Online");
    client.subscribe(mqttTopic(), { qos: 0 });
    setMode(state.running ? "running" : "idle", "Waiting for button1 pressed");
  });

  client.on("message", (_topic, message) => {
    if (client !== state.client) return;
    handleMqttMessage(message);
  });
  client.on("close", () => {
    if (client !== state.client) return;
    scheduleMqttReconnect("Closed");
  });
  client.on("offline", () => {
    if (client !== state.client) return;
    setStatus("idle", "Offline");
  });
  client.on("error", (error) => {
    if (client !== state.client) return;
    const message = error?.message ? error.message.slice(0, 28) : "MQTT Error";
    setStatus("error", message);
    client.end(true);
  });
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
  if (isStandaloneDisplay()) {
    els.fullscreenButton.textContent = "Fullscreen";
    return;
  }

  const active =
    document.fullscreenElement ||
    document.webkitFullscreenElement ||
    document.documentElement.classList.contains("fullscreen-fallback");
  els.fullscreenButton.textContent = active ? "Exit" : "Fullscreen";
}

function isStandaloneDisplay() {
  return window.matchMedia("(display-mode: standalone)").matches ||
    window.matchMedia("(display-mode: fullscreen)").matches ||
    window.navigator.standalone === true;
}

function setupPwaMode() {
  if (isStandaloneDisplay()) {
    document.documentElement.classList.add("is-standalone");
  }
}

function registerServiceWorker() {
  if (!("serviceWorker" in navigator)) return;
  navigator.serviceWorker.register("flip_timer_service_worker.js").catch(() => {});
}

els.fullscreenButton.addEventListener("click", requestFullscreen);
document.addEventListener("fullscreenchange", updateFullscreenButton);
document.addEventListener("webkitfullscreenchange", updateFullscreenButton);
document.addEventListener("keydown", (event) => {
  if (event.code === "Space") toggleTiming();
  if (event.code === "KeyF") requestFullscreen();
});

renderDigits(true);
setupPwaMode();
updateFullscreenButton();
registerServiceWorker();
frame();
connectMqtt();
