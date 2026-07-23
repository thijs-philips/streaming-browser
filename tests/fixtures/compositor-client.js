(() => {
  'use strict';

  const PROTOCOL = 'flexvision-layout.v1';
  const DEFAULT_URL = 'ws://127.0.0.1:8765/layout/v1';

  class Client {
    constructor(options = {}) {
      this.url = options.url || DEFAULT_URL;
      this.viewports = new Map();
      this.socket = null;
      this.revision = 0;
      this.flushScheduled = false;
      this.reconnectTimer = 0;
      this.backoff = 100;
      this.closed = false;
      this.connect();
    }

    setViewport(viewport) {
      if (!viewport || !viewport.viewportId || !viewport.sourceId || !viewport.rect) {
        throw new TypeError('setViewport requires viewportId, sourceId, and rect');
      }
      const { x, y, width, height } = viewport.rect;
      if (![x, y, width, height].every(Number.isFinite)) {
        throw new TypeError('viewport rectangle must contain finite numbers');
      }
      this.viewports.set(viewport.viewportId, {
        viewportId: viewport.viewportId,
        sourceId: viewport.sourceId,
        label: viewport.label || viewport.sourceId,
        x, y, width, height
      });
      this.scheduleFlush();
    }

    removeViewport(viewportId) {
      if (this.viewports.delete(viewportId)) this.scheduleFlush();
    }

    replaceAll(viewports) {
      this.viewports.clear();
      for (const viewport of viewports) this.setViewport(viewport);
      this.scheduleFlush();
    }

    refresh() {
      this.scheduleFlush();
    }

    close() {
      this.closed = true;
      clearTimeout(this.reconnectTimer);
      if (this.socket) this.socket.close();
      this.socket = null;
    }

    connect() {
      if (this.closed) return;
      try {
        const socket = new WebSocket(this.url, PROTOCOL);
        this.socket = socket;
        socket.addEventListener('open', () => {
          if (this.socket !== socket) return;
          this.backoff = 100;
          this.scheduleFlush();
        });
        socket.addEventListener('message', event => {
          try {
            const message = JSON.parse(event.data);
            document.documentElement.dataset.compositorStatus = message.type;
          } catch {
            document.documentElement.dataset.compositorStatus = 'invalid-response';
          }
        });
        const reconnect = () => {
          if (this.socket !== socket || this.closed) return;
          this.socket = null;
          const jitter = Math.floor(Math.random() * Math.min(100, this.backoff));
          this.reconnectTimer = setTimeout(() => this.connect(), this.backoff + jitter);
          this.backoff = Math.min(this.backoff * 2, 5000);
          document.documentElement.dataset.compositorStatus = 'disconnected';
        };
        socket.addEventListener('close', reconnect);
        socket.addEventListener('error', () => socket.close());
      } catch {
        this.reconnectTimer = setTimeout(() => this.connect(), this.backoff);
        this.backoff = Math.min(this.backoff * 2, 5000);
      }
    }

    scheduleFlush() {
      if (this.flushScheduled) return;
      this.flushScheduled = true;
      requestAnimationFrame(() => {
        this.flushScheduled = false;
        this.flush();
      });
    }

    flush() {
      if (!this.socket || this.socket.readyState !== WebSocket.OPEN) return;
      this.revision += 1;
      this.socket.send(JSON.stringify({
        type: 'layout',
        protocol: 1,
        revision: this.revision,
        viewports: [...this.viewports.values()]
      }));
    }
  }

  window.CompositorClient = Object.freeze({
    connect(options) { return new Client(options); }
  });
})();
