const BASE = import.meta.env.VITE_API_URL || "http://localhost:8000/api";

async function request(path, options = {}) {
  const res = await fetch(`${BASE}${path}`, {
    headers: { "Content-Type": "application/json" },
    credentials: "include", // send/receive the httpOnly session cookie
    ...options,
  });
  if (!res.ok) {
    let detail = res.statusText;
    try {
      const body = await res.json();
      if (Array.isArray(body.detail)) {
        // FastAPI/pydantic validation errors: a list of {loc, msg, ...}
        detail = body.detail.map((e) => e.msg).join("; ");
      } else if (body.detail) {
        detail = body.detail;
      }
    } catch (_) {
      /* ignore */
    }
    throw new Error(detail);
  }
  if (res.status === 204) return null;
  return res.json();
}

export const api = {
  auth: {
    login: (username, password) => request("/auth/login", { method: "POST", body: JSON.stringify({ username, password }) }),
    logout: () => request("/auth/logout", { method: "POST" }),
    // Bootstrapping the session on page load: a 401 (not logged in) is the
    // expected steady state for a fresh visitor, not an error to surface.
    me: async () => {
      try {
        return await request("/auth/me");
      } catch (_) {
        return null;
      }
    },
  },
  zones: {
    list: () => request("/zones"),
    get: (id) => request(`/zones/${id}`),
    create: (data) => request("/zones", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/zones/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/zones/${id}`, { method: "DELETE" }),
    assignSensors: (id, sensor_ids) =>
      request(`/zones/${id}/sensors`, { method: "POST", body: JSON.stringify({ sensor_ids }) }),
    unassignSensor: (id, sensorId) => request(`/zones/${id}/sensors/${sensorId}`, { method: "DELETE" }),
    assignValves: (id, valve_ids) =>
      request(`/zones/${id}/valves`, { method: "POST", body: JSON.stringify({ valve_ids }) }),
    unassignValve: (id, valveId) => request(`/zones/${id}/valves/${valveId}`, { method: "DELETE" }),
    schedules: (id) => request(`/zones/${id}/schedules`),
    createSchedule: (id, data) => request(`/zones/${id}/schedules`, { method: "POST", body: JSON.stringify(data) }),
    deleteSchedule: (id, scheduleId) => request(`/zones/${id}/schedules/${scheduleId}`, { method: "DELETE" }),
    thresholds: (id) => request(`/zones/${id}/thresholds`),
    upsertThreshold: (id, data) => request(`/zones/${id}/thresholds`, { method: "POST", body: JSON.stringify(data) }),
    errors: (id) => request(`/zones/${id}/errors`),
    continueTransition: (id) => request(`/zones/${id}/transition/continue`, { method: "POST" }),
    deactivateFromTransition: (id) => request(`/zones/${id}/transition/deactivate`, { method: "POST" }),
  },
  sensors: {
    list: () => request("/sensors"),
    create: (data) => request("/sensors", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/sensors/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/sensors/${id}`, { method: "DELETE" }),
    readings: (id, limit) => request(`/sensors/${id}/readings${limit ? `?limit=${limit}` : ""}`),
  },
  executors: {
    list: () => request("/executors"),
    create: (data) => request("/executors", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/executors/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/executors/${id}`, { method: "DELETE" }),
  },
  repeaters: {
    list: () => request("/repeaters"),
    create: (data) => request("/repeaters", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/repeaters/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/repeaters/${id}`, { method: "DELETE" }),
  },
  gateway: {
    list: () => request("/gateway"),
    create: (data) => request("/gateway", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/gateway/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/gateway/${id}`, { method: "DELETE" }),
  },
  pumps: {
    list: () => request("/pumps"),
    create: (data) => request("/pumps", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/pumps/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/pumps/${id}`, { method: "DELETE" }),
    command: (id, requested_state) =>
      request(`/pumps/${id}/command`, { method: "POST", body: JSON.stringify({ requested_state }) }),
  },
  valves: {
    list: () => request("/valves"),
    create: (data) => request("/valves", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/valves/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/valves/${id}`, { method: "DELETE" }),
    command: (id, requested_state) =>
      request(`/valves/${id}/command`, { method: "POST", body: JSON.stringify({ requested_state }) }),
  },
  users: {
    list: () => request("/users"),
    create: (data) => request("/users", { method: "POST", body: JSON.stringify(data) }),
    update: (id, data) => request(`/users/${id}`, { method: "PATCH", body: JSON.stringify(data) }),
    remove: (id) => request(`/users/${id}`, { method: "DELETE" }),
  },
  config: {
    get: () => request("/config"),
    update: (data) => request("/config", { method: "PATCH", body: JSON.stringify(data) }),
  },
  errors: {
    network: () => request("/errors/network"),
  },
};
