// Small outline icons (24x24, 2px round stroke, drawn in the style of the Tabler
// set) so the tabs and gauges can say "drop" or "thermometer" at a glance.
// Colour comes from the surrounding text colour (currentColor).
const PATHS = {
  droplet: ["M12 3c3.2 4.2 6 7.2 6 11a6 6 0 0 1 -12 0c0 -3.8 2.8 -6.8 6 -11z"],
  thermometer: ["M10 13.5a4 4 0 1 0 4 0v-8.5a2 2 0 0 0 -4 0v8.5", "M10 9h4"],
  home: ["M5 12h-2l9 -9l9 9h-2", "M5 12v7a2 2 0 0 0 2 2h10a2 2 0 0 0 2 -2v-7", "M9 21v-6a2 2 0 0 1 2 -2h2a2 2 0 0 1 2 2v6"],
  map: ["M3 7l6 -3l6 3l6 -3v13l-6 3l-6 -3l-6 3v-13", "M9 4v13", "M15 7v13"],
  list: ["M9 6h11", "M9 12h11", "M9 18h11", "M5 6v.01", "M5 12v.01", "M5 18v.01"],
  sun: ["M12 12m-4 0a4 4 0 1 0 8 0a4 4 0 1 0 -8 0", "M3 12h1", "M12 3v1", "M20 12h1", "M12 20v1", "M5.6 5.6l.7 .7", "M18.4 5.6l-.7 .7", "M17.7 17.7l.7 .7", "M6.3 17.7l-.7 .7"],
  chart: ["M4 19h16", "M4 15l4 -6l4 2l4 -5l4 4"],
  signal: ["M6 18v-3", "M10 18v-6", "M14 18v-9", "M18 18v-12"],
  sliders: ["M14 6m-2 0a2 2 0 1 0 4 0a2 2 0 1 0 -4 0", "M4 6h8", "M16 6h4", "M8 12m-2 0a2 2 0 1 0 4 0a2 2 0 1 0 -4 0", "M4 12h2", "M10 12h10",
            "M17 18m-2 0a2 2 0 1 0 4 0a2 2 0 1 0 -4 0", "M4 18h11", "M19 18h1"],
  battery: ["M5 8h12a2 2 0 0 1 2 2v4a2 2 0 0 1 -2 2h-12a2 2 0 0 1 -2 -2v-4a2 2 0 0 1 2 -2", "M22 11v2", "M6 11v2", "M9 11v2"],
  clock: ["M12 12m-9 0a9 9 0 1 0 18 0a9 9 0 1 0 -18 0", "M12 7v5l3 3"],
};

export default function Icon({ name, size = 22 }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"
         strokeLinecap="round" strokeLinejoin="round" aria-hidden="true" className="icon">
      {(PATHS[name] || []).map((d, i) => <path key={i} d={d} />)}
    </svg>
  );
}
