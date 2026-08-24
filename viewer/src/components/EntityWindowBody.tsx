import { useMemo } from "react";
import type {
  Agent,
  Domain,
  EntityId,
  Identifier,
  Ratings,
  Work,
} from "../lib/types";
import {
  dateLabel,
  externalUrl,
  humanize,
  moneyLabel,
  schemeLabel,
} from "../lib/format";
import { buildQueryToken } from "../lib/query";
import { manifestationCreditsForAgent } from "../lib/agent-credits";
import type { ImageHintProductIdentity } from "../lib/image-hints";
import type { RateHandler } from "./common";
import {
  EntityRatingButtons,
  GroupedConceptChips,
  RatingButtons,
} from "./common";
import { EntityImageCarousel } from "./ImageCarousel";

interface FilterMenuOption {
  label: string;
  query?: string;
  onSelect?: () => void;
}

function InlineFilterMenu({
  label,
  options,
}: {
  label: string;
  options: FilterMenuOption[];
}) {
  return (
    <details className="inline-filter-menu">
      <summary>{label}</summary>
      <div className="inline-filter-options">
        {options.map((option) => (
          <button
            type="button"
            key={`${option.label}:${option.query ?? "action"}`}
            data-query={option.query}
            onClick={option.onSelect}
          >
            {option.label}
          </button>
        ))}
      </div>
    </details>
  );
}

function IdentifierSection({ identifiers }: { identifiers: Identifier[] }) {
  if (!identifiers.length) return null;
  return (
    <section>
      <h3>Identifiers</h3>
      <ul className="plain-list">
        {identifiers.map((identifier) => {
          const url = externalUrl(
            identifier.scheme,
            identifier.value,
            identifier.url,
          );
          return (
            <li key={`${identifier.scheme}:${identifier.value}`}>
              {url ? (
                <a
                  href={url}
                  target="_blank"
                  rel="noreferrer"
                  onClick={(event) => event.stopPropagation()}
                >
                  {schemeLabel(identifier.scheme)}: {identifier.value}
                </a>
              ) : (
                <>
                  {schemeLabel(identifier.scheme)}: {identifier.value}
                </>
              )}
            </li>
          );
        })}
      </ul>
    </section>
  );
}

export function WorkEntityBody({
  work,
  domain,
  ratings,
  onRate,
  onSearch,
  onOpen,
  imageHintsUrl,
  imageHintProduct,
}: {
  work: Work;
  domain: Domain;
  ratings: Ratings;
  onRate: RateHandler;
  onSearch: (query: string) => void;
  onOpen: (id: EntityId) => void;
  imageHintsUrl: string;
  imageHintProduct: ImageHintProductIdentity;
}) {
  const memberships = domain.workMemberships.filter(
    (membership) =>
      membership.childId === work.id || membership.parentId === work.id,
  );
  return (
    <>
      <div className="window-meta">
        <span>{dateLabel(work)}</span>
        <button
          type="button"
          className="meta-filter-link"
          data-query={buildQueryToken("medium", work.medium)}
        >
          {humanize(work.medium)}
        </button>
        {work.countryCode ? (
          <button
            type="button"
            className="meta-filter-link"
            data-query={buildQueryToken("country", work.countryCode)}
          >
            {work.countryCode}
          </button>
        ) : null}
        {work.languageCode ? (
          <button
            type="button"
            className="meta-filter-link"
            data-query={buildQueryToken("lang", work.languageCode)}
          >
            {work.languageCode}
          </button>
        ) : null}
      </div>

      <EntityImageCarousel
        entity={{
          id: work.id,
          family: "work",
          identifiers: work.identifiers,
          medium: work.medium,
        }}
        label={work.label}
        imageHintsUrl={imageHintsUrl}
        imageHintProduct={imageHintProduct}
      />

      <RatingButtons work={work} ratings={ratings} onRate={onRate} />

      {work.concepts.length ? (
        <section>
          <h3>Concepts</h3>
          <GroupedConceptChips
            concepts={work.concepts}
            onFilter={(concept) =>
              onSearch(buildQueryToken("tag", concept.label))
            }
          />
        </section>
      ) : null}

      {work.contributors.length ? (
        <section>
          <h3>Contributors</h3>
          <dl className="detail-list">
            {work.contributors.map((contributor) => (
              <div key={`${contributor.role}:${contributor.id}`}>
                <dt>{humanize(contributor.role)}</dt>
                <dd>
                  <InlineFilterMenu
                    label={contributor.label}
                    options={[
                      {
                        label: "Open agent card",
                        onSelect: () => onOpen(contributor.id),
                      },
                      {
                        label: "Filter by this person",
                        query: buildQueryToken("agent", contributor.label),
                      },
                      {
                        label: "Exclude this person",
                        query: buildQueryToken(
                          "agent",
                          contributor.label,
                          true,
                        ),
                      },
                      {
                        label: `Filter as ${humanize(contributor.role)}`,
                        query: buildQueryToken(
                          contributor.role,
                          contributor.label,
                        ),
                      },
                    ]}
                  />
                  {contributor.creditedAs &&
                  contributor.creditedAs !== contributor.label
                    ? ` as ${contributor.creditedAs}`
                    : ""}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ) : null}

      {memberships.length ? (
        <section>
          <h3>Structure</h3>
          <dl className="detail-list">
            {memberships.map((membership) => {
              const outgoing = membership.childId === work.id;
              const peerId = outgoing ? membership.parentId : membership.childId;
              const peer = domain.workById.get(peerId);
              return (
                <div key={membership.id}>
                  <dt>{humanize(outgoing ? membership.membershipType : `contains_${membership.membershipType}`)}</dt>
                  <dd>
                    <button
                      type="button"
                      className="detail-filter-link"
                      onClick={() => onOpen(peerId)}
                    >
                      {peer?.label ?? peerId}
                    </button>
                    {membership.positionText
                      ? ` · ${membership.positionText}`
                      : membership.position !== null
                        ? ` · #${membership.position}`
                        : ""}
                  </dd>
                </div>
              );
            })}
          </dl>
        </section>
      ) : null}

      {work.events.length ? (
        <section>
          <h3>Events</h3>
          <dl className="detail-list">
            {work.events.map((event) => (
              <div key={event.id}>
                <dt>{humanize(event.eventType)}</dt>
                <dd>
                  {event.dateText ?? event.yearStart ?? "Date unknown"}
                  {event.yearEnd !== null && event.yearEnd !== event.yearStart
                    ? `–${event.yearEnd}`
                    : ""}
                  {event.placeText ? ` · ${event.placeText}` : ""}
                  {event.datePrecision ? ` · ${humanize(event.datePrecision)}` : ""}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ) : null}

      {work.advisories.length ? (
        <section>
          <h3>Content guide</h3>
          <dl className="detail-list content-guide-list">
            {work.advisories.map((advisory) => (
              <div key={advisory.id}>
                <dt>
                  <button
                    type="button"
                    className="detail-filter-link"
                    data-query={buildQueryToken("guide", advisory.category)}
                  >
                    {humanize(advisory.category)}
                  </button>
                </dt>
                <dd>
                  <span
                    className="guide-meter"
                    aria-label={`Intensity ${advisory.intensity ?? "unknown"} out of 5`}
                  >
                    {Array.from({ length: 5 }, (_, index) => (
                      <i
                        key={index}
                        className={
                          advisory.intensity !== null &&
                          index < advisory.intensity
                            ? "active"
                            : ""
                        }
                      />
                    ))}
                  </span>
                  <span>{advisory.label}</span>
                  <details>
                    <summary>Details</summary>
                    <dl className="advisory-details">
                      {advisory.intensity !== null ? (
                        <div>
                          <dt>Intensity</dt>
                          <dd>{advisory.intensity}/5</dd>
                        </div>
                      ) : null}
                      {advisory.frequency !== null ? (
                        <div>
                          <dt>Frequency</dt>
                          <dd>{advisory.frequency}/5</dd>
                        </div>
                      ) : null}
                      {advisory.explicitness !== null ? (
                        <div>
                          <dt>Explicitness</dt>
                          <dd>{advisory.explicitness}/5</dd>
                        </div>
                      ) : null}
                      {advisory.realism !== null ? (
                        <div>
                          <dt>Realism</dt>
                          <dd>{advisory.realism}/5</dd>
                        </div>
                      ) : null}
                      {advisory.confidence !== null ? (
                        <div>
                          <dt>Confidence</dt>
                          <dd>{Math.round(advisory.confidence * 100)}%</dd>
                        </div>
                      ) : null}
                    </dl>
                  </details>
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ) : null}

      {work.measurements.length ? (
        <section>
          <h3>Measurements</h3>
          <dl className="detail-list">
            {work.measurements.map((measurement, index) => (
              <div key={`${measurement.type}:${index}`}>
                <dt>{humanize(measurement.type)}</dt>
                <dd>
                  {measurement.value}
                  {measurement.unit ? ` ${measurement.unit}` : ""}
                  {measurement.qualifier
                    ? ` (${measurement.qualifier})`
                    : ""}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ) : null}

      {work.financialFacts.length ? (
        <section>
          <h3>Financial facts</h3>
          <dl className="detail-list">
            {work.financialFacts.map((fact, index) => (
              <div key={`${fact.type}:${index}`}>
                <dt>{humanize(fact.type)}</dt>
                <dd>
                  {moneyLabel(
                    fact.amountMin,
                    fact.amountMax,
                    fact.currencyCode,
                  )}
                  {fact.valueYear ? ` (${fact.valueYear})` : ""}
                  {fact.isEstimate ? " · estimate" : ""}
                </dd>
              </div>
            ))}
          </dl>
        </section>
      ) : null}

      {work.manifestations.length ? (
        <section>
          <h3>Manifestations</h3>
          <ul className="plain-list">
            {work.manifestations.map((manifestation) => (
              <li key={manifestation.id}>
                {manifestation.label || humanize(manifestation.type)}
                {manifestation.releaseYear
                  ? ` · ${manifestation.releaseYear}`
                  : ""}
                {manifestation.regionCode
                  ? ` · ${manifestation.regionCode}`
                  : ""}
                {manifestation.contributors.length ? (
                  <ul className="plain-list">
                    {manifestation.contributors.map((contributor, index) => (
                      <li key={`${contributor.id}:${contributor.role}:${index}`}>
                        {humanize(contributor.role)}: {contributor.label}
                      </li>
                    ))}
                  </ul>
                ) : null}
                {manifestation.events.length ? (
                  <ul className="plain-list">
                    {manifestation.events.map((event) => (
                      <li key={event.id}>
                        {humanize(event.eventType)}: {event.dateText ?? event.yearStart ?? "date unknown"}
                        {event.placeText ? ` · ${event.placeText}` : ""}
                      </li>
                    ))}
                  </ul>
                ) : null}
              </li>
            ))}
          </ul>
        </section>
      ) : null}

      <IdentifierSection identifiers={work.identifiers} />
    </>
  );
}

export function AgentEntityBody({
  agent,
  domain,
  ratings,
  onRate,
  onOpen,
  imageHintsUrl,
  imageHintProduct,
}: {
  agent: Agent;
  domain: Domain;
  ratings: Ratings;
  onRate: RateHandler;
  onOpen: (id: EntityId) => void;
  imageHintsUrl: string;
  imageHintProduct: ImageHintProductIdentity;
}) {
  const creditedWorks = useMemo(
    () =>
      domain.works.filter((work) =>
        work.contributors.some((contributor) => contributor.id === agent.id),
      ),
    [agent.id, domain.works],
  );
  const manifestationCredits = useMemo(
    () => manifestationCreditsForAgent(domain.works, agent.id),
    [agent.id, domain.works],
  );
  const visibleWorks = creditedWorks.slice(0, 100);
  const visibleManifestationCredits = manifestationCredits.slice(0, 100);
  const relations = domain.agentRelations.filter(
    (relation) => relation.subjectId === agent.id || relation.objectId === agent.id,
  );

  return (
    <>
      <div className="window-meta">
        <span>Agent</span>
        <span>{humanize(agent.agentType)}</span>
      </div>

      <EntityRatingButtons
        id={agent.id}
        label={agent.label}
        ratings={ratings}
        onRate={onRate}
      />

      <EntityImageCarousel
        entity={{
          id: agent.id,
          family: "agent",
          identifiers: agent.identifiers,
          agentType: agent.agentType,
        }}
        label={agent.label}
        imageHintsUrl={imageHintsUrl}
        imageHintProduct={imageHintProduct}
      />

      {creditedWorks.length ? (
        <section>
          <h3>Credited works ({creditedWorks.length.toLocaleString()})</h3>
          <ul className="plain-list agent-work-list">
            {visibleWorks.map((work) => (
              <li key={work.id}>
                <button
                  type="button"
                  className="detail-filter-link"
                  onClick={(event) => {
                    event.stopPropagation();
                    onOpen(work.id);
                  }}
                >
                  {work.label}
                </button>
                {dateLabel(work) ? ` · ${dateLabel(work)}` : ""}
              </li>
            ))}
          </ul>
          {creditedWorks.length > visibleWorks.length ? (
            <p className="window-note">
              Showing the first {visibleWorks.length.toLocaleString()} works.
            </p>
          ) : null}
        </section>
      ) : null}

      {manifestationCredits.length ? (
        <section>
          <h3>
            Manifestation credits ({manifestationCredits.length.toLocaleString()})
          </h3>
          <ul className="plain-list agent-work-list">
            {visibleManifestationCredits.map((reference, index) => (
              <li
                key={`${reference.manifestation.id}:${reference.contributor.role}:${index}`}
              >
                <button
                  type="button"
                  className="detail-filter-link"
                  onClick={(event) => {
                    event.stopPropagation();
                    onOpen(reference.work.id);
                  }}
                >
                  {reference.work.label}
                </button>
                {` · ${reference.manifestation.label || humanize(reference.manifestation.type)}`}
                {` · ${humanize(reference.contributor.role)}`}
                {reference.contributor.creditedAs &&
                reference.contributor.creditedAs !== reference.contributor.label
                  ? ` as ${reference.contributor.creditedAs}`
                  : ""}
              </li>
            ))}
          </ul>
          {manifestationCredits.length > visibleManifestationCredits.length ? (
            <p className="window-note">
              Showing the first {visibleManifestationCredits.length.toLocaleString()} credits.
            </p>
          ) : null}
        </section>
      ) : null}

      {relations.length ? (
        <section>
          <h3>Relationships</h3>
          <dl className="detail-list">
            {relations.map((relation) => {
              const outgoing = relation.subjectId === agent.id;
              const peerId = outgoing ? relation.objectId : relation.subjectId;
              const peer = domain.agentById.get(peerId);
              return (
                <div key={relation.id}>
                  <dt>{humanize(outgoing ? relation.relationType : `inverse_${relation.relationType}`)}</dt>
                  <dd>
                    <button
                      type="button"
                      className="detail-filter-link"
                      onClick={() => onOpen(peerId)}
                    >
                      {peer?.label ?? peerId}
                    </button>
                    {relation.roleText ? ` · ${relation.roleText}` : ""}
                    {relation.periodText
                      ? ` · ${relation.periodText}`
                      : relation.fromYear !== null
                        ? ` · ${relation.fromYear}${relation.toYear !== null ? `–${relation.toYear}` : ""}`
                        : ""}
                  </dd>
                </div>
              );
            })}
          </dl>
        </section>
      ) : null}

      <IdentifierSection identifiers={agent.identifiers} />
    </>
  );
}
