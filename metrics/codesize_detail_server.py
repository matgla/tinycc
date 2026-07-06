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
import os
import sqlite3
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse

OPTS = {"o0", "o1", "o2"}
CHANGE_FILTERS = {"all", "regressions", "improvements"}
SORT_DIRS = {"asc", "desc"}


def h(text) -> str:
    return html.escape("" if text is None else str(text), quote=True)


def qs(params: dict) -> str:
    return "&".join(f"{quote(str(k))}={quote(str(v))}" for k, v in params.items())


def short_sha(sha: str) -> str:
    return sha[:12] if sha else "?"


def query_one(query: dict, key: str, default: str) -> str:
    return query.get(key, [default])[0]


def sort_dir(query: dict) -> str:
    value = query_one(query, "dir", "desc").lower()
    if value not in SORT_DIRS:
        raise ValueError("dir must be asc or desc")
    return value


def change_filter(query: dict) -> str:
    value = query_one(query, "change", "all").lower()
    if value not in CHANGE_FILTERS:
        raise ValueError("change must be all, regressions, or improvements")
    return value


def order_clause(sort: str, direction: str, sort_map: dict,
                 default_sort: str) -> str:
    if sort not in sort_map:
        raise ValueError("unknown sort column")
    primary = sort_map[sort]
    tie = sort_map[default_sort]
    if sort == default_sort:
        return f"{primary} {direction.upper()}, c.suite, c.test"
    return f"{primary} {direction.upper()}, {tie} DESC, c.suite, c.test"


def sort_link(label: str, path: str, params: dict, key: str,
              current_sort: str, current_dir: str) -> str:
    next_dir = "asc"
    marker = ""
    if current_sort == key:
        next_dir = "desc" if current_dir == "asc" else "asc"
        marker = " ^" if current_dir == "asc" else " v"
    link_params = dict(params)
    link_params["sort"] = key
    link_params["dir"] = next_dir
    return f"<a href=\"{h(path + '?' + qs(link_params))}\">{h(label + marker)}</a>"


class DatabaseOpenError(sqlite3.Error):
    pass


def _display_path(db_path: str) -> Path:
    path = Path(db_path).expanduser()
    if not path.is_absolute():
        path = path.resolve()
    return path


def _readonly_uri(path: Path) -> str:
    return path.as_uri() + "?mode=ro"


def connect(db_path: str) -> sqlite3.Connection:
    path = _display_path(db_path)
    try:
        path.stat()
    except FileNotFoundError as e:
        raise DatabaseOpenError(
            f"detail database does not exist: {path}. Create it with "
            f"`sqlite3 {path} < metrics/schema.sql` or populate it with "
            f"`metrics/record.py --codesize-detail --detail-db {path}`."
        ) from e
    except OSError as e:
        raise DatabaseOpenError(
            f"cannot stat detail database {path}: {e}. Check parent directory "
            "execute permissions and service user access."
        ) from e
    if not os.path.isfile(path):
        raise DatabaseOpenError(f"detail database path is not a regular file: {path}")
    if not os.access(path, os.R_OK):
        raise DatabaseOpenError(
            f"detail database is not readable: {path}. Check file ownership and "
            "service user access."
        )
    try:
        conn = sqlite3.connect(_readonly_uri(path), uri=True)
    except sqlite3.Error as e:
        raise DatabaseOpenError(
            f"cannot open detail database {path}: {e}. Check that the file is a "
            "valid SQLite database and that its directory is readable."
        ) from e
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
        opt = query_one(query, "opt", "o2")
        limit = int(query_one(query, "limit", "200"))
        search = query_one(query, "q", "").strip()
        run_arg = query.get("run", [None])[0]
        change = change_filter(query)
        sort = query_one(query, "sort", "abs_delta")
        direction = sort_dir(query)
        if opt not in OPTS:
            raise ValueError("opt must be o0, o1, or o2")
        limit = max(1, min(limit, 1000))

        with connect(self.db_path) as conn:
            run = self.selected_run(conn, run_arg)
            parent_id = self.parent_run_id(conn, run)
            params = [run["run_id"], opt]
            where_clauses = []
            if search:
                where_clauses.append("(c.suite LIKE ? OR c.test LIKE ?)")
                like = f"%{search}%"
                params.extend([like, like])
            parent_join = ""
            parent_cols = "NULL AS parent_tcc, NULL AS parent_gcc, NULL AS delta"
            delta_expr = "c.tcc_size"
            compare_sort_map = {
                "suite": "c.suite",
                "file": "c.test",
                "funcs": "c.funcs",
                "tcc": "c.tcc_size",
                "parent": "0",
                "delta": "c.tcc_size",
                "abs_delta": "c.tcc_size",
                "gcc": "c.gcc_size",
                "ratio": "ratio",
            }
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
                               "(c.tcc_size - COALESCE(p.tcc_size, 0)) AS delta")
                params = [run["run_id"], opt, parent_id, opt] + params[2:]
                delta_expr = "(c.tcc_size - COALESCE(p.tcc_size, 0))"
                compare_sort_map.update({
                    "parent": "p.tcc_size",
                    "delta": delta_expr,
                    "abs_delta": f"ABS({delta_expr})",
                })
                if change == "regressions":
                    where_clauses.append(f"{delta_expr} > 0")
                elif change == "improvements":
                    where_clauses.append(f"{delta_expr} < 0")
            elif change != "all":
                where_clauses.append("0")
            where = " AND ".join(where_clauses)
            if where:
                where = "AND " + where
            order = order_clause(sort, direction, compare_sort_map, "abs_delta")
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
                    ORDER BY {order}
                    LIMIT ?""",
                (*params, limit)).fetchall()

        base_params = {
            "run": run["run_id"],
            "opt": opt,
            "q": search,
            "limit": limit,
            "change": change,
            "sort": sort,
            "dir": direction,
        }
        body = [
            f"<p><code>{h(short_sha(run['commit_sha']))}</code> {h(run['subject'])}</p>",
            "<form method=\"get\" action=\"/compare\">",
            f"<input type=\"hidden\" name=\"run\" value=\"{h(run['run_id'])}\">",
            f"<input type=\"hidden\" name=\"sort\" value=\"{h(sort)}\">",
            f"<input type=\"hidden\" name=\"dir\" value=\"{h(direction)}\">",
            "<label>opt <select name=\"opt\">"
            + "".join(f"<option value=\"{o}\"{' selected' if o == opt else ''}>{o}</option>"
                      for o in ("o0", "o1", "o2"))
            + "</select></label>",
            "<label>change <select name=\"change\">"
            + "".join(
                f"<option value=\"{value}\"{' selected' if value == change else ''}>{label}</option>"
                for value, label in (
                    ("all", "all"),
                    ("regressions", "regressions"),
                    ("improvements", "improvements"),
                ))
            + "</select></label>",
            f"<label>filter <input name=\"q\" value=\"{h(search)}\" placeholder=\"suite or file\"></label>",
            f"<label>limit <input name=\"limit\" type=\"number\" min=\"1\" max=\"1000\" value=\"{limit}\"></label>",
            "<button>apply</button></form>",
        ]
        if parent_id is None:
            if change == "all":
                body.append("<p class=\"muted\">Parent run is not present; showing current totals only.</p>")
            else:
                body.append("<p class=\"muted\">Parent run is not present; change filters cannot match.</p>")
        body.append("<table><thead><tr>"
                    f"<th>{sort_link('suite', '/compare', base_params, 'suite', sort, direction)}</th>"
                    f"<th>{sort_link('file', '/compare', base_params, 'file', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('funcs', '/compare', base_params, 'funcs', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('tcc', '/compare', base_params, 'tcc', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('parent', '/compare', base_params, 'parent', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('delta', '/compare', base_params, 'delta', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('gcc', '/compare', base_params, 'gcc', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('ratio', '/compare', base_params, 'ratio', sort, direction)}</th>"
                    "<th>detail</th></tr></thead><tbody>")
        for row in rows:
            delta = row["delta"]
            cls = "pos" if delta and delta > 0 else "neg" if delta and delta < 0 else ""
            file_link = "/file?" + qs({
                "run": run["run_id"], "opt": opt,
                "suite": row["suite"], "test": row["test"],
                "change": change,
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
        opt = query_one(query, "opt", "o2")
        run_arg = query.get("run", [None])[0]
        change = change_filter(query)
        sort = query_one(query, "sort", "abs_delta")
        direction = sort_dir(query)
        suite = unquote(query.get("suite", [""])[0])
        test = unquote(query.get("test", [""])[0])
        if not suite or not test:
            raise ValueError("suite and test are required")
        if opt not in OPTS:
            raise ValueError("opt must be o0, o1, or o2")
        with connect(self.db_path) as conn:
            run = self.selected_run(conn, run_arg)
            parent_id = self.parent_run_id(conn, run)
            params = [run["run_id"], opt, suite, test]
            parent_join = ""
            parent_cols = "NULL AS parent_tcc, NULL AS parent_gcc, NULL AS delta"
            delta_expr = "c.tcc_size"
            file_sort_map = {
                "function": "c.function",
                "tcc": "c.tcc_size",
                "parent": "0",
                "delta": "c.tcc_size",
                "abs_delta": "c.tcc_size",
                "gcc": "c.gcc_size",
                "ratio": "c.ratio",
            }
            where_clauses = []
            if parent_id is not None:
                parent_join = """
                  LEFT JOIN codesize_func p
                    ON p.run_id=? AND p.opt=c.opt AND p.suite=c.suite
                   AND p.test=c.test AND p.function=c.function"""
                parent_cols = ("p.tcc_size AS parent_tcc, p.gcc_size AS parent_gcc, "
                               "(c.tcc_size - COALESCE(p.tcc_size, 0)) AS delta")
                params = [parent_id, run["run_id"], opt, suite, test]
                delta_expr = "(c.tcc_size - COALESCE(p.tcc_size, 0))"
                file_sort_map.update({
                    "parent": "p.tcc_size",
                    "delta": delta_expr,
                    "abs_delta": f"ABS({delta_expr})",
                })
                if change == "regressions":
                    where_clauses.append(f"{delta_expr} > 0")
                elif change == "improvements":
                    where_clauses.append(f"{delta_expr} < 0")
            elif change != "all":
                where_clauses.append("0")
            if sort not in file_sort_map:
                raise ValueError("unknown sort column")
            where = " AND ".join(where_clauses)
            if where:
                where = "AND " + where
            primary_order = file_sort_map[sort]
            tie_order = file_sort_map["abs_delta"]
            if sort == "abs_delta":
                order = f"{primary_order} {direction.upper()}, c.function"
            else:
                order = f"{primary_order} {direction.upper()}, {tie_order} DESC, c.function"
            rows = conn.execute(
                f"""SELECT c.function, c.tcc_size, c.gcc_size, c.ratio,
                           {parent_cols}
                    FROM codesize_func c
                    {parent_join}
                    WHERE c.run_id=? AND c.opt=? AND c.suite=? AND c.test=? {where}
                    ORDER BY {order}""",
                params).fetchall()

        back = "/compare?" + qs({"run": run["run_id"], "opt": opt, "change": change})
        base_params = {
            "run": run["run_id"],
            "opt": opt,
            "suite": suite,
            "test": test,
            "change": change,
            "sort": sort,
            "dir": direction,
        }
        body = [f"<p><a href=\"{h(back)}\">back to files</a></p>",
                f"<p><code>{h(short_sha(run['commit_sha']))}</code> "
                f"{h(suite)}/{h(test)} at {h(opt)}</p>",
                "<form method=\"get\" action=\"/file\">",
                f"<input type=\"hidden\" name=\"run\" value=\"{h(run['run_id'])}\">",
                f"<input type=\"hidden\" name=\"opt\" value=\"{h(opt)}\">",
                f"<input type=\"hidden\" name=\"suite\" value=\"{h(suite)}\">",
                f"<input type=\"hidden\" name=\"test\" value=\"{h(test)}\">",
                f"<input type=\"hidden\" name=\"sort\" value=\"{h(sort)}\">",
                f"<input type=\"hidden\" name=\"dir\" value=\"{h(direction)}\">",
                "<label>change <select name=\"change\">"
                + "".join(
                    f"<option value=\"{value}\"{' selected' if value == change else ''}>{label}</option>"
                    for value, label in (
                        ("all", "all"),
                        ("regressions", "regressions"),
                        ("improvements", "improvements"),
                    ))
                + "</select></label>",
                "<button>apply</button></form>"]
        if parent_id is None:
            if change == "all":
                body.append("<p class=\"muted\">Parent run is not present; showing current totals only.</p>")
            else:
                body.append("<p class=\"muted\">Parent run is not present; change filters cannot match.</p>")
        body.append("<table><thead><tr>"
                    f"<th>{sort_link('function', '/file', base_params, 'function', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('tcc', '/file', base_params, 'tcc', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('parent', '/file', base_params, 'parent', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('delta', '/file', base_params, 'delta', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('gcc', '/file', base_params, 'gcc', sort, direction)}</th>"
                    f"<th class=\"num\">{sort_link('ratio', '/file', base_params, 'ratio', sort, direction)}</th>"
                    "</tr></thead><tbody>")
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
