import type { Agent, ResearchItem, Work } from "./types";

export type QueryMatcher = "text" | "word" | "regex";

export interface QueryTerm {
  raw: string;
  field: string | null;
  value: string;
  negated: boolean;
  matcher: QueryMatcher;
  regex?: RegExp;
  invalid?: boolean;
}

export interface ParsedQuery {
  terms: QueryTerm[];
  errors: string[];
}

const KNOWN_FIELDS = new Set([
  "title",
  "agent",
  "person",
  "contributor",
  "role",
  "type",
  "concept",
  "tag",
  "genre",
  "movement",
  "theme",
  "style",
  "medium",
  "country",
  "lang",
  "language",
  "id",
  "guide",
  "advisory",
  "year",
  "kind",
  "severity",
  "category",
  "batch",
  "path",
  "entity",
  "work",
  "quality",
  "similarity",
]);

const CONTRIBUTOR_ROLE_FIELDS = new Set([
  "author",
  "director",
  "screenwriter",
  "producer",
  "actor",
  "composer",
  "performer",
  "artist",
  "engraver",
  "sculptor",
  "photographer",
  "editor",
  "cinematographer",
  "production_company",
  "publisher",
  "record_label",
  "band",
  "distributor",
  "broadcaster",
  "platform",
  "translator",
  "illustrator",
  "printer",
  "curator",
  "choreographer",
  "narrator",
  "lyricist",
  "songwriter",
  "arranger",
  "sound_engineer",
  "designer",
  "animator",
]);

function contributorRoleField(field: string): string | null {
  const normalized = field.replaceAll("-", "_");
  return CONTRIBUTOR_ROLE_FIELDS.has(normalized) ? normalized : null;
}

function isKnownField(field: string): boolean {
  return KNOWN_FIELDS.has(field) || contributorRoleField(field) !== null;
}

function scanTokens(input: string): { tokens: string[]; errors: string[] } {
  const tokens: string[] = [];
  const errors: string[] = [];
  let index = 0;

  while (index < input.length) {
    while (/\s/u.test(input[index] ?? "")) index += 1;
    if (index >= input.length) break;

    const start = index;
    let quote: '"' | "'" | null = null;
    let regex = false;
    let regexClosed = false;
    let escaped = false;

    while (index < input.length) {
      const character = input[index];

      if (escaped) {
        escaped = false;
        index += 1;
        continue;
      }
      if (character === "\\") {
        escaped = true;
        index += 1;
        continue;
      }
      if (quote) {
        if (character === quote) quote = null;
        index += 1;
        continue;
      }
      if (regex) {
        if (!regexClosed) {
          if (character === "/") regexClosed = true;
          index += 1;
          continue;
        }
        if (/[a-z]/iu.test(character)) {
          index += 1;
          continue;
        }
        regex = false;
        if (/\s/u.test(character)) break;
        continue;
      }
      if (character === '"' || character === "'") {
        quote = character;
        index += 1;
        continue;
      }
      const prefix = input.slice(start, index).replace(/^-/, "").toLocaleLowerCase();
      if (character === "/" && (index === start || prefix === "regex:")) {
        regex = true;
        regexClosed = false;
        index += 1;
        continue;
      }
      if (/\s/u.test(character)) break;
      index += 1;
    }

    const token = input.slice(start, index);
    if (token) tokens.push(token);
    if (quote) errors.push(`Unclosed quote in ${token}`);
    if (regex && !regexClosed) errors.push(`Unclosed regular expression in ${token}`);
  }

  return { tokens, errors };
}

function stripQuotes(value: string): string {
  if (value.length >= 2) {
    const first = value[0];
    const last = value[value.length - 1];
    if ((first === '"' && last === '"') || (first === "'" && last === "'")) {
      return value.slice(1, -1).replace(/\\([\\"'])/gu, "$1");
    }
  }
  return value;
}

function parseRegex(value: string): { regex?: RegExp; error?: string } {
  if (!value.startsWith("/")) return {};

  let closing = -1;
  let escaped = false;
  for (let index = 1; index < value.length; index += 1) {
    const character = value[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (character === "\\") {
      escaped = true;
      continue;
    }
    if (character === "/") closing = index;
  }
  if (closing <= 0) return { error: `Invalid regular expression ${value}` };

  const pattern = value.slice(1, closing);
  const requestedFlags = value.slice(closing + 1);
  if (pattern.length > 256) return { error: "Regular expression is limited to 256 characters" };
  if (!/^[dgimsuvy]*$/u.test(requestedFlags)) {
    return { error: `Unsupported regular-expression flags: ${requestedFlags}` };
  }

  const flags = `${requestedFlags.includes("i") ? "" : "i"}${requestedFlags}`
    .replace(/[gy]/gu, "")
    .split("")
    .filter((flag, position, all) => all.indexOf(flag) === position)
    .join("");

  try {
    return { regex: new RegExp(pattern, flags) };
  } catch (cause) {
    return { error: cause instanceof Error ? cause.message : String(cause) };
  }
}

export function parseQuery(input: string): ParsedQuery {
  const scanned = scanTokens(input.trim());
  const errors = [...scanned.errors];
  const terms: QueryTerm[] = [];

  for (const rawToken of scanned.tokens) {
    let token = rawToken;
    let negated = false;
    if (token.startsWith("-") && token.length > 1) {
      negated = true;
      token = token.slice(1);
    }

    let field: string | null = null;
    let value = token;
    let requestedRegex = false;
    const colon = token.indexOf(":");
    if (colon > 0) {
      const candidate = token.slice(0, colon).toLocaleLowerCase();
      if (candidate === "word" || candidate === "exact") {
        value = token.slice(colon + 1);
      } else if (candidate === "regex") {
        value = token.slice(colon + 1);
        requestedRegex = true;
      } else {
        field = candidate;
        value = token.slice(colon + 1);
        if (!isKnownField(candidate)) errors.push(`Unknown search field: ${candidate}`);
      }
    }

    let matcher: QueryMatcher = "text";
    if (colon > 0 && ["word", "exact"].includes(token.slice(0, colon).toLocaleLowerCase())) {
      matcher = "word";
    }

    const regexResult =
      requestedRegex || (field === null && value.startsWith("/"))
        ? parseRegex(value)
        : {};
    if (regexResult.regex) matcher = "regex";
    if (regexResult.error) errors.push(regexResult.error);

    const normalizedValue = matcher === "regex" ? value : stripQuotes(value);
    if (!normalizedValue) {
      errors.push(`Empty search term: ${rawToken}`);
      continue;
    }

    terms.push({
      raw: rawToken,
      field,
      value: normalizedValue,
      negated,
      matcher,
      regex: regexResult.regex,
      invalid: Boolean(regexResult.error) || Boolean(field && !isKnownField(field)),
    });
  }

  return { terms, errors };
}

export function queryDiagnostics(input: string): string[] {
  return parseQuery(input).errors;
}

export function quoteQueryValue(value: string): string {
  if (/^[\p{L}\p{N}_.-]+$/u.test(value)) return value;
  return `"${value.replaceAll("\\", "\\\\").replaceAll('"', '\\"')}"`;
}

export function buildQueryToken(
  field: string,
  value: string,
  negated = false,
): string {
  return `${negated ? "-" : ""}${field}:${quoteQueryValue(value)}`;
}

function queryTermKey(term: QueryTerm): string {
  return [
    term.negated ? "not" : "yes",
    term.matcher,
    term.field ?? "",
    term.value.toLocaleLowerCase(),
  ].join("\u0000");
}

export function appendQueryTerms(current: string, addition: string): string {
  const combined: QueryTerm[] = [];
  const seen = new Set<string>();

  for (const term of [...parseQuery(current).terms, ...parseQuery(addition).terms]) {
    if (term.invalid) continue;
    const key = queryTermKey(term);
    if (seen.has(key)) continue;
    seen.add(key);
    combined.push(term);
  }

  return combined.map((term) => term.raw).join(" ");
}

export function removeQueryTermAt(input: string, index: number): string {
  return parseQuery(input).terms
    .filter((_, position) => position !== index)
    .map((term) => term.raw)
    .join(" ");
}

function resetAndTest(regex: RegExp, value: string): boolean {
  regex.lastIndex = 0;
  return regex.test(value);
}

function wordRegex(value: string): RegExp {
  const escaped = value.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
  return new RegExp(`(?<![\\p{L}\\p{N}_])${escaped}(?![\\p{L}\\p{N}_])`, "iu");
}

function matchesValues(term: QueryTerm, values: Array<string | null | undefined>): boolean {
  if (term.invalid) return false;
  const available = values.filter((value): value is string => Boolean(value));
  if (term.matcher === "regex" && term.regex) {
    return available.some((value) => resetAndTest(term.regex!, value));
  }
  if (term.matcher === "word") {
    const expression = wordRegex(term.value);
    return available.some((value) => expression.test(value));
  }
  const needle = term.value.toLocaleLowerCase();
  return available.some((value) => value.toLocaleLowerCase().includes(needle));
}

function numericCondition(expression: string, value: number | null | undefined): boolean {
  if (value === null || value === undefined || !Number.isFinite(value)) return false;
  const range = expression.match(/^(-?\d+(?:\.\d+)?)?\.\.(-?\d+(?:\.\d+)?)?$/u);
  if (range) {
    const minimum = range[1] === undefined ? null : Number(range[1]);
    const maximum = range[2] === undefined ? null : Number(range[2]);
    return (minimum === null || value >= minimum) && (maximum === null || value <= maximum);
  }
  const comparison = expression.match(/^(<=|>=|<|>|=)?\s*(-?\d+(?:\.\d+)?)$/u);
  if (!comparison) return false;
  const expected = Number(comparison[2]);
  switch (comparison[1] ?? "=") {
    case "<": return value < expected;
    case "<=": return value <= expected;
    case ">": return value > expected;
    case ">=": return value >= expected;
    default: return value === expected;
  }
}

function conceptGroupMatches(conceptType: string, field: string): boolean {
  const normalized = conceptType.toLocaleLowerCase().replaceAll("_", "-");
  const aliases: Record<string, string[]> = {
    genre: ["genre"],
    movement: ["movement", "school", "period", "scene"],
    theme: ["theme", "topic", "subject", "motif"],
    style: ["style", "technique", "form", "aesthetic"],
  };
  return (aliases[field] ?? [field]).some((alias) => normalized.includes(alias));
}

function guideMatches(work: Work, term: QueryTerm): boolean {
  const comparison = term.value.match(/^(.*?)(<=|>=|<|>|=)(\d(?:\.\d+)?)$/u);
  const label = comparison ? comparison[1].trim() : term.value;
  return work.advisories.some((advisory) => {
    if (!matchesValues({ ...term, value: label }, [advisory.label, advisory.category])) return false;
    return comparison ? numericCondition(`${comparison[2]}${comparison[3]}`, advisory.intensity) : true;
  });
}

function matchWorkTerm(work: Work, term: QueryTerm): boolean {
  const concepts = work.concepts;
  const contributors = work.contributors;
  const contributorRole = term.field ? contributorRoleField(term.field) : null;

  if (contributorRole) {
    return contributors.some(
      (contributor) =>
        contributor.role.toLocaleLowerCase() === contributorRole &&
        matchesValues(term, [
          contributor.label,
          contributor.creditedAs,
          contributor.id,
        ]),
    );
  }

  switch (term.field) {
    case null:
      return matchesValues(term, [
        work.label,
        work.medium,
        work.countryCode,
        work.languageCode,
        work.id,
        ...concepts.flatMap((concept) => [
          concept.label,
          concept.slug,
          concept.conceptType,
          concept.relationType,
          concept.centralityScale,
          concept.historicalRole,
        ]),
        ...contributors.flatMap((contributor) => [
          contributor.label,
          contributor.creditedAs,
          contributor.role,
          contributor.agentType,
        ]),
        ...work.advisories.flatMap((advisory) => [advisory.label, advisory.category]),
        ...work.identifiers.flatMap((identifier) => [identifier.scheme, identifier.value]),
        ...work.manifestations.flatMap((manifestation) => [
          manifestation.label,
          manifestation.type,
          manifestation.regionCode,
          manifestation.languageCode,
        ]),
      ]);
    case "title":
      return matchesValues(term, [work.label]);
    case "agent":
    case "person":
    case "contributor":
      return contributors.some((contributor) =>
        matchesValues(term, [contributor.label, contributor.creditedAs, contributor.id]));
    case "role":
      return contributors.some((contributor) => matchesValues(term, [contributor.role]));
    case "concept":
    case "tag":
      return concepts.some((concept) => matchesValues(term, [concept.label, concept.slug, concept.id]));
    case "genre":
    case "movement":
    case "theme":
    case "style":
      return concepts.some((concept) =>
        conceptGroupMatches(concept.conceptType, term.field!) &&
        matchesValues(term, [concept.label, concept.slug]));
    case "medium":
      return matchesValues(term, [work.medium]);
    case "country":
      return matchesValues(term, [work.countryCode]);
    case "lang":
    case "language":
      return matchesValues(term, [work.languageCode]);
    case "id":
      return matchesValues(term, [
        work.id,
        ...work.identifiers.flatMap((identifier) => [identifier.scheme, identifier.value]),
      ]);
    case "guide":
    case "advisory":
      return guideMatches(work, term);
    case "year":
      return numericCondition(term.value, work.yearStart);
    default:
      return false;
  }
}

export function matchesWorkQuery(work: Work, parsed: ParsedQuery): boolean {
  return parsed.terms.every((term) => {
    const matched = matchWorkTerm(work, term);
    return term.negated ? !matched : matched;
  });
}

function textScore(value: string, term: QueryTerm, exact: number, prefix: number, partial: number): number {
  if (term.matcher !== "text") return matchesValues(term, [value]) ? partial : 0;
  const candidate = value.toLocaleLowerCase();
  const query = term.value.toLocaleLowerCase();
  if (candidate === query) return exact;
  if (candidate.startsWith(query)) return prefix;
  return candidate.includes(query) ? partial : 0;
}

function scoreWorkTerm(work: Work, term: QueryTerm): number {
  if (term.negated || !matchWorkTerm(work, term)) return 0;
  if (term.field === "title") return textScore(work.label, term, 14, 10, 7);
  if (["agent", "person", "contributor"].includes(term.field ?? "")) {
    return Math.max(0, ...work.contributors.map((item) => textScore(item.label, term, 8, 6, 4)));
  }
  const contributorRole = term.field ? contributorRoleField(term.field) : null;
  if (contributorRole) {
    return Math.max(
      0,
      ...work.contributors
        .filter((item) => item.role.toLocaleLowerCase() === contributorRole)
        .map((item) => textScore(item.label, term, 9, 7, 5)),
    );
  }
  if (["concept", "tag", "genre", "movement", "theme", "style"].includes(term.field ?? "")) {
    return Math.max(0, ...work.concepts.map((item) => textScore(item.label, term, 8, 6, 4)));
  }
  if (term.field) return 3;

  return Math.max(
    textScore(work.label, term, 12, 8, 5),
    ...work.contributors.map((item) => textScore(item.label, term, 6, 4, 2)),
    ...work.concepts.map((item) => textScore(item.label, term, 6, 4, 2)),
    1,
  );
}

export function scoreWorkQuery(work: Work, parsed: ParsedQuery): number {
  return parsed.terms.reduce((total, term) => total + scoreWorkTerm(work, term), 0);
}

export interface AgentQueryDocument {
  agent: Agent;
  roles: string[];
  knownWorkLabels: string[];
}

function matchAgentTerm(document: AgentQueryDocument, term: QueryTerm): boolean {
  const { agent, roles, knownWorkLabels } = document;
  const contributorRole = term.field ? contributorRoleField(term.field) : null;
  if (contributorRole) {
    return roles.some((role) => role.toLocaleLowerCase() === contributorRole) &&
      matchesValues(term, [agent.label, agent.id]);
  }

  switch (term.field) {
    case null:
      return matchesValues(term, [
        agent.label,
        agent.agentType,
        agent.id,
        ...roles,
        ...knownWorkLabels,
        ...agent.identifiers.flatMap((identifier) => [identifier.scheme, identifier.value]),
      ]);
    case "title":
    case "agent":
    case "person":
    case "contributor":
      return matchesValues(term, [agent.label]);
    case "type":
      return matchesValues(term, [agent.agentType]);
    case "role":
      return matchesValues(term, roles);
    case "id":
      return matchesValues(term, [
        agent.id,
        ...agent.identifiers.flatMap((identifier) => [identifier.scheme, identifier.value]),
      ]);
    case "work":
      return matchesValues(term, knownWorkLabels);
    default:
      return false;
  }
}

export function matchesAgentQuery(
  document: AgentQueryDocument,
  parsed: ParsedQuery,
): boolean {
  return parsed.terms.every((term) => {
    const matched = matchAgentTerm(document, term);
    return term.negated ? !matched : matched;
  });
}

export function scoreAgentQuery(
  document: AgentQueryDocument,
  parsed: ParsedQuery,
): number {
  return parsed.terms.reduce((total, term) => {
    if (term.negated || !matchAgentTerm(document, term)) return total;
    if (["agent", "person", "contributor", "title"].includes(term.field ?? "")) {
      return total + textScore(document.agent.label, term, 14, 10, 7);
    }
    if (term.field === "type" || term.field === "role") return total + 6;
    if (term.field === "id") return total + 10;
    if (term.field) return total + 3;
    return total + Math.max(
      textScore(document.agent.label, term, 12, 8, 5),
      ...document.roles.map((role) => textScore(role, term, 5, 4, 3)),
      ...document.knownWorkLabels.map((label) => textScore(label, term, 4, 3, 2)),
      1,
    );
  }, 0);
}

function matchResearchTerm(item: ResearchItem, term: QueryTerm): boolean {
  switch (term.field) {
    case null:
      return matchesValues(term, [
        item.title,
        item.message,
        item.kind,
        item.severity,
        item.category,
        item.batchId,
        item.jsonPath,
        item.workId,
        item.workLabel,
        item.leftId,
        item.leftLabel,
        item.rightId,
        item.rightLabel,
        item.entityType,
        ...(item.details ?? []),
      ]);
    case "title":
      return matchesValues(term, [item.title]);
    case "kind":
      return matchesValues(term, [item.kind]);
    case "severity":
      return matchesValues(term, [item.severity]);
    case "category":
      return matchesValues(term, [item.category]);
    case "batch":
      return matchesValues(term, [item.batchId]);
    case "path":
      return matchesValues(term, [item.jsonPath]);
    case "entity":
      return matchesValues(term, [item.entityType]);
    case "id":
      return matchesValues(term, [item.id, item.workId, item.leftId, item.rightId]);
    case "work":
      return matchesValues(term, [
        item.workId,
        item.workLabel,
        item.leftId,
        item.leftLabel,
        item.rightId,
        item.rightLabel,
      ]);
    case "quality":
      return numericCondition(term.value, item.score);
    case "similarity":
      return numericCondition(term.value, item.similarityScore);
    default:
      return false;
  }
}

export function matchesResearchQuery(item: ResearchItem, parsed: ParsedQuery): boolean {
  return parsed.terms.every((term) => {
    const matched = matchResearchTerm(item, term);
    return term.negated ? !matched : matched;
  });
}
