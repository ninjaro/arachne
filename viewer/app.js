"use strict";

const SVG_NS = "http://www.w3.org/2000/svg";
const VIEW_WIDTH = 1100;
const VIEW_HEIGHT = 650;
const MAX_NODES = 180;
const MAX_EDGES = 500;
const MAX_RELATION_ROWS = 100;

const state = {
  projection: null,
  query: "",
  view: "relations",
  showDerived: true,
  selectedId: null,
  transform: { x: 0, y: 0, scale: 1 },
  drag: null,
};

let nodesById = new Map();

function text(value) {
  return String(value ?? "");
}

function attributes(item) {
  return item.attributes && typeof item.attributes === "object" ? item.attributes : {};
}

function provenance(item) {
  return item.provenance && typeof item.provenance === "object" ? item.provenance : {};
}

function isDerived(item) {
  return provenance(item).origin !== "human_authored";
}

function itemId(item) {
  return item.node_id || item.edge_id || "unknown";
}

function itemLabel(item) {
  return item.label || item.edge_type || itemId(item);
}

function nodeYear(node) {
  const values = attributes(node);
  const year = values.year_start ?? values.year;
  return Number.isInteger(year) ? year : null;
}

function edgeKind(edge) {
  if (edge.edge_type === "derived_chronological") return "chronology";
  if (edge.edge_type === "derived_similarity") return "similarity";
  if (provenance(edge).origin === "derived_external" || attributes(edge).soft_guidance) return "suggestion";
  return isDerived(edge) ? "derived" : "human";
}

function searchable(item) {
  const values = attributes(item);
  return `${text(itemLabel(item))} ${text(itemId(item))} ${text(item.node_type)} ${text(item.edge_type)} ${text(values.medium)} ${text(values.group_id)} ${text(values.year_start)} ${text(provenance(item).explanation)}`.toLocaleLowerCase();
}

function edgeInView(edge) {
  if (!state.showDerived && isDerived(edge)) return false;
  if (state.view === "chronology") return edge.edge_type === "derived_chronological";
  if (state.view === "similarity") return edge.edge_type === "derived_similarity";
  if (state.view === "research") {
    return provenance(edge).origin === "derived_external"
      || attributes(edge).soft_guidance
      || nodesById.get(edge.source)?.graph_domain === "candidate"
      || nodesById.get(edge.target)?.graph_domain === "candidate";
  }
  return true;
}

function nodeInView(node, scopedEndpointIds) {
  if (state.view === "chronology") return node.node_type === "work" && nodeYear(node) !== null;
  if (state.view === "similarity") return scopedEndpointIds.has(node.node_id);
  if (state.view === "research") return node.graph_domain === "candidate";
  return true;
}

function compareNodes(left, right) {
  if (state.view === "chronology") {
    const byYear = (nodeYear(left) ?? 0) - (nodeYear(right) ?? 0);
    if (byYear !== 0) return byYear;
  }
  return `${left.graph_domain}\n${left.node_type}\n${left.node_id}`.localeCompare(
    `${right.graph_domain}\n${right.node_type}\n${right.node_id}`,
  );
}

function visibleModel() {
  const scopedEdges = state.projection.edges.filter(edgeInView);
  const scopedEndpointIds = new Set(scopedEdges.flatMap((edge) => [edge.source, edge.target]));
  const scopedNodes = state.projection.nodes.filter((node) => nodeInView(node, scopedEndpointIds));
  const scopedIds = new Set(scopedNodes.map((node) => node.node_id));
  let edges = scopedEdges.filter((edge) => scopedIds.has(edge.source) && scopedIds.has(edge.target));
  let nodes = scopedNodes;

  const query = state.query.trim().toLocaleLowerCase();
  if (query) {
    const matchingNodeIds = new Set(nodes.filter((node) => searchable(node).includes(query)).map((node) => node.node_id));
    const matchingEdgeIds = new Set(edges.filter((edge) => searchable(edge).includes(query)).map((edge) => edge.edge_id));
    edges = edges.filter((edge) => matchingEdgeIds.has(edge.edge_id) || matchingNodeIds.has(edge.source) || matchingNodeIds.has(edge.target));
    const contextualIds = new Set(matchingNodeIds);
    for (const edge of edges) {
      contextualIds.add(edge.source);
      contextualIds.add(edge.target);
    }
    nodes = nodes.filter((node) => contextualIds.has(node.node_id));
  }

  nodes = [...nodes].sort(compareNodes);
  const totalNodes = nodes.length;
  const totalEdges = edges.length;
  if (nodes.length > MAX_NODES) {
    const selected = nodes.find((node) => node.node_id === state.selectedId);
    nodes = nodes.slice(0, MAX_NODES);
    if (selected && !nodes.some((node) => node.node_id === selected.node_id)) {
      nodes[nodes.length - 1] = selected;
      nodes.sort(compareNodes);
    }
  }
  const renderedIds = new Set(nodes.map((node) => node.node_id));
  edges = edges
    .filter((edge) => renderedIds.has(edge.source) && renderedIds.has(edge.target))
    .sort((left, right) => left.edge_id.localeCompare(right.edge_id))
    .slice(0, MAX_EDGES);

  return { nodes, edges, totalNodes, totalEdges };
}

function svgElement(name, className = "") {
  const element = document.createElementNS(SVG_NS, name);
  if (className) element.setAttribute("class", className);
  return element;
}

function relationshipContext(model) {
  const selectedNode = nodesById.get(state.selectedId);
  const selectedEdge = model.edges.find((edge) => edge.edge_id === state.selectedId);
  const relatedIds = new Set();
  if (selectedNode) {
    relatedIds.add(selectedNode.node_id);
    for (const edge of model.edges) {
      if (edge.source === selectedNode.node_id) relatedIds.add(edge.target);
      if (edge.target === selectedNode.node_id) relatedIds.add(edge.source);
    }
  } else if (selectedEdge) {
    relatedIds.add(selectedEdge.source);
    relatedIds.add(selectedEdge.target);
  }
  return { selectedNode, selectedEdge, relatedIds };
}

function relationLayout(nodes) {
  const positions = new Map();
  if (nodes.length === 1) {
    positions.set(nodes[0].node_id, { x: VIEW_WIDTH / 2, y: VIEW_HEIGHT / 2 });
    return positions;
  }
  const goldenAngle = Math.PI * (3 - Math.sqrt(5));
  nodes.forEach((node, index) => {
    const radius = Math.min(280, 24 + Math.sqrt(index + 1) * 20);
    const angle = -Math.PI / 2 + index * goldenAngle;
    positions.set(node.node_id, {
      x: VIEW_WIDTH / 2 + Math.cos(angle) * radius,
      y: VIEW_HEIGHT / 2 + Math.sin(angle) * radius * 0.86,
    });
  });
  return positions;
}

function chronologyLayout(nodes, viewport) {
  const positions = new Map();
  const years = nodes.map(nodeYear).filter((year) => year !== null);
  if (!years.length) return positions;
  const minimum = Math.min(...years);
  const maximum = Math.max(...years);
  const span = maximum - minimum;
  const occurrences = new Map();
  for (const node of nodes) {
    const year = nodeYear(node);
    const occurrence = occurrences.get(year) || 0;
    occurrences.set(year, occurrence + 1);
    const ratio = span === 0 ? 0.5 : (year - minimum) / span;
    const x = 90 + ratio * (VIEW_WIDTH - 180);
    const y = 245 + (occurrence % 5) * 62;
    positions.set(node.node_id, { x, y });
  }

  const axis = svgElement("g", "timeline-axis");
  const line = svgElement("line");
  line.setAttribute("x1", "75");
  line.setAttribute("x2", String(VIEW_WIDTH - 75));
  line.setAttribute("y1", "180");
  line.setAttribute("y2", "180");
  axis.append(line);
  const ticks = span === 0 ? 1 : Math.min(10, span + 1);
  for (let index = 0; index < ticks; index += 1) {
    const ratio = ticks === 1 ? 0 : index / (ticks - 1);
    const year = Math.round(minimum + ratio * span);
    const x = 90 + ratio * (VIEW_WIDTH - 180);
    const tick = svgElement("line");
    tick.setAttribute("x1", String(x));
    tick.setAttribute("x2", String(x));
    tick.setAttribute("y1", "171");
    tick.setAttribute("y2", "189");
    const label = svgElement("text");
    label.setAttribute("x", String(x));
    label.setAttribute("y", "155");
    label.textContent = String(year);
    axis.append(tick, label);
  }
  viewport.append(axis);
  return positions;
}

function shortened(value, length = 25) {
  const content = text(value);
  return content.length <= length ? content : `${content.slice(0, length - 1)}…`;
}

function drawGraph(model) {
  const canvas = document.querySelector("#graph-canvas");
  canvas.replaceChildren();
  const title = svgElement("title");
  title.id = "graph-title";
  title.textContent = "Art-lineage graph";
  const description = svgElement("desc");
  description.id = "graph-description";
  description.textContent = "Pan and zoom the canvas. Select a node or line to inspect its relation and provenance.";
  const viewport = svgElement("g", "graph-viewport");
  canvas.append(title, description, viewport);
  const positions = state.view === "chronology"
    ? chronologyLayout(model.nodes, viewport)
    : relationLayout(model.nodes);
  const context = relationshipContext(model);

  const edgeLayer = svgElement("g", "edge-layer");
  for (const edge of model.edges) {
    const source = positions.get(edge.source);
    const target = positions.get(edge.target);
    if (!source || !target) continue;
    const line = svgElement("line", `graph-edge ${edgeKind(edge)}`);
    line.setAttribute("x1", String(source.x));
    line.setAttribute("y1", String(source.y));
    line.setAttribute("x2", String(target.x));
    line.setAttribute("y2", String(target.y));
    line.setAttribute("tabindex", "0");
    line.setAttribute("role", "button");
    line.setAttribute("aria-label", `${edge.edge_type}: ${edge.source} to ${edge.target}`);
    if (state.selectedId === edge.edge_id) line.classList.add("selected");
    const incident = context.selectedNode && (edge.source === context.selectedNode.node_id || edge.target === context.selectedNode.node_id);
    if ((context.selectedNode && !incident) || (context.selectedEdge && context.selectedEdge.edge_id !== edge.edge_id)) line.classList.add("dimmed");
    const title = svgElement("title");
    title.textContent = `${edge.edge_type}: ${nodesById.get(edge.source)?.label || edge.source} → ${nodesById.get(edge.target)?.label || edge.target}`;
    line.append(title);
    line.addEventListener("click", (event) => { event.stopPropagation(); selectItem(edge); });
    line.addEventListener("keydown", (event) => activateWithKeyboard(event, edge));
    edgeLayer.append(line);
  }
  viewport.append(edgeLayer);

  const nodeLayer = svgElement("g", "node-layer");
  for (const node of model.nodes) {
    const position = positions.get(node.node_id);
    if (!position) continue;
    const group = svgElement("g", `graph-node ${node.graph_domain === "candidate" ? "candidate" : "product"}`);
    group.setAttribute("transform", `translate(${position.x} ${position.y})`);
    group.setAttribute("tabindex", "0");
    group.setAttribute("role", "button");
    group.setAttribute("aria-label", `${node.node_type}: ${node.label}`);
    if (state.selectedId === node.node_id) group.classList.add("selected");
    if (context.relatedIds.size && !context.relatedIds.has(node.node_id)) group.classList.add("dimmed");
    const circle = svgElement("circle");
    circle.setAttribute("r", node.graph_domain === "candidate" ? "11" : "13");
    const label = svgElement("text", "node-label");
    label.setAttribute("x", "18");
    label.setAttribute("y", "4");
    label.textContent = shortened(node.label);
    const type = svgElement("text", "node-kind");
    type.setAttribute("x", "18");
    type.setAttribute("y", "20");
    type.textContent = state.view === "chronology" ? text(nodeYear(node)) : node.node_type.replaceAll("_", " ");
    const title = svgElement("title");
    title.textContent = `${node.label} (${node.node_type})`;
    group.append(circle, label, type, title);
    group.addEventListener("click", (event) => { event.stopPropagation(); selectItem(node); });
    group.addEventListener("keydown", (event) => activateWithKeyboard(event, node));
    nodeLayer.append(group);
  }
  viewport.append(nodeLayer);
  applyTransform();
}

function activateWithKeyboard(event, item) {
  if (event.key === "Enter" || event.key === " ") {
    event.preventDefault();
    event.stopPropagation();
    selectItem(item);
  }
}

function appendEndpoint(details, label, nodeId) {
  const row = document.createElement("p");
  row.className = "endpoint";
  const prefix = document.createElement("span");
  prefix.textContent = `${label}: `;
  const button = document.createElement("button");
  button.type = "button";
  button.textContent = nodesById.get(nodeId)?.label || nodeId;
  button.addEventListener("click", () => selectItem(nodesById.get(nodeId)));
  row.append(prefix, button);
  details.append(row);
}

function appendDetailsList(details, headingText, values) {
  if (!Array.isArray(values) || !values.length) return;
  const heading = document.createElement("h3");
  heading.textContent = headingText;
  const list = document.createElement("ul");
  list.className = "detail-list";
  for (const value of values) {
    const item = document.createElement("li");
    const linkedNode = nodesById.get(value);
    if (linkedNode) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = linkedNode.label;
      button.title = value;
      button.addEventListener("click", () => selectItem(linkedNode));
      item.append(button);
    } else {
      const code = document.createElement("code");
      code.textContent = value;
      item.append(code);
    }
    list.append(item);
  }
  details.append(heading, list);
}

function explain(item) {
  if (!item) return;
  const details = document.querySelector("#details");
  details.replaceChildren();
  const heading = document.createElement("h2");
  heading.textContent = itemLabel(item);
  details.append(heading);

  const origin = provenance(item).origin;
  const badge = document.createElement("p");
  badge.className = `badge ${origin === "human_authored" ? "human" : "derived"}`;
  badge.textContent = origin === "human_authored"
    ? "Accepted human-authored data"
    : "Algorithmically derived — not a research claim";
  details.append(badge);

  if (item.edge_id) {
    appendEndpoint(details, "From", item.source);
    appendEndpoint(details, "To", item.target);
  }

  const explanation = document.createElement("p");
  explanation.textContent = provenance(item).explanation || "No additional explanation is available.";
  details.append(explanation);

  if (attributes(item).soft_guidance) {
    const soft = document.createElement("p");
    soft.className = "soft-note";
    soft.textContent = "This is a soft suggestion. It does not reserve or assign the subject.";
    details.append(soft);
  }

  appendDetailsList(details, "Shared accepted concepts", attributes(item).shared_concept_ids);
  appendDetailsList(details, "Projection source records", provenance(item).source_ids);

  const code = document.createElement("code");
  code.textContent = itemId(item);
  details.append(code);
}

function resetDetails() {
  const details = document.querySelector("#details");
  details.replaceChildren();
  const heading = document.createElement("h2");
  heading.textContent = "Select a thread";
  const copy = document.createElement("p");
  copy.textContent = "Choose a node or line to inspect its source, provenance, and explanation.";
  details.append(heading, copy);
}

function selectItem(item) {
  if (!item) return;
  state.selectedId = itemId(item);
  render();
  explain(item);
}

function renderRelationList(model) {
  const list = document.querySelector("#relation-list");
  list.replaceChildren();
  let edges = [...model.edges];
  const selectedNode = nodesById.get(state.selectedId);
  if (selectedNode) {
    edges.sort((left, right) => {
      const leftIncident = left.source === selectedNode.node_id || left.target === selectedNode.node_id;
      const rightIncident = right.source === selectedNode.node_id || right.target === selectedNode.node_id;
      return Number(rightIncident) - Number(leftIncident) || left.edge_id.localeCompare(right.edge_id);
    });
  }
  for (const edge of edges.slice(0, MAX_RELATION_ROWS)) {
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = `relation-row ${edgeKind(edge)}`;
    if (state.selectedId === edge.edge_id) button.classList.add("selected");
    const relation = document.createElement("strong");
    relation.textContent = edge.edge_type.replaceAll("_", " ");
    const endpoints = document.createElement("span");
    endpoints.textContent = `${nodesById.get(edge.source)?.label || edge.source} → ${nodesById.get(edge.target)?.label || edge.target}`;
    const origin = document.createElement("small");
    origin.textContent = isDerived(edge) ? "Derived navigation" : "Human-authored relation";
    button.append(relation, endpoints, origin);
    button.addEventListener("click", () => selectItem(edge));
    row.append(button);
    list.append(row);
  }
  if (!list.childElementCount) {
    const empty = document.createElement("li");
    empty.className = "empty";
    empty.textContent = "No relations match these controls.";
    list.append(empty);
  }
}

function render() {
  if (!state.projection) return;
  const model = visibleModel();
  drawGraph(model);
  renderRelationList(model);
  const truncated = model.totalNodes > model.nodes.length || model.totalEdges > model.edges.length;
  document.querySelector("#graph-status").textContent =
    `${model.nodes.length} of ${model.totalNodes} nodes · ${model.edges.length} of ${model.totalEdges} relations${truncated ? " · narrow the search to explore omitted items" : ""}`;
}

function applyTransform() {
  const viewport = document.querySelector(".graph-viewport");
  if (viewport) viewport.setAttribute("transform", `translate(${state.transform.x} ${state.transform.y}) scale(${state.transform.scale})`);
}

function zoomAt(x, y, factor) {
  const oldScale = state.transform.scale;
  const nextScale = Math.min(3.5, Math.max(0.55, oldScale * factor));
  const graphX = (x - state.transform.x) / oldScale;
  const graphY = (y - state.transform.y) / oldScale;
  state.transform.x = x - graphX * nextScale;
  state.transform.y = y - graphY * nextScale;
  state.transform.scale = nextScale;
  applyTransform();
}

function resetCamera() {
  state.transform = { x: 0, y: 0, scale: 1 };
  applyTransform();
}

function canvasPoint(event) {
  const canvas = document.querySelector("#graph-canvas");
  const bounds = canvas.getBoundingClientRect();
  return {
    x: (event.clientX - bounds.left) * (VIEW_WIDTH / bounds.width),
    y: (event.clientY - bounds.top) * (VIEW_HEIGHT / bounds.height),
  };
}

function bindNavigation() {
  const canvas = document.querySelector("#graph-canvas");
  canvas.addEventListener("wheel", (event) => {
    event.preventDefault();
    const point = canvasPoint(event);
    zoomAt(point.x, point.y, event.deltaY < 0 ? 1.12 : 1 / 1.12);
  }, { passive: false });
  canvas.addEventListener("pointerdown", (event) => {
    if (event.target.closest(".graph-node, .graph-edge")) return;
    canvas.setPointerCapture(event.pointerId);
    state.drag = { pointerId: event.pointerId, clientX: event.clientX, clientY: event.clientY, x: state.transform.x, y: state.transform.y };
    canvas.classList.add("panning");
  });
  canvas.addEventListener("pointermove", (event) => {
    if (!state.drag || state.drag.pointerId !== event.pointerId) return;
    const bounds = canvas.getBoundingClientRect();
    state.transform.x = state.drag.x + (event.clientX - state.drag.clientX) * (VIEW_WIDTH / bounds.width);
    state.transform.y = state.drag.y + (event.clientY - state.drag.clientY) * (VIEW_HEIGHT / bounds.height);
    applyTransform();
  });
  const stopPanning = (event) => {
    if (!state.drag || state.drag.pointerId !== event.pointerId) return;
    state.drag = null;
    canvas.classList.remove("panning");
  };
  canvas.addEventListener("pointerup", stopPanning);
  canvas.addEventListener("pointercancel", stopPanning);
  canvas.addEventListener("click", (event) => {
    if (!event.target.closest(".graph-node, .graph-edge")) {
      state.selectedId = null;
      resetDetails();
      render();
    }
  });
}

async function start() {
  const response = await fetch("data/projection.json", { cache: "no-store" });
  if (!response.ok) throw new Error(`Projection load failed (${response.status})`);
  state.projection = await response.json();
  nodesById = new Map(state.projection.nodes.map((node) => [node.node_id, node]));
  document.querySelector("#snapshots").textContent =
    `Product ${state.projection.product_snapshot_id} · Candidates ${state.projection.candidate_snapshot_id} · ${state.projection.projection_version}`;
  document.querySelector("#search").addEventListener("input", (event) => { state.query = event.target.value; state.selectedId = null; render(); });
  document.querySelector("#view").addEventListener("change", (event) => { state.view = event.target.value; state.selectedId = null; resetCamera(); resetDetails(); render(); });
  document.querySelector("#derived").addEventListener("change", (event) => { state.showDerived = event.target.checked; state.selectedId = null; resetDetails(); render(); });
  document.querySelector("#zoom-in").addEventListener("click", () => zoomAt(VIEW_WIDTH / 2, VIEW_HEIGHT / 2, 1.2));
  document.querySelector("#zoom-out").addEventListener("click", () => zoomAt(VIEW_WIDTH / 2, VIEW_HEIGHT / 2, 1 / 1.2));
  document.querySelector("#reset-camera").addEventListener("click", resetCamera);
  bindNavigation();
  render();
}

start().catch((error) => {
  document.querySelector("#graph-status").textContent = error.message;
});
