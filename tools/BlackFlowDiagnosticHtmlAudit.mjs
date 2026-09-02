import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const sourcePath = path.join(
  root,
  "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowMapObservationSource.cpp",
);
const source = fs.readFileSync(sourcePath, "utf8");
const sessionSource = fs.readFileSync(
  path.join(root, "src/MaaCore/Task/Roguelike/BlackFlow/BlackFlowSession.cpp"),
  "utf8",
);
if (!sessionSource.includes(
  "node.battle.has_value() ? node.battle->stage_name : std::string(battle_stage_name(node))",
)) {
  throw new Error("BlackFlow diagnostic node payload has no explicit battle stage field");
}
const templateStart = source.indexOf('output << R"HTML(');
const templateEnd = source.indexOf("    if (!output) {", templateStart);
if (templateStart < 0 || templateEnd < 0) {
  throw new Error("BlackFlow routing HTML template was not found");
}
const templateSource = source.slice(templateStart, templateEnd);
const rawParts = [...templateSource.matchAll(/R"HTML\(([\s\S]*?)\)HTML"/g)].map((match) => match[1]);
let html = rawParts.join("");
if (!html.includes('<script src="routing-history-data.js"></script>') ||
    !html.includes('<script src="processing-item-history-data.js"></script>') ||
    !html.includes(
      "const raw={history:BLACKFLOW_ROUTING_HISTORY,processingEvidence:BLACKFLOW_PROCESSING_ITEM_HISTORY};",
    )) {
  throw new Error("BlackFlow routing HTML data insertion seam changed");
}
const fixture = {
  history: [
    {
      floor: 4,
      map_generation: 7,
      floor_four_remembrance: false,
      map_section_key: "floor-4-generation-7",
      map_section_label: "4 层",
      map_nodes: [
        {
          id: 1,
          row: 0,
          column: 0,
          node_type: "empty",
          node_name: "林间空地",
          marker_type: "fruit_cache",
          marker_display_name: "藏果地",
          marker_score: 0.91,
          progress: "completed",
        },
      ],
      exploration_note_nodes: [
        {
          id: 1,
          row: 0,
          column: 0,
          node_type: "battle_normal",
          node_name: "灌水贤者",
          stage_name: "灌水贤者",
          identity_source: "move_preview_stage_name",
          progress: "completed",
        },
        {
          id: 2,
          row: 0,
          column: 1,
          node_type: "battle_boss",
          node_name: "失落的人偶",
          stage_name: "失落的人偶",
          identity_source: "battle_stage_name",
          progress: "active",
        },
        {
          id: 3,
          row: 1,
          column: 0,
          node_type: "incident",
          node_name: "事件插件返回的任意名称",
          fate_event: true,
          identity_source: "event_name",
          marker_type: "informant",
          marker_display_name: "线人与线索",
          progress: "completed",
        },
        {
          id: 4,
          row: 1,
          column: 1,
          node_type: "wish",
          node_name: "无人商店",
          identity_source: "event_name",
          progress: "completed",
        },
      ],
    },
    {
      floor: 4,
      map_generation: 8,
      floor_four_remembrance: true,
      map_section_key: "floor-4-generation-8-remembrance",
      map_section_label: "追忆 4 层",
    },
  ],
  processingEvidence: [],
};
const requiredFragments = [
  "#map-background[hidden]",
  "function routeSegmentGeometry",
  "function routeLaneOffsets",
  "function routeSegmentsVisuallyOverlap",
  "function readableRouteAngle",
  "arrowPoints",
  "appendStepBadge",
  "0 为起点",
  "svgEl('polygon'",
  "node-label-type",
  "node-label-content",
  "data-layer=\"markers\"",
  "node-marker-badge",
  "node-tooltip",
  "map-view-note",
  "function markerPresentation",
  "function nodePresentation",
  "function nodeTooltipLines",
  "function mapViewDescription",
  "节点标记层",
  "藏果地",
  "流窜“居民”",
  "图例（按图层分组）",
  "节点身份层",
  "规划路线层",
  "识别证据层",
  "揭示核对层",
  "recognition-grid",
  "零件箱截图",
  "移动方式选择截图",
  "ocr_name_hits",
  "latestByType",
  "map_section_key",
];
for (const fragment of requiredFragments) {
  if (!html.includes(fragment)) {
    throw new Error(`BlackFlow routing HTML is missing: ${fragment}`);
  }
}
for (const obsoleteFragment of ["routeModeRail", "route-label-leader", "marker-end", "placeRouteLabel"]) {
  if (html.includes(obsoleteFragment)) {
    throw new Error(`BlackFlow routing HTML still contains obsolete route clutter: ${obsoleteFragment}`);
  }
}

const scripts = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)].map((match) => match[1]);
if (!scripts.length) {
  throw new Error("BlackFlow routing HTML has no script");
}
// The first script is JSON data; the last one is the DOM renderer.
new Function(scripts.at(-1));

const extractFunction = (name) => {
  const match = source.match(new RegExp(`^function ${name}\\([^\\n]+$`, "m"));
  if (!match) {
    throw new Error(`BlackFlow routing HTML helper was not found: ${name}`);
  }
  return match[0];
};
const sandbox = {};
vm.createContext(sandbox);
const helperConstants = ["nodeTypeNames", "fateEventNames", "markerTypeNames", "identitySourceNames"].map((name) => {
  const match = source.match(new RegExp(`^const ${name}=.*;$`, "m"));
  if (!match) {
    throw new Error(`BlackFlow routing HTML helper constant was not found: ${name}`);
  }
  return match[0];
});
vm.runInContext(
  [...helperConstants,
    "readableRouteAngle",
    "routeSegmentGeometry",
    "routeSegmentsVisuallyOverlap",
    "routeLaneOffsets",
    "routeSequence",
    "mapSection",
    "prettyNodeType",
    "prettyMarkerType",
    "nodePresentation",
    "markerPresentation",
    "nodeTooltipLines",
    "mapViewDescription",
  ].map((entry) => entry.startsWith("const ") ? entry : extractFunction(entry)).join("\n"),
  sandbox,
);
const geometry = sandbox.routeSegmentGeometry({ x: 100, y: 100 }, { x: 300, y: 100 }, 0);
if (!geometry || Math.abs(Math.hypot(geometry.start.x - 100, geometry.start.y - 100) - 25) > 0.01) {
  throw new Error("route tail is not trimmed away from the source badge");
}
if (Math.abs(Math.hypot(geometry.end.x - 300, geometry.end.y - 100) - 31) > 0.01) {
  throw new Error("route line does not stop at the arrow-head base");
}
if (Math.abs(Math.hypot(geometry.arrowTip.x - 300, geometry.arrowTip.y - 100) - 17) > 0.01) {
  throw new Error("route arrow tip is too far from the landing badge");
}
const reciprocal = sandbox.routeLaneOffsets([
  { from: "a", to: "b", a: { x: 100, y: 100 }, b: { x: 300, y: 100 } },
  { from: "b", to: "a", a: { x: 300, y: 100 }, b: { x: 100, y: 100 } },
]);
const outboundGeometry = sandbox.routeSegmentGeometry(
  { x: 100, y: 100 },
  { x: 300, y: 100 },
  reciprocal[0].laneOffset,
);
const returnGeometry = sandbox.routeSegmentGeometry(
  { x: 300, y: 100 },
  { x: 100, y: 100 },
  reciprocal[1].laneOffset,
);
if (!(outboundGeometry.anchor.y < 100 && returnGeometry.anchor.y > 100)) {
  throw new Error("reciprocal route legs are not separated onto opposite physical lanes");
}
const partiallyOverlapping = sandbox.routeLaneOffsets([
  { from: "a", to: "c", a: { x: 100, y: 200 }, b: { x: 500, y: 200 } },
  { from: "a", to: "b", a: { x: 100, y: 200 }, b: { x: 300, y: 200 } },
]);
const longGeometry = sandbox.routeSegmentGeometry(
  partiallyOverlapping[0].a,
  partiallyOverlapping[0].b,
  partiallyOverlapping[0].laneOffset,
);
const shortGeometry = sandbox.routeSegmentGeometry(
  partiallyOverlapping[1].a,
  partiallyOverlapping[1].b,
  partiallyOverlapping[1].laneOffset,
);
if (!(Math.abs(longGeometry.anchor.y - shortGeometry.anchor.y) >= 9.9)) {
  throw new Error("partially overlapping collinear route legs still share one visual lane");
}
if (sandbox.mapSection(fixture.history[0]).key === sandbox.mapSection(fixture.history[1]).key) {
  throw new Error("normal fourth floor and remembrance fourth floor share one navigation key");
}
const marker = sandbox.markerPresentation(fixture.history[0].map_nodes[0]);
if (marker.short !== "果" || marker.name !== "藏果地") {
  throw new Error("fruit-cache marker has no distinct visible presentation");
}
const tooltip = sandbox.nodeTooltipLines(fixture.history[0].exploration_note_nodes[0], "notebook");
if (!tooltip.some((line) => line === "关卡名：灌水贤者") || !tooltip.some((line) => line.includes("作战"))) {
  throw new Error("exploration notebook tooltip omits its explicit battle stage field");
}
const boss = sandbox.nodePresentation(fixture.history[0].exploration_note_nodes[1]);
if (boss.title !== "险路恶敌" || boss.content !== "失落的人偶" || boss.contentLabel !== "关卡名") {
  throw new Error("boss node is mislabeled or omits its normalized stage name");
}
const fate = sandbox.nodePresentation(fixture.history[0].exploration_note_nodes[2]);
if (fate.title !== "命运所指" || fate.content !== "事件插件返回的任意名称" ||
    fate.contentLabel !== "事件名") {
  throw new Error("fate event does not display the event plugin result verbatim");
}
const fateMarker = sandbox.markerPresentation(fixture.history[0].exploration_note_nodes[2]);
if (fateMarker.name !== "谜题与谜底" || fateMarker.short !== "线") {
  throw new Error("informant marker on a fate node is not presented as 谜题与谜底");
}
const regularInformant = sandbox.markerPresentation({
  node_type: "incident",
  node_name: "不期而遇",
  marker_type: "informant",
  marker_display_name: "线人",
});
if (regularInformant.name !== "线人与线索") {
  throw new Error("ordinary informant marker does not use its full name");
}
const wish = sandbox.nodePresentation(fixture.history[0].exploration_note_nodes[3]);
if (wish.title !== "得偿所愿" || wish.content !== "") {
  throw new Error("node detail leaks from a category that should show only its title");
}
for (const [node, expectedTitle, expectedContent, expectedLabel] of [
  [
    { node_type: "battle_elite", node_name: "紧急作战", stage_name: "冰冷流亡" },
    "紧急作战",
    "冰冷流亡",
    "关卡名",
  ],
  [{ node_type: "battle_boss", node_name: "险路恶敌", stage_name: "" }, "险路恶敌", "", "关卡名"],
  [{ node_type: "incident", node_name: "划算买卖" }, "不期而遇", "划算买卖", "事件名"],
  [{ node_type: "duel", node_name: "原始娱乐" }, "狭路相逢", "原始娱乐", "事件名"],
  [{ node_type: "incident", node_name: "窥视箱中" }, "命运所指", "窥视箱中", "事件名"],
  [{ node_type: "incident", node_name: "好奇心之死" }, "命运所指", "好奇心之死", "事件名"],
  [{ node_type: "incident", node_name: "命运所指" }, "命运所指", "", "事件名"],
]) {
  const presentation = sandbox.nodePresentation(node);
  if (presentation.title !== expectedTitle || presentation.content !== expectedContent ||
      presentation.contentLabel !== expectedLabel) {
    throw new Error(`unexpected node presentation: ${JSON.stringify(presentation)}`);
  }
}
if (sandbox.mapViewDescription("current") === sandbox.mapViewDescription("notebook")) {
  throw new Error("current observation and exploration notebook have no distinct explanation");
}

const outputDirectory = path.join(root, "build/blackflow-diagnostic-html-audit");
fs.mkdirSync(outputDirectory, { recursive: true });
const outputPath = path.join(outputDirectory, "routing.fixture.html");
fs.writeFileSync(
  path.join(outputDirectory, "routing-history-data.js"),
  `const BLACKFLOW_ROUTING_HISTORY=${JSON.stringify(fixture.history)};\n`,
);
fs.writeFileSync(
  path.join(outputDirectory, "processing-item-history-data.js"),
  `const BLACKFLOW_PROCESSING_ITEM_HISTORY=${JSON.stringify(fixture.processingEvidence)};\n`,
);
fs.writeFileSync(outputPath, html);
console.log(`BlackFlow diagnostic HTML audit passed: ${outputPath}`);

const roundTripLabel = (geometry, text) => {
  const width = Math.max(62, Math.min(190, text.length * 13 + 18));
  return `<g transform="translate(${geometry.anchor.x} ${geometry.anchor.y}) rotate(${geometry.labelAngle})"><rect x="${-width / 2}" y="-12" width="${width}" height="24" rx="6" class="route-mode-bg item"/><text x="0" y="0" class="route-mode">${text}</text></g>`;
};
const roundTripPath = path.join(outputDirectory, "routing.roundtrip.svg");
fs.writeFileSync(roundTripPath, `<svg xmlns="http://www.w3.org/2000/svg" width="400" height="220" viewBox="0 0 400 220">
<style>.route-casing,.route{fill:none;stroke-linecap:round}.route-casing{stroke:#07131ae6;stroke-width:10;stroke-dasharray:14 8}.route{stroke:#ff9d4d;stroke-width:5.5;stroke-dasharray:14 8}.route-arrow{fill:#ff9d4d;stroke:#10151d;stroke-width:1.5}.route-mode{fill:#fff;font:700 13px system-ui,"Microsoft YaHei",sans-serif;text-anchor:middle;dominant-baseline:central;paint-order:stroke;stroke:#10151d;stroke-width:4}.route-mode-bg{fill:#10151df2;stroke:#ff9d4d;stroke-width:1.5}.node{fill:#263544;stroke:#b7cde0;stroke-width:3}.step{fill:#101923;stroke:#ff9d4d;stroke-width:4}.step.start{stroke:#f6d365}.step-number{fill:#fff;font:800 12px system-ui,sans-serif;text-anchor:middle;dominant-baseline:central}</style>
<rect width="400" height="220" fill="#101923"/><circle class="node" cx="100" cy="100" r="26"/><circle class="node" cx="300" cy="100" r="26"/>
<path class="route-casing" d="${outboundGeometry.path}"/><path class="route" d="${outboundGeometry.path}"/><polygon class="route-arrow" points="${outboundGeometry.arrowPoints}"/>
<path class="route-casing" d="${returnGeometry.path}"/><path class="route" d="${returnGeometry.path}"/><polygon class="route-arrow" points="${returnGeometry.arrowPoints}"/>
${roundTripLabel(outboundGeometry, "1 标准引擎")}${roundTripLabel(returnGeometry, "2 重弹簧")}
<circle class="step start" cx="100" cy="75" r="13"/><text class="step-number" x="100" y="75">0</text><circle class="step" cx="300" cy="100" r="13"/><text class="step-number" x="300" y="100">1</text><circle class="step" cx="100" cy="125" r="13"/><text class="step-number" x="100" y="125">2</text>
</svg>`);
console.log(`BlackFlow reciprocal route visual audit fixture: ${roundTripPath}`);

const routingPath = process.argv[2];
if (routingPath) {
  const routingHtml = fs.readFileSync(routingPath, "utf8");
  const routingDirectory = path.dirname(routingPath);
  const readSidecar = (fileName, variableName) => {
    const sidecar = fs.readFileSync(path.join(routingDirectory, fileName), "utf8");
    const prefix = `const ${variableName}=`;
    if (!sidecar.startsWith(prefix) || !sidecar.endsWith(";\n")) {
      throw new Error(`BlackFlow routing data frame is invalid: ${fileName}`);
    }
    return JSON.parse(sidecar.slice(prefix.length, -2));
  };
  if (!routingHtml.includes('<script src="routing-history-data.js"></script>') ||
      !routingHtml.includes('<script src="processing-item-history-data.js"></script>')) {
    throw new Error(`BlackFlow routing data was not found: ${routingPath}`);
  }
  const routingData = {
    history: readSidecar("routing-history-data.js", "BLACKFLOW_ROUTING_HISTORY"),
    processingEvidence: readSidecar(
      "processing-item-history-data.js",
      "BLACKFLOW_PROCESSING_ITEM_HISTORY",
    ),
  };
  const requestedImage = process.argv[3];
  const records = routingData.history.filter((entry) =>
    Array.isArray(entry.planned_route_steps) && entry.planned_route_steps.length > 0
  );
  const record = requestedImage
    ? records.find((entry) => entry.captured_image_file === requestedImage)
    : records.toSorted(
      (left, right) => right.planned_route_steps.length - left.planned_route_steps.length,
    )[0];
  if (!record) {
    throw new Error(`No routed observation matched: ${requestedImage || "largest route"}`);
  }
  const imagePath = path.resolve(path.dirname(routingPath), record.captured_image_file);
  const imageMime = path.extname(imagePath).toLowerCase() === ".png" ? "image/png" : "image/jpeg";
  const imageData = fs.readFileSync(imagePath).toString("base64");
  const width = Number(record.captured_image_width) || 1280;
  const height = Number(record.captured_image_height) || 720;
  const nodes = record.map_nodes || record.planning_map?.nodes || record.nodes || [];
  const byId = new Map(nodes.map((node) => [String(node.id), node]));
  const rows = nodes.map((node) => Number(node.row) || 0);
  const columns = nodes.map((node) => Number(node.column) || 0);
  const minRow = Math.min(...rows);
  const maxRow = Math.max(...rows);
  const minColumn = Math.min(...columns);
  const maxColumn = Math.max(...columns);
  const position = (node) => Number.isFinite(Number(node.visual_x)) &&
      Number.isFinite(Number(node.visual_y))
    ? { x: Number(node.visual_x), y: Number(node.visual_y) }
    : {
      x: width * 0.08 + (Number(node.column) - minColumn) * width * 0.64 /
        Math.max(1, maxColumn - minColumn),
      y: height * 0.12 + (Number(node.row) - minRow) * height * 0.72 /
        Math.max(1, maxRow - minRow),
    };
  const segments = [];
  record.planned_route_steps.forEach((step, stepIndex) => {
    const sequence = sandbox.routeSequence(step);
    for (let index = 1; index < sequence.length; ++index) {
      const first = byId.get(sequence[index - 1]);
      const second = byId.get(sequence[index]);
      if (first && second) {
        segments.push({
          from: sequence[index - 1],
          to: sequence[index],
          a: position(first),
          b: position(second),
          stepIndex,
          item: Boolean(step.uses_processing_item),
        });
      }
    }
  });
  sandbox.routeLaneOffsets(segments);
  const routeGeometry = new Map();
  const routeMarkup = segments.map((segment) => {
    const geometry = sandbox.routeSegmentGeometry(segment.a, segment.b, segment.laneOffset);
    if (!geometry) return "";
    const current = routeGeometry.get(segment.stepIndex);
    if (!current || geometry.length > current.length) routeGeometry.set(segment.stepIndex, geometry);
    const className = segment.item ? " item" : "";
    return `<path d="${geometry.path}" class="route-casing${className}"/><path d="${geometry.path}" class="route${className}"/><polygon points="${geometry.arrowPoints}" class="route-arrow${className}"/>`;
  }).join("");
  const escapeXml = (value) => String(value).replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;");
  const landingTotals = new Map();
  const landingOrdinals = new Map();
  const firstSequence = record.planned_route_steps.length
    ? sandbox.routeSequence(record.planned_route_steps[0])
    : [];
  const startId = firstSequence[0];
  if (startId !== undefined) landingTotals.set(startId, 1);
  for (const step of record.planned_route_steps) {
    const landingId = sandbox.routeSequence(step).at(-1);
    landingTotals.set(landingId, (landingTotals.get(landingId) || 0) + 1);
  }
  const badgeFor = (nodeId, item, number, start = false) => {
    const node = byId.get(nodeId);
    if (!node) return "";
    const base = position(node);
    const total = landingTotals.get(nodeId) || 1;
    const ordinal = landingOrdinals.get(nodeId) || 0;
    landingOrdinals.set(nodeId, ordinal + 1);
    const angle = -Math.PI / 2 + ordinal * Math.PI * 2 / total;
    const distance = total > 1 ? 25 : 0;
    const x = base.x + Math.cos(angle) * distance;
    const y = base.y + Math.sin(angle) * distance;
    return `<circle cx="${x}" cy="${y}" r="13" class="step${item ? " item" : ""}${start ? " start" : ""}"/><text x="${x}" y="${y}" class="step-number">${number}</text>`;
  };
  const startBadgeMarkup = startId === undefined ? "" : badgeFor(startId, false, 0, true);
  const badgeMarkup = startBadgeMarkup + record.planned_route_steps.map((step, index) => {
    const sequence = sandbox.routeSequence(step);
    const landingId = sequence.at(-1);
    const item = Boolean(step.uses_processing_item);
    let markup = "";
    const geometry = routeGeometry.get(index);
    if (item && geometry) {
      const movement = step.movement_name || step.movement;
      const text = `${index + 1} ${movement}`;
      const labelWidth = Math.max(62, Math.min(190, text.length * 13 + 18));
      markup += `<g transform="translate(${geometry.anchor.x} ${geometry.anchor.y}) rotate(${geometry.labelAngle})"><rect x="${-labelWidth / 2}" y="-12" width="${labelWidth}" height="24" rx="6" class="route-mode-bg item"/><text x="0" y="0" class="route-mode">${escapeXml(text)}</text></g>`;
    }
    if (landingId !== undefined) markup += badgeFor(landingId, item, index + 1);
    return markup;
  }).join("");
  const auditSvg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">
<style>.route-casing,.route{fill:none;stroke-linecap:round;stroke-linejoin:round}.route-casing{stroke:#07131ae6;stroke-width:10}.route-casing.item{stroke-dasharray:14 8}.route{stroke:#50e6d5;stroke-width:5.5}.route.item{stroke:#ff9d4d;stroke-dasharray:14 8}.route-arrow{fill:#50e6d5;stroke:#10151d;stroke-width:1.5;stroke-linejoin:round}.route-arrow.item{fill:#ff9d4d}.route-mode{fill:#fff;font:700 13px system-ui,"Microsoft YaHei",sans-serif;text-anchor:middle;dominant-baseline:central;paint-order:stroke;stroke:#10151d;stroke-width:4}.route-mode-bg{fill:#10151df2;stroke:#50e6d5;stroke-width:1.5}.route-mode-bg.item{stroke:#ff9d4d}.step{fill:#101923;stroke:#50e6d5;stroke-width:4}.step.item{stroke:#ff9d4d}.step.start{stroke:#f6d365}.step-number{fill:#fff;font:800 12px system-ui,"Microsoft YaHei",sans-serif;text-anchor:middle;dominant-baseline:central;paint-order:stroke;stroke:#10151d;stroke-width:3}</style>
<image width="${width}" height="${height}" href="data:${imageMime};base64,${imageData}"/>
${routeMarkup}${badgeMarkup}</svg>`;
  const visualPath = path.join(outputDirectory, "routing.actual.svg");
  fs.writeFileSync(visualPath, auditSvg);
  console.log(`BlackFlow route visual audit fixture: ${visualPath}`);
}
