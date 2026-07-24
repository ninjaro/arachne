import type { Domain, Projection, Work } from "./types";

function numberAttribute(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) ? value : null;
}

export async function loadProjection(): Promise<Projection> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/projection.json`, {
    cache: "no-store",
  });
  if (!response.ok) {
    throw new Error(`Projection load failed (${response.status})`);
  }
  const value = (await response.json()) as Projection;
  if (
    value?.artifact_type !== "viewer_projection_data_v1" ||
    value.format_version !== 1 ||
    !Array.isArray(value.nodes) ||
    !Array.isArray(value.edges)
  ) {
    throw new Error("Unsupported or invalid projection.json");
  }
  return value;
}

export function buildDomain(projection: Projection): Domain {
  const nodeById = new Map(projection.nodes.map((node) => [node.node_id, node]));
  const works: Work[] = projection.nodes
    .filter((node) => node.graph_domain === "product" && node.node_type === "work")
    .map((node) => ({
      id: node.node_id,
      label: node.label,
      medium: String(node.attributes?.medium ?? "unknown"),
      year:
        numberAttribute(node.attributes?.year_start) ??
        numberAttribute(node.attributes?.year),
      concepts: [],
      contributors: [],
    }));
  const workById = new Map(works.map((work) => [work.id, work]));

  for (const edge of projection.edges) {
    const source = nodeById.get(edge.source);
    const target = nodeById.get(edge.target);
    if (!source || !target) continue;

    const conceptWork =
      source.node_type === "work" && target.node_type === "concept"
        ? { work: workById.get(source.node_id), concept: target }
        : target.node_type === "work" && source.node_type === "concept"
          ? { work: workById.get(target.node_id), concept: source }
          : null;
    if (conceptWork?.work) {
      conceptWork.work.concepts.push({
        id: conceptWork.concept.node_id,
        label: conceptWork.concept.label,
        relation: edge.edge_type,
      });
      continue;
    }

    const credit = edge.edge_type.startsWith("credit:");
    if (credit) {
      const work =
        source.node_type === "work"
          ? workById.get(source.node_id)
          : target.node_type === "work"
            ? workById.get(target.node_id)
            : undefined;
      const contributor =
        source.node_type === "work" ? target : target.node_type === "work" ? source : null;
      if (work && contributor) {
        work.contributors.push({
          id: contributor.node_id,
          label: contributor.label,
          role: edge.edge_type.slice("credit:".length),
        });
      }
    }
  }

  for (const work of works) {
    work.concepts.sort((a, b) => a.label.localeCompare(b.label));
    work.contributors.sort(
      (a, b) => a.role.localeCompare(b.role) || a.label.localeCompare(b.label),
    );
  }
  works.sort(
    (a, b) =>
      (a.year ?? Number.MAX_SAFE_INTEGER) - (b.year ?? Number.MAX_SAFE_INTEGER) ||
      a.label.localeCompare(b.label),
  );
  return { works, workById, nodeById };
}
