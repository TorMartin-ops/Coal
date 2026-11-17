"""Summary table, JSON and JUnit XML."""
import json
import re
import statistics
import time
from xml.sax.saxutils import escape, quoteattr

from .model import BAD, PASS, SKIP


_CONTROL = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff]")


def _xml(text):
    """Text that is legal inside an XML document: control characters from a guest's output are dropped."""
    return escape(_CONTROL.sub("?", text))


def _attr(text):
    return quoteattr(_CONTROL.sub("?", text))


def _median(values):
    return statistics.median(values) if values else 0


def row(res):
    runs = [r for r in res.runs if r.verdict != SKIP]
    cases = next((r.cases for r in res.runs if r.cases is not None), None)
    return {
        "id": res.test.id, "suite": res.test.suite, "name": res.test.name, "verdict": res.verdict,
        "detail": res.detail, "runs": len(res.runs), "cases": cases,
        "guest_ms": round(_median([r.guest_ns for r in runs]) / 1e6, 1),
        "guest_minsn": round(_median([r.guest_insn for r in runs]) / 1e6, 2),
        "host_s": round(_median([r.host_s for r in runs]), 2),
        "output_hash": res.runs[0].output_hash if res.runs else "",
        "deterministic": res.test.deterministic, "seeds": sorted({r.seed for r in res.runs}),
        "bundle": res.bundle, "diff": res.diff,
    }


def table(results):
    rows = [row(r) for r in results]
    width = max([len(r["id"]) for r in rows] + [10])
    lines = ["%-14s %-*s %5s %7s %10s %9s %8s" % ("RESULT", width, "TEST", "RUNS", "CASES", "GUEST ms", "GUEST Minsn",
                                                      "HOST s")]
    for r in rows:
        lines.append("%-14s %-*s %5d %7s %10.1f %9.2f %8.2f" % (
            r["verdict"], width, r["id"], r["runs"], r["cases"] if r["cases"] is not None else "-",
            r["guest_ms"], r["guest_minsn"], r["host_s"]))
        if r["verdict"] in BAD or r["verdict"] == SKIP:
            lines.append("    %s" % r["detail"])
            if r["diff"]:
                lines += ["    " + l for l in r["diff"].splitlines()[:40]]
            if r["bundle"]:
                lines.append("    repro bundle: %s" % r["bundle"])
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    lines.append("")
    lines.append("%d tests: %s; %d checks/cases counted" % (
        len(rows), ", ".join("%d %s" % (n, v) for v, n in sorted(counts.items())),
        sum(r["cases"] or 0 for r in rows)))
    return "\n".join(lines)


def to_json(results, meta):
    return json.dumps({"meta": meta, "tests": [row(r) for r in results]}, indent=2, sort_keys=True)


def to_junit(results):
    by_suite = {}
    for r in results:
        by_suite.setdefault(r.test.suite, []).append(r)
    out = ['<?xml version="1.0" encoding="UTF-8"?>', "<testsuites>"]
    for suite, rs in by_suite.items():
        bad = sum(r.verdict in BAD for r in rs)
        skipped = sum(r.verdict == SKIP for r in rs)
        total_s = sum(row(r)["host_s"] for r in rs)
        out.append('<testsuite name=%s tests="%d" failures="%d" skipped="%d" time="%.2f" timestamp=%s>' % (
            _attr(suite), len(rs), bad, skipped, total_s, _attr(time.strftime("%Y-%m-%dT%H:%M:%S"))))
        for r in rs:
            d = row(r)
            out.append('  <testcase classname=%s name=%s time="%.2f">' % (_attr(suite), _attr(r.test.name),
                                                                        d["host_s"]))
            if r.verdict == SKIP:
                out.append("    <skipped message=%s/>" % _attr(r.detail))
            elif r.verdict in BAD:
                out.append("    <failure type=%s message=%s>%s</failure>" % (
                    _attr(r.verdict), _attr(r.detail), _xml(r.diff or (r.runs[-1].output[-3000:] if r.runs else ""))))
            out.append("    <system-out>%s</system-out>" % _xml(
                "guest_ms=%s guest_minsn=%s cases=%s hash=%s bundle=%s" % (
                    d["guest_ms"], d["guest_minsn"], d["cases"], d["output_hash"], d["bundle"])))
            out.append("  </testcase>")
        out.append("</testsuite>")
    out.append("</testsuites>")
    return "\n".join(out) + "\n"
