const CACHE_NAME = "mqtt-flip-timer-v1";
const APP_SHELL = [
  "./flip_timer.html",
  "./flip_timer_config.js",
  "./assets/flip_timer.css",
  "./assets/flip_timer.js",
  "./flip_timer.webmanifest",
  "./flip_timer_icon.svg"
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(APP_SHELL))
  );
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key)))
    )
  );
  self.clients.claim();
});

self.addEventListener("fetch", (event) => {
  if (event.request.method !== "GET") return;
  event.respondWith(
    caches.match(event.request).then((cached) =>
      cached || fetch(event.request).catch(() => caches.match("./flip_timer.html"))
    )
  );
});
