import { useEffect, useMemo, useState } from "react";
import type {
  Domain,
  EntityOpenContext,
  Ratings,
  Settings,
} from "../lib/types";
import type { FeatureIndex } from "../lib/features";
import { factorPhrase } from "../lib/features";
import { scoreRecommendations } from "../lib/recommendations";
import { Pagination, RatingButtons } from "../components/common";
import type { OpenHandler, RateHandler } from "../components/common";
import { dateLabel, humanize } from "../lib/format";
import type { TasteIndex } from "../lib/taste";

export function RecommendationsView({
  domain,
  index,
  ratings,
  settings,
  tasteIndex,
  onOpen,
  onRate,
}: {
  domain: Domain;
  index: FeatureIndex;
  ratings: Ratings;
  settings: Settings;
  tasteIndex: TasteIndex;
  onOpen: OpenHandler;
  onRate: RateHandler;
}) {
  const likedCount = Object.values(ratings).filter((value) => value === 1).length;
  const scored = useMemo(
    () => scoreRecommendations(domain, index, ratings, settings, tasteIndex),
    [domain, index, ratings, settings, tasteIndex],
  );
  const [page, setPage] = useState(1);
  const [pageSize, setPageSize] = useState(settings.browse.defaultPageSize);
  const pageCount = Math.max(1, Math.ceil(scored.length / pageSize));
  const safePage = Math.min(page, pageCount);
  const pageItems = scored.slice(
    (safePage - 1) * pageSize,
    safePage * pageSize,
  );

  useEffect(() => setPage(1), [ratings]);

  function recommendationContext(
    result: (typeof scored)[number],
  ): EntityOpenContext {
    return {
      kind: "recommendation",
      title: `Why this recommendation · ${result.score.toFixed(2)}`,
      details: [
        ...result.positive.map((factor) => `+ ${factorPhrase(factor)}`),
        ...result.negative.map((factor) => `− ${factorPhrase(factor)}`),
      ],
    };
  }

  function openRecommendation(result: (typeof scored)[number]) {
    onOpen(result.work.id, recommendationContext(result));
  }

  if (!likedCount) {
    return (
      <section className="empty">
        Like several works in Browse first. Likes build a feature profile from
        concepts, contributors, and content-guide values; dislikes subtract.
      </section>
    );
  }

  if (!scored.length) {
    return (
      <section className="empty">
        No unrated works currently have a positive recommendation score.
      </section>
    );
  }

  return (
    <section>
      <div className="section-heading">
        <h2>Recommendations</h2>
        <span>{scored.length.toLocaleString()} experimental matches</span>
      </div>
      <Pagination
        page={safePage}
        pageCount={pageCount}
        total={scored.length}
        pageSize={pageSize}
        pageSizeOptions={settings.browse.pageSizeOptions}
        onPage={setPage}
        onPageSize={(value) => {
          setPageSize(value);
          setPage(1);
        }}
      />
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Score</th>
              <th>Date</th>
              <th>Work</th>
              <th>Medium</th>
              <th>Why</th>
              <th>Rating</th>
            </tr>
          </thead>
          <tbody>
            {pageItems.map((result) => (
              <tr
                key={result.work.id}
                tabIndex={0}
                onClick={() => openRecommendation(result)}
                onKeyDown={(event) => {
                  if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    openRecommendation(result);
                  }
                }}
              >
                <td className="score-cell">{result.score.toFixed(2)}</td>
                <td className="date-cell">{dateLabel(result.work)}</td>
                <td className="label-cell">{result.work.label}</td>
                <td>{humanize(result.work.medium)}</td>
                <td className="why-cell">
                  {result.positive.slice(0, 3).map((factor) => (
                    <span className="evidence positive" key={factor.id}>
                      {factorPhrase(factor)}
                    </span>
                  ))}
                  {result.negative[0] ? (
                    <span className="evidence negative">
                      − {factorPhrase(result.negative[0])}
                    </span>
                  ) : null}
                </td>
                <td>
                  <RatingButtons
                    work={result.work}
                    ratings={ratings}
                    onRate={onRate}
                  />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <Pagination
        page={safePage}
        pageCount={pageCount}
        total={scored.length}
        pageSize={pageSize}
        pageSizeOptions={settings.browse.pageSizeOptions}
        onPage={setPage}
        onPageSize={(value) => {
          setPageSize(value);
          setPage(1);
        }}
      />
    </section>
  );
}
