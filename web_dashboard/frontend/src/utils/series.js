// Chart periods for the external-sensor charts: hours of history and how many
// time buckets to draw (24 h -> 30-min buckets, 7 d -> 3-hour, 30 d -> 12-hour).
export const PERIODS = {
  "24h": { label: "24 часа", hours: 24, buckets: 48 },
  "7d": { label: "7 дни", hours: 24 * 7, buckets: 56 },
  "30d": { label: "30 дни", hours: 24 * 30, buckets: 60 },
};

const NO_DATA = -32768; // the "no data" marker of the sensor protocol - never a real value

// One sensor's readings -> points for SensorChart: the mean of each equal-width
// time bucket between tStart and tEnd; empty buckets stay null (a gap, not a
// made-up value). Returns [] when there is nothing at all.
export function bucketSeries(readings, attr, tStart, tEnd, bucketCount) {
  const span = tEnd - tStart || 1;
  const sums = Array.from({ length: bucketCount }, () => ({ sum: 0, n: 0 }));
  let any = false;
  for (const r of readings) {
    const v = r[attr];
    if (v === null || v === undefined || v === NO_DATA) continue;
    const t = new Date(r.recorded_at).getTime();
    if (t < tStart || t > tEnd) continue;
    const i = Math.min(bucketCount - 1, Math.floor(((t - tStart) / span) * bucketCount));
    sums[i].sum += v;
    sums[i].n += 1;
    any = true;
  }
  if (!any) return [];
  return sums.map((b, i) => ({
    value: b.n ? b.sum / b.n : null,
    t: new Date(tStart + ((i + 0.5) / bucketCount) * span),
  }));
}
