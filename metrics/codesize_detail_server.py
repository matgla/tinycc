#!/usr/bin/env python3
"""Tiny file/function code-size comparison viewer.

This serves the separate detail DB populated by:

  metrics/record.py --detail-db /var/lib/tcc-metrics/codesize-detail.db

It intentionally stays outside Grafana.  Grafana is good for commit-level
summaries; this is for high-cardinality file/function drilldown when a summary
panel says code size moved.
"""

import argparse
import html
import sqlite3
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, unquote, urlparse


def h(text) -> str:
    return html.escape("" if text is None else str(text), quote=True)


def qs(params: dict) -> str:
    return "&".join(f"{quote(str(k))}={quote(str(v))}" for k, v in params.items())


def short_sha(sha: str) -> str:
    return sha[:12] if sha else "?"


def connect(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    conn.row_factory = sqlite3.Row
    return conn


class CodesizeHandler(BaseHTTPRequestHandler):
    db_path = ""

    def log_message(self, fmt, *args):
        print(f"[codesize-detail] {self.address_string()} {fmt % args}",
              file=sys.stderr)

    def send_html(self, title: str, body: str, status: int = 200) -> None:
        page = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{h(title)}</title>
  <style>
    body {{ font: 14px/1.4 system-ui, sans-serif; margin: 24px; color: #111; }}
    header {{ display: flex; gap: 16px; align-items: baseline; margin-bottom: 18px; }}
    a {{ color: #0645ad; text-decoration: none; }}
    a:hover {{ text-decoration: underline; }}
    table {{ border-collapse: collapse; width: 100%; margin: 12px 0 24px; }}
    th, td {{ border-bottom: 1px solid #ddd; padding: 6px 8px; text-align: left; }}
    th {{ background: #f6f6f6; position: sticky; top: 0; }}
    td.num, th.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
    .pos {{ color: #a40000; }}
    .neg {{ color: #006400; }}
    .muted {{ color: #666; }}
    form {{ display: flex; gap: 8px; flex-wrap: wrap; margin: 12px 0 18px; }}
    input, select, button {{ font: inherit; padding: 4px 6px; }}
    code {{ background: #f6f6f6; padding: 1px 3px; border-radius: 3px; }}
  </style>
</head>
<body>
  <header><h1>{h(title)}</h1><a href="/">runs</a></header>
  {body}
</body>
</html>"""
        data = page.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        try:
            if parsed.path == "/":
                self.handle_index()
            elif parsed.path == "/compare":
                self.handle_compare(query)
            elif parsed.path == "/file":
                self.handle_file(query)
            else:
                self.send_html("Not found", "<p>Not found.</p>", 404)
        except sqlite3.Error as e:
            self.send_html("SQLite error", f"<p><code>{h(e)}</code></p>", 500)
        except ValueError as e:
            self.send_html("Bad request", f"<p>{h(e)}</p>", 400)

    def handle_index(self) -> None:
        with connect(self.db_path) as conn:
            rows = conn.execute(
                """SELECT r.run_id, r.commit_sha, r.parent_sha, r.subject,
                          r.commit_ts, r.host, r.branch,
                          COUNT(cf.function) AS detail_rows
                   FROM runs r
                   LEFT JOIN codesize_func cf USING(run_id)
                   GROUP BY r.run_id
                   ORDER BY r.commit_ts DESC, r.run_ts DESC
                   LIMIT 100""").fetchall()
        body = ["<p class=\"muted\">Latest per-function code-size runs.</p>",
                "<table><thead><tr><th>commit</th><th>subject</th>"
                "<th>branch</th><th>host</th><th class=\"num\">rows</th>"
                "<th>view</th></tr></thead><tbody>"]
        for row in rows:
            link = "/compare?" + qs({"run": row["run_id"], "opt": "o2"})
            body.append(
                "<tr>"
                f"<td><code>{h(short_sha(row['commit_sha']))}</code></td>"
                f"<td>{h(row['subject'])}</td>"
                f"<td>{h(row['branch'])}</td>"
                f"<td>{h(row['host'])}</td>"
                f"<td class=\"num\">{row['detail_rows']}</td>"
                f"<td><a href=\"{h(link)}\">compare</a></td>"
                "</tr>")
        body.append("</tbody></table>")
        self.send_html("Code-Size Detail", "\n".join(body))

    def selected_run(self, conn: sqlite3.Connection, run_value: str | None):
        if run_value:
            if run_value.isdigit():
                row = conn.execute("SELECT * FROM runs WHERE run_id=?",
                                   (int(run_value),)).fetchone()
            else:
                row = conn.execute(
                    "SELECT * FROM runs WHERE commit_sha LIKE ? ORDER BY run_ts DESC LIMIT 1",
                    (run_value + "%",)).fetchone()
        else:
            row = conn.execute(
                "SELECT * FROM runs ORDER BY commit_ts DESC, run_ts DESC LIMIT 1").fetchone()
        if row is None:
            raise ValueError("run not found")
        return row

    def parent_run_id(self, conn: sqlite3.Connection, row) -> int | None:
        if not row["parent_sha"]:
            return None
        parent = conn.execute(
            "SELECT run_id FROM runs WHERE commit_sha=? AND host=?",
            (row["parent_sha"], row["host"])).fetchone()
        return parent["run_id"] if parent else None

    def handle_compare(self, query: dict) -> None:
        opt = query.get("opt", ["o2"])[0]
        limit = int(query.get("limit", ["200"])[0])
        search = query.get("q", [""])[0].strip()
        run_arg = query.get("run", [None])[0]
        if opt not in {"o0", "o1", "o2"}:
            raise ValueError("opt must be o0, o1, or o2")
        limit = max(1, min(limit, 1000))

        with connect(self.db_path) as conn:
            run = self.selected_run(conn, run_arg)
            parent_id = self.parent_run_id(conn, run)
            params = [run["run_id"], opt]
            where = ""
            if search:
                where = "AND (c.suite LIKE ? OR c.test LIKE ?)"
                like = f"%{search}%"
                params.extend([like, like])
            parent_join = ""
            parent_cols = "NULL AS parent_tcc, NULL AS parent_gcc, NULL AS delta"
            if parent_id is not None:
                parent_join = """
                   LEFT JOIN (
                     SELECT suite, test, SUM(tcc_size) AS tcc_size,
                            SUM(gcc_size) AS gcc_size
                     FROM codesize_func
                     WHERE run_id=? AND opt=?
                     GROUP BY suite, test
                   ) p ON p.suite = c.suite AND p.test = c.test"""
                parent_cols = ("p.tcc_size AS parent_tcc, p.gcc_size AS parent_gcc, "
                               "(c.tcc_size - p.tcc_size) AS delta")
                params = [run["run_id"], opt, parent_id, opt] + params[2:]
            order_expr = ("ABS(COALESCE(c.tcc_size - p.tcc_size, c.tcc_size))"
                          if parent_id is not None else "c.tcc_size")
            rows = conn.execute(
                f"""SELECT c.suite, c.test, c.funcs, c.tcc_size, c.gcc_size,
                           {parent_cols},
                           CASE WHEN c.gcc_size > 0
                                THEN 1.0 * c.tcc_size / c.gcc_size ELSE 0 END AS ratio
                    FROM (
                      SELECT suite, test, COUNT(*) AS funcs,
                             SUM(tcc_size) AS tcc_size, SUM(gcc_size) AS gcc_size
                      FROM codesize_func
                      WHERE run_id=? AND opt=?
                      GROUP BY suite, test
                    ) c
                    {parent_join}
                    WHERE 1=1 {where}
                    ORDER BY {order_expr} DESC, c.suite, c.test
                    LIMIT ?""",
                (*params, limit)).fetchall()

        body = [
            f"<p><code>{h(short_sha(run['commit_sha']))}</code> {h(run['subject'])}</p>",
            "<form method=\"get\" action=\"/compare\">",
            f"<input type=\"hidden\" name=\"run\" value=\"{h(run['run_id'])}\">",
            "<label>opt <select name=\"opt\">"
            + "".join(f"<option value=\"{o}\"{' selected' if o == opt else ''}>{o}</option>"
                      for o in ("o0", "o1", "o2"))
            + "</select></label>",
            f"<label>filter <input name=\"q\" value=\"{h(search)}\" placeholder=\"suite or file\"></label>",
            f"<label>limit <input name=\"limit\" type=\"number\" min=\"1\" max=\"1000\" value=\"{limit}\"></label>",
            "<button>apply</button></form>",
        ]
        if parent_id is None:
            body.append("<p class=\"muted\">Parent run is not present; showing current totals only.</p>")
        body.append("<table><thead><tr><th>suite</th><th>file</th>"
                    "<th class=\"num\">funcs</th><th class=\"num\">tcc</th>"
                    "<th class=\"num\">parent</th><th class=\"num\">delta</th>"
                    "<th class=\"num\">gcc</th><th class=\"num\">ratio</th>"
                    "<th>detail</th></tr></thead><tbody>")
        for row in rows:
            delta = row["delta"]
            cls = "pos" if delta and delta > 0 else "neg" if delta and delta < 0 else ""
            file_link = "/file?" + qs({
                "run": run["run_id"], "opt": opt,
                "suite": row["suite"], "test": row["test"],
            })
            body.append(
                "<tr>"
                f"<td>{h(row['suite'])}</td><td>{h(row['test'])}</td>"
                f"<td class=\"num\">{row['funcs']}</td>"
                f"<td class=\"num\">{row['tcc_size']}</td>"
                f"<td class=\"num\">{h(row['parent_tcc'])}</td>"
                f"<td class=\"num {cls}\">{h(delta)}</td>"
                f"<td class=\"num\">{row['gcc_size']}</td>"
                f"<td class=\"num\">{row['ratio']:.3f}</td>"
                f"<td><a href=\"{h(file_link)}\">functions</a></td>"
                "</tr>")
        body.append("</tbody></table>")
        self.send_html("File Comparison", "\n".join(body))

    def handle_file(self, query: dict) -> None:
        opt = query.get("opt", ["o2"])[0]
        run_arg = query.get("run", [None])[0]
        suite = unquote(query.get("suite", [""])[0])
        test = unquote(query.get("test", [""])[0])
        if not suite or not test:
            raise ValueError("suite and test are required")
        with connect(self.db_path) as conn:
            run = self.selected_run(conn, run_arg)
            parent_id = self.parent_run_id(conn, run)
            params = [run["run_id"], opt, suite, test]
            parent_join = ""
            parent_cols = "NULL AS parent_tcc, NULL AS parent_gcc, NULL AS delta"
            if parent_id is not None:
                parent_join = """
                  LEFT JOIN codesize_func p
                    ON p.run_id=? AND p.opt=c.opt AND p.suite=c.suite
                   AND p.test=c.test AND p.function=c.function"""
                parent_cols = ("p.tcc_size AS parent_tcc, p.gcc_size AS parent_gcc, "
                               "(c.tcc_size - p.tcc_size) AS delta")
                params = [parent_id, run["run_id"], opt, suite, test]
            order_expr = ("ABS(COALESCE(c.tcc_size - p.tcc_size, c.tcc_size))"
                          if parent_id is not None else "c.tcc_size")
            rows = conn.execute(
                f"""SELECT c.function, c.tcc_size, c.gcc_size, c.ratio,
                           {parent_cols}
                    FROM codesize_func c
                    {parent_join}
                    WHERE c.run_id=? AND c.opt=? AND c.suite=? AND c.test=?
                    ORDER BY {order_expr} DESC, c.function""",
                params).fetchall()

        back = "/compare?" + qs({"run": run["run_id"], "opt": opt})
        body = [f"<p><a href=\"{h(back)}\">back to files</a></p>",
                f"<p><code>{h(short_sha(run['commit_sha']))}</code> "
                f"{h(suite)}/{h(test)} at {h(opt)}</p>",
                "<table><thead><tr><th>function</th><th class=\"num\">tcc</th>"
                "<th class=\"num\">parent</th><th class=\"num\">delta</th>"
                "<th class=\"num\">gcc</th><th class=\"num\">ratio</th>"
                "</tr></thead><tbody>"]
        for row in rows:
            delta = row["delta"]
            cls = "pos" if delta and delta > 0 else "neg" if delta and delta < 0 else ""
            body.append(
                "<tr>"
                f"<td>{h(row['function'])}</td>"
                f"<td class=\"num\">{row['tcc_size']}</td>"
                f"<td class=\"num\">{h(row['parent_tcc'])}</td>"
                f"<td class=\"num {cls}\">{h(delta)}</td>"
                f"<td class=\"num\">{row['gcc_size']}</td>"
                f"<td class=\"num\">{row['ratio']:.3f}</td>"
                "</tr>")
        body.append("</tbody></table>")
        self.send_html("Function Comparison", "\n".join(body))


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--db", default="/var/lib/tcc-metrics/codesize-detail.db")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8008)
    args = p.parse_args(argv)

    CodesizeHandler.db_path = args.db
    server = ThreadingHTTPServer((args.host, args.port), CodesizeHandler)
    print(f"serving {args.db} on http://{args.host}:{args.port}",
          file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
