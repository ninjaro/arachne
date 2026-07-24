import { useMemo } from "react";
import type { Domain, Ratings, Settings } from "../lib/types";
import type { FeatureIndex } from "../lib/features";
import { factorPhrase } from "../lib/features";
import { scoreRecommendations } from "../lib/recommendations";
import { RatingButtons } from "../components/common";
import type { OpenHandler, RateHandler } from "../components/common";
import { dateLabel, humanize } from "../lib/format";

export function RecommendationsView({
  domain,
  index,
  ratings,
  settings,
  onOpen,
  onRate,
}: {
  domain: Domain;
  index: FeatureIndex;
  ratings: Ratings;
  settings: Settings;
  onOpen: OpenHandler;
  onRate: RateHandler;
}) {
  const likedCount = Object.values(ratings).filter((value) => value === 1).length;
  const scored = useMemo(
    () => scoreRecommendations(domain, index, ratings, settings),
    [domain, index, ratings, settings],
  );

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
        <span>{scored.length.toLocaleString()} shown</span>
      </div>
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
            {scored.map((result) => (
              <tr
                key={result.work.id}
                tabIndex={0}
                onClick={() => onOpen(result.work.id)}
                onKeyDown={(event) => {
                  if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    onOpen(result.work.id);
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
    </section>
  );
}
